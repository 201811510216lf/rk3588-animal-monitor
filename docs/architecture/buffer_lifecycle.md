# 缓冲区生命周期与所有权（buffer_lifecycle）

> DMA-BUF/MPP/RGA/V4L2 的回收协议。协议模板取自参考仓库（经审计确认的零拷贝实现），
> 并修正其缺陷；目标板 fence/cache 行为 UNVERIFIED，须按实际 SDK 文档与实测确认。

## 1. 所有权总原则

1. 任何 fd、mmap、MPP buffer、RGA handle 必须 RAII 封装；**禁止裸 owning 指针**。
2. "fd 在队列里" ≠ "底层缓冲可回收"：回收以**下游显式归还**为准，不以队列为界。
3. 消费者未完成前，生产侧禁止 QBUF / mpp_buffer_put / 覆写。
4. 每条边明确"谁持有、何时归还、归还给谁"；归还函数必须幂等。
5. 退出/错误路径必须归还全部在飞缓冲（drain 流程）。

## 2. 相机缓冲池（V4L2 DMABUF 注入，模板：参考仓库 v4l2Camera）

```text
dma-heap 分配(16 块) → mmap → REQBUFS(16, V4L2_MEMORY_DMABUF) → 逐 index QBUF(带 fd)
        │ 驱动按 index 写入（MJPEG/NV12，具体格式以 BSP 为准）
        ▼ DQBUF（时间戳取驱动，兜底 MONOTONIC）
  capture 线程：FrameDesc{index,fd,base,pts} → readyQueue
        ▼ 解码线程：MppDecode（同 fd import，零拷贝）
        ▼ 解码完成后 FrameDesc → recycleQueue → capture 线程 → QBUF
```

规则：
- 相机 buffer 只有在**解码（或跳过路径）完成**后才能回队；丢帧也要先回队（审计教训）。
- RGA 合成时输入两帧按引用计数持有；对齐器丢弃旧帧时立即释放其引用。

## 3. MPP 解码输出（external-buffer 模式三 + holder 协议）

```text
info-change → mpp_buffer_group_get_external(EXT_DMA) → limit_config(48) →
  commit(dma-heap fd) → MPP_DEC_SET_EXT_BUF_GROUP → SET_INFO_CHANGE_READY
  输出帧 → DecodedTaskHolder{output_desc, output_frame(MppFrame), output_buf(MppBuffer)}
        │ holder 保证底层句柄在 RGA 消费完成前不被归还
        ▼ RGA 完成后 → PendingRecycleQueue → 下一次 MppDecode 开头统一
          mpp_frame_deinit + mpp_buffer_put（延迟一轮，避免与当前解码复用冲突）
```

## 4. RGA 输出/合成池

- 惰性建池（首帧几何决定，48 块 dma-heap + mmap，NV12）；几何变化时校验并重建。
- 获取/归还：可用队列 + in_use 位图；归还幂等。
- RGA 输入输出：优先 `importbuffer_fd + wrapbuffer_handle`，失败回退虚拟地址路径。

## 5. MPP 编码输入消费判定

- `encode_put_frame` 为阻塞语义：**返回即视为编码器已消费输入帧**，立即归还上游池。
- RGA 输出 fd 在编码器侧 import 一次、会话期常驻，归还后安全复写。
- 编码输出：get_packet 的 MppPacket 由编码输出线程持有，AU 聚合完成后 ReleasePacket。

## 6. 推理侧缓冲

- 阶段策略（Phase 3）：rknn_inputs_set pass_through=0 为内部拷贝（安全但费 CPU）；
  目标板评估 `rknn_set_io_mem`（输入帧须保活至 rknn_run 返回；输出 tensor 在
  rknn_outputs_release 前不可覆写）。
- 推理采样帧引用计数与推流共享；结果只带元数据，不持有像素缓冲。

## 7. 状态机（每帧）

```text
  ALLOC ──QBUF──▶ IN_DRIVER ──DQBUF──▶ CAPTURED
                                           │
                       ┌───────────────────┼────────────────────┐
                       ▼                   ▼                    ▼
                  DECODING           DROPPED(立即释放)      (对齐等待, refcnt++)
                       │                   │                    │
                       ▼                   └──▶ RELEASED ──▶ 池空闲
                  DECODED(refcnt=N 消费者)
                       │ 每个消费者完成 → refcnt--
                       ▼ refcnt==0
                  RECYCLED ──QBUF──▶ IN_DRIVER（相机缓冲复用）
```

MPP/RGA 句柄同理：持有者列表 + 幂等归还 + 关停时 drain 全部在飞项。

## 8. 错误路径与关停

- 超时/失败：逐级释放（解码失败仅回收相机缓冲并计数；RGA 失败重试有限次后丢帧计数；
  编码失败重试有限次后归还合成池）。
- 关停顺序：停 source → 队列 Close（排空）→ 各持有者归还 → 硬件上下文销毁
  （STREAMOFF → REQBUFS(0) → mpp_destroy → rga release → ZLM close）。
- 审计教训逐条落实：无 detach 永不停止的线程、无运行中 delete、无未 join 退出、
  mpp_ctx 唯一持有（无局部变量遮蔽）、info-change 失败必须上抛。

## 9. 状态

- 本协议为设计基线；Phase 2 在目标板上按"每环节硬件测试"逐项验证并回填实测结论。
- fence/cache sync 规则：UNVERIFIED（依赖 BSP/SDK 文档），禁止臆造。
