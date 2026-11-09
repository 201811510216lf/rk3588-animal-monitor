# 子审计报告 Ref：JoeChen2me/RK-MediaProject（存档）

> 只读审计（本机浅克隆 `.ref_audit_RKMediaProject`），2026-11-06。摘要版。

## 结构与风格
- src/ + include/ 分离；类：`V4L2_Camera`、`MppDecInstance`、`MppEncInstance`、`RgaInstance`、
  `ZlmPublisher`；常量 k 前缀 + 命名空间聚合（resource_limits/camera_params/mpp_enc_packet_result/mpp_common）。
- 方法：PascalCase 语义动词（camera_GlobalInit/DecInit/EncConfigWidthHeight/EncodePushFrame/
  InputPacketChunk）；私有成员尾下划线；0/-1 返回码、不抛异常、goto fail 回滚。
- .clang-format：Google 基底，4 空格/100 列/无 Tab/不排序 include/对齐赋值与声明/自定义大括号换行
  （本工程已原样采用）。

## 零拷贝协议（架构模板）
```
dma-heap×16 → V4L2_MEMORY_DMABUF 注入 → 相机写 MJPEG → DQBUF(→MppDecode 零拷贝输入)
  FrameDesc → readyQueue(有界12,丢最旧) → decode → rgaConsumeQueue → RGA imcopy
  解码输出(48池+holder) → mppRecycleOutputQueue → 回收线程 → PendingRecycleQueue(下一轮 decode 才 put)
  RGA 输出池(48) → mppEncodeInputQueue → encode_put_frame(阻塞=已消费) → 幂等归还
  encode_get_packet → AU 聚合 → ZLM mk_media_input_h264(逐 NALU)
```
- 相机池：`/dev/dma_heap/system` ioctl 分配 + mmap；REQBUFS(16, DMABUF) + QBUF 带 fd。
- 解码：external-buffer 模式三 + mpp_buffer_import 相机 fd（输入零拷贝）；DecodedTaskHolder
  持 MppFrame/MppBuffer 引用直到 RGA 消费完成。
- RGA：importbuffer_fd+wrapbuffer_handle（失败回退虚拟地址）；同格式 imcopy。
- 编码：CBR 2Mbps/GOP 60/High@L4.0/CABAC/split NONE/EACH_IDR 头模式；import RGA fd 常驻。

## 线程与退出
- 6 线程：capture / decode / mpp_output_recycle / rga / mpp_encode_input / mpp_encode_output；
  队列 mutex+cv+deque；唯一有界队列 readyQueue（上限 12，丢最旧并回收）。
- 优雅退出：SIGINT → request_stop+notify_all → join 按依赖顺序 → 残留 drain → ZLM Close → 队列清空；
  栈对象析构兜底。编码器不推 EOS，退出靠 stop 标志+队列耗尽。

## ZLM 集成
- mk_env_init → http(8000)/rtsp(8554)/rtc(8001) → mk_media_create2(live/camera, 仅 rtsp) →
  mk_track H264 → init_complete；自实现 `/index/api/webrtc?type=play`（mk_webrtc_get_answer_sdp）。
- 时间戳：us → 强制单调(dts>last、pts≥dts) → ms 再单调 → mk_media_input_h264(dts_ms/pts_ms)；
  fallback 计数。mk_pusher 未用（内嵌 server 发布，非推流到外部）。

## 对新建项目的借鉴
沿用：硬件模块一对一类封装、固定容量池+幂等回收、holder 延迟归还、队列锁内摘取锁外 I/O、
依赖顺序退出、ZlmPublishConfig 参数化。
改进：配置硬编码（设备/分辨率/码率/端口）全部参数化；单相机假设 → Channel 抽象；
命名统一；SIGTERM/systemd 支持；单路故障降级；文档与实现同步。
