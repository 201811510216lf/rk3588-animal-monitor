# 子审计报告 A：demo_multhread_decode_infer（存档）

> 只读审计，2026-11-06。摘要版；结论已并入 source_audit.md 与 module_mapping.md。

## 结构
- 编译入口 `demo_multhread_decode_infer.cpp`（CMake 实际构建）；`main.cc` 不参与构建。
- lib：mpp_decoder.cpp / postprocess.cpp / stream_loader.cpp；include：对应头 +
  rknnPool.hpp / ThreadPool.hpp / drm_func.h / rga_func.h / m_buffer.hpp。
- rga_func.h、drm_func.h **无任何编译单元 include**（死头文件）；im2d 调用全部注释；
  person.txt 无引用；deinitPostProcess 无调用点。

## MPP 解码
- Init：mpp_create/init(DEC, AVC|HEVC)，旧式 cfg `base:split_parse=1`；未用
  PARSER_SPLIT_MODE / OUTPUT_FORMAT / EXTRA_FRAME_INFO。
- info-change：`mpp_buffer_group_get_internal(DRM)` → SET_EXT_BUF_GROUP → limit 24 →
  INFO_CHANGE_READY；**无 importbuffer_fd**，回调 fd 恒 0，数据走 `mpp_buffer_get_ptr`。
- get_frame 错误（非 OK/TIMEOUT）静默丢帧；无 flush/EOS；Reset 作用于恒 NULL 的成员 ctx。

## RKNN 契约
- person_relu.rknn（单类）；输入 NHWC/UINT8/RGB、无 letterbox、无归一化、pass_through=0；
- 输出 3 头 int8（stride 8/16/32，yolov5 风格 sigmoid 框解码 + 固定 anchor），
  BOX_THRESH=0.25、NMS_THRESH=0.45；NPU core mask 轮转 0/1/2。

## StreamLoader
- FFmpeg：rtbufsize 8MB / rtsp_transport=tcp / stimeout 2s / max_delay 0.5s；
  H264 用 h264_mp4toannexb（par_in 取 streams[0] 而非视频流）；HEVC 无 BSF。
- 回调：NV12 去 stride 拷贝 → cvtColor BGR → 单槽 Mbuffer{img,mtx}（无 cv、轮询、覆盖式）。
- 重连：网络 10s / 本地 100ms；close 未 av_bsf_free/av_dict_free；重连跳 Init 沿用旧 ctx。

## 关键缺陷（高）
1. mpp_decoder.cpp:65 局部 `MppCtx mpp_ctx` 遮蔽成员 → ctx 泄漏、Reset 无效（析构 :29-33 不 destroy）。
2. stream_loader.cpp:236/270/354 Init/open 返回值不检查。
3. 重连反复 Init 且不 destroy/put 旧资源 → 每次重连泄漏 context+buffer group。
4. stream_loader.h:92-96 RTSP 明文密码硬编码。
5. demo_multhread_decode_infer.cpp:113-128 unload/join 被注释 → 线程永不退出/terminate。
6. unload_stream 对运行中 loader delete（UAF）+ 线程未 join。
7. stoi(argv[1]) 无 argc 校验；num_stream 无上限（数组固定 6）。
8. postprocess.cpp:126 NMS 内层条件 `classIds[i] != filterId` 恒假（跨类抑制缺失）。
9. mpp_decoder info-change 失败仅 break → 静默卡死。
10. rknnPool.hpp 构造失败 exit(-1)；run/get 返回值不检查。
11. 空 buffer 仍无条件 interf() → cv::cvtColor 空 Mat 抛异常。

## 可复用
MppDecoder 框架、postprocess 量化 yolov5 数学、dpool::ThreadPool（修 result_of/finishedThreadIDs_）、
FFmpeg RTSP 打开参数组合与 is_annexb() 逻辑。StreamLoader/rknn_lite/postprocess 全局态必须重构。
