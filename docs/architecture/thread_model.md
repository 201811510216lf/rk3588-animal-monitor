# 线程模型（thread_model）

> 目标线程划分（任务书 §5.2）+ 每条队列边的完整契约。Phase 1 已实现基础设施；
> 线程本身随 Phase 2+ 落地。

## 1. 线程清单

| 线程 | 职责 | 输入边 | 输出边 |
|---|---|---|---|
| camera0_capture | V4L2 poll+DQBUF（DMABUF），时间戳标记 | 驱动 | ready0 |
| camera1_capture | 同上（camera1） | 驱动 | ready1 |
| frame_sync | 双路时间戳对齐（最近邻+容差） | ready0, ready1 | composed_in |
| rga_compose | RGA 合成 side-by-side → 合成池 | composed_in | enc_in |
| mpp_enc_input | encode_put_frame（阻塞=消费完成，立即归还合成池引用） | enc_in | （回收到合成池） |
| mpp_enc_output | get_packet → AU 聚合 → Publisher | 编码器 | publisher_in |
| stream_publisher | ZLM RTSP/WebRTC 喂流（逐 NALU + 时间戳单调化） | publisher_in | 网络 |
| inference_scheduler | 采样帧 → 路由到模型 | 帧池引用队列 | per-model queues |
| inference_worker ×N | RKNN 推理（并发策略 serial/shared/multi-context，实测决定） | per-model queues | results |
| thermal_worker | MLX90640 帧采集（cdev/V4L2） | 驱动 | thermal queue |
| env_worker | HTU21D IIO 周期采样 | IIO | env queue |
| fusion_worker | 行为-温度融合、个体基线、异常检测 | results, thermal, env | events |
| telemetry_worker | MQTT/HTTP/WS 上行与命令处理 | events/telemetry | 网络 |
| storage_worker | SQLite 落库、离线队列、补传 | events/clips | 磁盘/网络 |
| health_worker | 设备/磁盘/温度/看门狗巡检 | — | alerts/watchdog |

约束：
- 每个线程只消费/生产明确声明的队列边；禁止跨线程直接访问他人状态（除原子量）。
- 主线程（Application::Run）只做信号等待与启停；main 只做参数解析。

## 2. 队列边契约（每条边必须定义）

| 队列 | 容量（默认） | 满策略 | 超时 | 关停行为 | 指标 |
|---|---|---|---|---|---|
| ready0/ready1 | 8 | drop_oldest（保留新鲜帧） | push 50ms | Close→唤醒、push 失败、pop 排空 | 深度/丢帧/超时 |
| composed_in | 8 | drop_oldest | push 50ms | 同上 | 深度/丢帧/不同步计数 |
| enc_in | 4 | drop_oldest | push 100ms | 同上 | 深度/丢帧 |
| publisher_in | 8 | drop_oldest | push 100ms | 同上 | 深度/丢帧/断流计数 |
| per-model queues | 2（最新优先） | drop_oldest | 无阻塞 | Close 唤醒 | 深度/过期结果数 |
| results | 64 | drop_oldest | push 50ms | 同上 | 深度/丢结果 |
| thermal/env | 8 | drop_oldest | push 100ms | 同上 | 深度/传感器错误计数 |
| events | 64 | drop_oldest | push 100ms | 同上 | 深度/丢事件 |

说明：默认容量取自 configs/default.yaml（queues.*），实际值以目标板实测为准（UNVERIFIED）。
视频边一律优先保留新鲜帧；推理结果带有效期（过期即弃）。

## 3. 优雅关停协议（确定性）

1. 信号 → Application::RequestShutdown → 各服务 Stop()（**逆依赖顺序**：
   先停 producer 上游，再停 consumer 下游）。
2. 每线程：置 stop 标志 → 唤醒所有条件变量/队列 Close → 队列排空或取消 →
   释放下游引用 → 归还硬件缓冲 → 销毁硬件上下文（RAII 析构兜底）。
3. 禁止的模式（审计教训）：detach 线程永不停止、对运行中对象 delete、joinable 线程
   不 join 直接退出、退出阶段不还缓冲。
4. 单路故障降级：camera1 掉线 → 自动切 single 模式继续推流（Phase 2 实现），
   不因单路故障全链路停机。

## 4. 已实现（Phase 1）

- BoundedQueue（全部边契约所需语义：容量/策略/超时/Close/指标）→ 单测通过。
- MockPipelineService：producer/consumer 两线程走有界队列 + Close 关停 → host 实测
  produced=33/consumed=33/dropped=0/queue_left=0。
- Application：信号处理（Windows Ctrl+C / POSIX SIGINT+SIGTERM）+ 逆序停止。
