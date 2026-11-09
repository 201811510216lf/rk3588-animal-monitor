# 数据流设计（data_flow）

> 基于任务书第 5 节与审计结论。默认模式 composite：双路采集 → 合成 → 单路编码推流。

## 1. 默认链路（composite）

```text
MIPI Camera 0 → Sensor/ISP(BSP 驱动) → /dev/video0 → V4L2 DMABUF 池 (Camera0 Capture Thread)
                                                          │ ready0 (BoundedQueue, cap, drop_oldest)
MIPI Camera 1 → Sensor/ISP(BSP 驱动) → /dev/video1 → V4L2 DMABUF 池 (Camera1 Capture Thread)
                                                          │ ready1 (BoundedQueue, cap, drop_oldest)
                                                          ▼
                              FrameSynchronizer (TimestampAligner: 最近邻 + 容差阈值)
                                                          │ 对齐对 {frame0, frame1}
                                                          ▼
                              RGA FrameCompositor (side-by-side 合成到合成池)
                                                          │ 合成帧 IO_FD (引用计数持有输入帧)
                                                          ▼
                              MPP Encoder Input (encode_put_frame, 阻塞=已消费)
                                                          │
                              MPP Encoder Output (get_packet, Annex-B AU 聚合)
                                                          │
                              ZLMediaKit Publisher (RTSP 默认; WebRTC/RTMP 可选)
```

规则：
- 编码与推理解耦：推流 25/30fps，推理 5~10fps 独立采样（配置化，实测决定）。
- 合成前统一像素格式/尺寸/stride/色彩空间；**禁止忽略 stride**。
- 双路时间戳对齐：最近邻 + 最大容差（sync_tolerance_ms）；超阈值标记不同步并丢旧帧，
  指标记录 align_error / desync_count。
- 共享帧引用计数；RGA/推理未完成前禁止 QBUF/回收相机缓冲。

## 2. 可选模式

| 模式 | 数据流 | 启用条件 |
|---|---|---|
| composite（默认） | 如上 | composition.mode=composite |
| independent | 两路各自独立编码与 stream path（camera0→enc0→live/camera0；camera1→enc1→live/camera1） | composition.mode=independent |
| single | 单路输入降级运行（仅 camera0） | 一路不可用或 composition.mode=single |

## 3. 推理支路（与推流解耦）

```text
Camera N 帧池 ──(引用/或独立采样队列, 有界)──> InferenceScheduler (round_robin/priority/per_camera)
                                                  │
        ┌──────────────┬──────────────┬───────────────┐
   GroomingModel  FeedingModel  RearingModel   (IInferenceModel 接口)
        └──────────────┴──────────────┴───────────────┘
                                                  │ detect_result{frame_id,camera_id,
                                                  │   monotonic_ns, model_id, model_version}
        ┌─────────────────────────────────────────┤
        ▼                                         ▼
   FusionService (行为-温度融合、个体基线、异常检测)    StreamCompositor (画框叠加，可选)
                                                  │
        telemetry/storage/network (MQTT 事件、SQLite、补传)
```

- 同一帧禁止重复推理；结果带有效期；模型缺失时应用必须明确报错并允许无 AI 启动。
- 结果结构必须含 frame_id/camera_id/timestamp_monotonic_ns/model_id/model_version。

## 4. 传感器支路

```text
MLX90640 → 内核 I2C 驱动(cdev 或 V4L2, 待评估) → /dev/thermal0
    → ThermalService (32×24 帧 + frame_id + 状态位) → ThermalFeatureExtractor
                                                        │
HTU21D → 内核 IIO 驱动 → IIO sysfs/buffer → EnvironmentService
                                                        │
    CalibrationService (RGB↔热场坐标映射、ROI) → FusionService → 事件/告警
```

- 温度单位/状态位/UAPI 结构由驱动文档定义（32/64 位兼容、ABI 版本）。
- 环境温湿度仅作模型输入/补偿特征，不宣称单靠 HTU21D 修正 MLX90640 全部误差。
- 校准系数必须来自参考测温设备实测，禁止硬编码"准确体温"。

## 5. 通信与存储

```text
视频：ZLM(内嵌) RTSP 8554 / WebRTC 8001 / RTMP 可选；H.264 默认，H.265 可选
结构化：MQTT(遥测/事件/告警) + HTTP/REST(配置/诊断) + WebSocket(实时)
本地：SQLite 事件元数据 + 有界离线队列(优先级: 告警>结构化>视频片段) + 断网补传
OTA：SHA256+签名校验、版本检查、A/B 分区回滚（以真实 bootloader 为准）
```

## 6. 已实现部分（Phase 1）

- 队列边：BoundedQueue（容量/满策略/超时/关停/指标）。
- 对齐器：TimestampAligner（最近邻+容差，纯逻辑已单测）。
- MOCK 流水线：MockPipelineService 以合成 FrameMeta 走通"生产者→有界队列→消费者→指标"接线
  （明确标注 MOCK，不代表真实视频能力）。
