# 子审计报告 B：demo_multhread_decode_infer_mulmodel（存档）

> 只读审计，2026-11-06。摘要版；结论已并入 source_audit.md 与 module_mapping.md。

## 结构
- main.cpp：6 路 images/mutexes；combineImage（1280×1080 六宫格，但推流配置 1280×720 不一致）；
  rknn_infer 每路 4 模型（ThreadPool(4) 并行）；24fps 睡眠节拍；RTMP url 硬编码。
- lib：mpp_decoder / mpp_encoder / postprocess / stream_loader / streaming_manager /
  detection_fusion_manager；include 对应头 + rknnPool.hpp / ThreadPool.hpp / dma_alloc.hpp。
- 构建证据：CMake 3.16.3、g++ 板端原生（rpath /usr/lib/aarch64-linux-gnu、路径 /home/firefly/项目/project1）；
  **源码 7/27 修改后从未编译成功**（stream_loader.cpp:148 缺分号；build .o 停留在 4/17）。

## MPP 编码（B 新增）
- MPP_ENC_SET_*：prep width/height/stride（**未 16 对齐**）、format=**YUV420P(I420)**；
  rc VBR/bps_target=bitrate/bps_max=2x/bps_min=½；gop=fps；H264 Baseline+CAVLC。
- frm/pkt group 为 internal(ION)；每帧 mpp_buffer_get(w*h*3/2) + memcpy。
- get_packet 轮询 50×1ms 拷出 AnnexB；**编码侧不设帧 PTS**。
- RTMP：FFmpeg 仅 muxing（avformat flv + avio_open + av_interleaved_write_frame）；
  PTS = 帧序号 rescale 到 {1,fps}；**未标 AV_PKT_FLAG_KEY**；GetHeader 在首帧编码前取（可能为空）；
  每帧 new 1.8MB 缓冲不复用。

## 每路 4 模型
- rknn_lite(model, core_mask, class_num, id)；interf：BGR2RGB→resize（无 letterbox）→
  inputs_set/run/outputs_get（返回值不检查，假定 n_output==3）→ post_process → 在共享 ori_img 上画框（并发写风险）。
- fusion：两两配对（Person×Helmet 用 IoM≥0.3，其余 IoU>0.5）加权融合；
  融合类别取小 id（丢 Helmet 语义）；整体类别无关 NMS。**drawDetections 画的是四路原始结果，融合输出未接入推流链路。**

## StreamLoader / Manager
- 每路一线程（阻塞式）；Mbuffer 单槽覆盖式（消费端永远读最新帧）；stopFlag atomic。
- 断流：EOF 重试 10 → close → 重连（网络 10s/本地 100ms）。MAX_PACKETS_NO_FRAME=100
  return false 时不置 status → 退化为忙轮询。
- B 版回调用 RGA imcvtcolor（NV12→BGR888）失败回退 OpenCV；比 A 版多预分配 yuv_work/bgr_work。

## dma_alloc.hpp
- DMA_HEAP_PATH="/dev/dma_heap/system"（硬编码）；open→ioctl(DMA_HEAP_IOCTL_ALLOC)→mmap；
  heap fd 用完即关（不复用）；仅 main 的 RGA 拼接画布用（**整条推理/编码链仍是拷贝型，非零拷贝**）。

## 关键缺陷（高）
1. stream_loader.cpp:148 语法错误 → 当前源码不可编译。
2. postprocess：无 `results[i].prop` 赋值 + 恒用 labels[0] → 置信度/NMS 用未初始化内存（A 版有 prop 赋值）。
3. mpp_decoder.cpp:65 局部变量遮蔽成员 mpp_ctx → Reset/析构失效，每路/每次重连泄漏 MPP 上下文。
4. streaming_manager：frames_dropped 双锁竞争；RTSP 恒 true 空桩；初始化失败泄漏 mpp_encoder_。
5. 融合模块未接入推流画框（双重画框隐患）。
6. 编码 stride 未对齐、I420 与解码 NV12 不一致、PTS 无关键帧标记。

## 可复用
MppEncoder 封装骨架、rknn_lite（去 exit(-1)）、B 版 ThreadPool（修 finishedThreadIDs_ 竞争）、
dma_alloc.hpp、IoU/IoM/加权融合算法本体、断流重连策略。其余按 module_mapping.md 重构。
