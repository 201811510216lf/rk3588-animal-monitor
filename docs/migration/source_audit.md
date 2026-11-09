# 源项目只读审计报告（source_audit）

> 审计日期：2026-11-06。审计方式：只读遍历 + 全文阅读关键源码；未对源项目做任何修改。
> 本文件是迁移决策的唯一起点；任何从 A/B 迁移的代码必须保留出处与本文件记录。

## 1. 审计对象

| 代号 | 路径 | 性质 |
|---|---|---|
| A | `E:\embedded\双路 ISP 采集与双画面单路推流重构\多路视频` | 只读源项目（demo 合集 + 本地 SDK/模型） |
| B | `E:\embedded\双路 ISP 采集与双画面单路推流重构\多路视频\demo_multhread_decode_infer_mulmodel` | 只读源项目（A 的子目录，多路多模型 demo） |
| Ref | `https://github.com/JoeChen2me/RK-MediaProject`（本机浅克隆于 `.ref_audit_RKMediaProject`） | 风格/架构参考，不直接复制 |

## 2. 概览

### 2.1 项目 A（多路视频）

```
多路视频/
├── demo_multhread_decode/            # 空目录（无源码）
├── demo_multhread_decode_infer/      # 单路 RTSP 拉流+MPP硬解+RKNN(yolov5s) demo
│   ├── demo_multhread_decode_infer.cpp   # CMake 实际编译入口（多路显示 + 每路推理）
│   ├── main.cc                           # 另一个入口（不参与构建；单路+全局队列）
│   ├── lib/   mpp_decoder.cpp postprocess.cpp stream_loader.cpp
│   ├── include/ mpp_decoder.h postprocess.h stream_loader.h rknnPool.hpp ThreadPool.hpp
│   │           drm_func.h rga_func.h m_buffer.hpp（含 librknnrt.so/librknn_api.so）
│   └── model/ RK3588/{person_relu.rknn, yolov5s-640-640.rknn, yolov5s.rknn}
├── demo_multhread_decode_infer_mulmodel/  # 项目 B（见下）
├── model/  person/helmet/tired/callplay_relu.rknn + coco_80_labels_list.txt
├── mpp/    Rockchip MPP 头文件 + librockchip_mpp.so/.a + libmpi_enc.so
├── rga/    RGA im2d 头文件 + librga.so/.a
├── rknn/   rknn_api.h + librknnrt.so/librknn_api.so + rknn_server
├── opencv4-1/   OpenCV4 头文件 + lib.tar.gz（需 lib.sh 解压）
├── ffmpeg-lite/ FFmpeg 6.x aarch64 精简版（libavcodec.so.60 等，已解压）
├── ffmpeg7-1/   FFmpeg 7.1（lib.tar.gz 未解压）
├── 1.mp4 2.mp4 3.mp4 4.mp4           # 测试视频
├── 流程图.png lib.sh LICENSE .gitignore
```

- **Git**：A 不是 Git 仓库（无 .git，无提交历史可审计）。
- **许可证**：`LICENSE`（版权方 HeXiaotian，2025）为**学习/研究限定**：禁止未经书面许可的商用、再分发、转售、衍生再分发。**迁移代码商用前必须取得书面授权**（风险 R-01）。
- **构建目标**：aarch64 板端原生编译（B 的 build/CMakeCache.txt：`/usr/bin/c++`、rpath 含 `/usr/lib/aarch64-linux-gnu`、源码路径 `/home/firefly/项目/project1`，即 Firefly RK3588 系列；板卡型号未明）。

### 2.2 项目 B（demo_multhread_decode_infer_mulmodel）

多路 RTSP/文件 → MPP 硬解 → 每路 4 个 RKNN 模型（person/helmet/callplay）→ 检测融合 →
2×3 宫格拼接（RGA imresize 到 DMA tile）→ MPP H.264 编码 → FFmpeg 封装 RTMP 推流。

```
B/
├── main.cpp            # 6 路 images/mutexes 数组；combineImage；rknn_infer
├── CMakeLists.txt      # C++17；本地 ffmpeg-lite/opencv4-1/mpp/rga/rknn；GLOB lib/*.cpp
├── lib/  mpp_decoder.cpp mpp_encoder.cpp postprocess.cpp stream_loader.cpp
│         streaming_manager.cpp detection_fusion_manager.cpp
├── include/ 对应头 + rknnPool.hpp ThreadPool.hpp dma_alloc.hpp rga_func.h m_buffer.hpp
├── build/   # 4 月 17 日的旧产物（目标 myDemo）；当前源码 7 月 27 日后从未编译成功
└── node_modules/ package.json create_agent_rules_deck.js agent.md  # PPT 生成残留
```

## 3. 参考仓库（Ref：RK-MediaProject）

- 链路：V4L2(USB MJPEG) → MPP 解码(零拷贝) → RGA → MPP H.264 编码 → ZLMediaKit 推流(RTSP+WebRTC)。
- 类拆分：`V4L2_Camera` / `MppDecInstance` / `MppEncInstance` / `RgaInstance` / `ZlmPublisher`；
  `src/`+`include/` 分离、C++17、CMake、Google 风格 `.clang-format`（4 空格、100 列、自定义大括号换行）。
- 零拷贝协议：dma-heap 池 → `V4L2_MEMORY_DMABUF` 注入相机 → MPP external-buffer 模式三 +
  `mpp_buffer_import`（相机→解码零拷贝）→ RGA `importbuffer_fd` → 编码 import RGA fd；
  `DecodedTaskHolder` + `PendingRecycleQueue` 延迟归还，RGA 未消费完不解码输出 buffer。
- 线程：capture/decode/mpp_output_recycle/rga/mpp_encode_input/mpp_encode_output 六个，
  队列为 mutex+cv+deque；唯一有界队列 readyQueue（上限 12，丢最旧）；优雅退出按依赖顺序 join。
- README 含 1 小时 KPI 实测表（OrangePi 5 Plus，1080p30，端到端均值 43ms）。

## 4. 关键发现（审计结论汇总）

### 4.1 模型真实情况（禁止臆造）

| 模型文件 | 位置 | 实际用途 | 与新项目的关系 |
|---|---|---|---|
| person_relu.rknn | A\model、A\demo_*\model\RK3588 | B 每路第 1 模型（单类 person） | 可作为行为检测的通用目标检测子模块候选 |
| helmet_relu.rknn | A\model | B 每路第 2 模型 | 与动物业务无直接关系 |
| tired_relu.rknn | A\model | B 代码中注释掉 | 同上 |
| callplay_relu.rknn | A\model | B 每路第 4 模型 | 同上 |
| yolov5s*.rknn | A\demo_*\model\RK3588 | **未被任何入口使用** | 仅存在于目录 |
| **Grooming / Feeding / Rearing 三行为模型** | **不存在** | — | **只建接口与 manifest 模板，禁止编造模型结构或推理结果** |

推理契约（从 rknnPool.hpp + postprocess.cpp 提取，真实代码依据）：
- 输入：UINT8 / NHWC / RGB（BGR2RGB 由 OpenCV 完成）；**无 letterbox**，直接拉伸到模型尺寸；
  无均值/方差归一化（量化参数已烘焙）；pass_through=0（rknn_inputs_set 内部拷贝，非零拷贝）。
- 输出：3 头（stride 8/16/32）yolov5 风格解码（sigmoid 框解码 + 固定 anchor），
  `want_float=0` 取 int8，按 output_attrs 的 zp/scale 反量化；阈值 BOX_THRESH=0.25、NMS_THRESH=0.45。
- NPU 绑定：`rknn_set_core_mask` 按实例号轮转 0/1/2（RK3588 三核）。

### 4.2 A/B 共性问题（迁移必须修复，否则禁止带入新项目）

| # | 问题 | 位置 | 级别 |
|---|---|---|---|
| 1 | **B 当前源码无法编译**：`stream_loader.cpp:148` `int no_frame` 缺分号；build 产物为 4/17 旧版（源码 7/27 改动从未编译验证） | B lib/stream_loader.cpp | 高 |
| 2 | **post_process 丢失 `prop` 赋值**（A 版有 `prop=obj_conf`，B 版删掉），且恒用 `labels[0]` → 融合置信度/NMS 基于未初始化内存 | B lib/postprocess.cpp:398 | 高 |
| 3 | **MppDecoder 局部变量遮蔽成员**：`Init` 内 `MppCtx mpp_ctx` 局部变量使成员恒 NULL → Reset 无效、析构不释放、重连泄漏 MPP context（A/B 同病） | A/B lib/mpp_decoder.cpp:65 | 高 |
| 4 | RTSP URL 与明文密码硬编码（`admin:******@192.168.1.x`，密码已脱敏） | A stream_loader.h:92-96 | 高 |
| 5 | 推理线程永不退出：stopFlag 未置位/join 缺失 → 挂起或 std::terminate；unload_stream 对运行中 loader delete（UAF） | A demo_multhread_decode_infer.cpp:113-128、A stream_loader.cpp:360-367 | 高 |
| 6 | NMS 跨类抑制 bug：`classIds[i] != filterId` 恒假（迁移 80 类模型必错） | A/B postprocess.cpp:126 | 高 |
| 7 | rknnPool 构造失败直接 `exit(-1)`；rknn_run/outputs_get 返回值不检查；假定 n_output==3 | A/B rknnPool.hpp | 高 |
| 8 | MPP info-change 失败仅 break 不报错 → 静默卡死 | A/B mpp_decoder.cpp:182-230 | 高 |
| 9 | StreamingManager 统计双锁竞争；RTSP 推流为恒 true 空桩；PTS 无关键帧标记；GetHeader 时机过早 | B streaming_manager.cpp | 高 |
| 10 | 全局静态 labels 无锁（多实例首调竞态）；`memset(ptr,0,sizeof(ptr))` 只清 8 字节；VLA 非标准 | A/B postprocess.cpp / rknnPool.hpp | 中 |
| 11 | ThreadPool `finishedThreadIDs_` 无锁并发修改；C++17 弃用 result_of | A/B ThreadPool.hpp | 中 |
| 12 | StreamLoader：close 未 av_bsf_free/av_dict_free；BSF par_in 取 streams[0] 而非视频流；HEVC 无 hvcC 处理；回调恒按 NV12 | A/B stream_loader.cpp | 中 |
| 13 | RGA/DRM 头文件均为死代码；全链路 CPU 三重拷贝（NV12→BGR→RGB）+ 每帧 malloc，多路 CPU 压力线性放大 | A/B | 中 |
| 14 | B 编码器输入为 I420（MPP_FMT_YUV420P）且 stride 未 16 对齐；每帧 new 1.8MB 缓冲 | B mpp_encoder.cpp | 中 |
| 15 | 融合模块与推流画框脱节：drawDetections 画四路原始结果，fuseDetections 输出未接入 | B | 中 |
| 16 | 大量硬编码：label 路径、模型相对路径依赖 cwd、urls/num_stream 不一致、rtmp_url、帧率睡眠 | A/B | 中/低 |

完整问题清单（含 40+ 条逐行定位）见子审计报告存档：
`docs/migration/_audit_reports/`（本文件下方"附：子审计索引"）。

### 4.3 可复用资产（迁移策略见 module_mapping.md）

- **MppDecoder 框架**（A/B）：split_parse + DRM internal group + SET_EXT_BUF_GROUP + limit 24 +
  INFO_CHANGE_READY 的标准 mpi_dec_test 流程 —— 修复问题 3/8/13 后可复用。
- **postprocess 量化 yolov5 数学**：qnt 反量化/sigmoid 框解码/anchor 公式正确 —— 参数化
  （anchor/stride/类别数/标签路径）后复用。
- **MppEncoder 封装**（B）：Init/EncodeFrame/GetHeader + RGA/OpenCV 双路径 —— 修复 stride/PTS/
  extradata 时机后复用。
- **StreamLoader 断流重连策略**（网络 10s / 本地 100ms 区分 + EOF 计数）—— 重构 RAII 后复用。
- **rknn_lite 模型封装**（core mask、scale/zp 透传、want_float 输出契约）—— 去 exit(-1) 后复用。
- **dma_alloc.hpp**：dma-heap ioctl 简洁封装 —— 可复用。
- **融合算法本体**（IoU/IoM/加权融合/NMS）—— 修跨类 NMS、接入推流链路后复用。
- **参考仓库**：V4L2 DMABUF 注入、external-buffer 零拷贝协议、holder 延迟归还、
  ZLM 内嵌发布（mk_media_input_h264 逐 NALU + 时间戳单调化）—— 作为新项目视频链路的
  架构模板与风格基准。

### 4.4 依赖清单（A/B 实际使用版本）

| 依赖 | 位置/版本 | 备注 |
|---|---|---|
| Rockchip MPP | A\mpp（librockchip_mpp.so） | 头文件版本未标注，`UNVERIFIED` |
| RGA | A\rga（librga.so，im2d） | 同上 |
| RKNN | A\rknn（librknnrt.so/librknn_api.so；rknn_context=uint64_t，API 1.4.x~2.x 系） | 同上 |
| OpenCV 4 | A\opencv4-1（lib.tar.gz 需解压） | 仅用于颜色转换/缩放/显示 |
| FFmpeg | A\ffmpeg-lite（6.x：libavcodec.so.60）、A\ffmpeg7-1（7.1 tar 未解压） | 拉流 demux/BSF；RTMP muxing |
| ZLMediaKit C API | Ref 仓库 lib（libmk_api.so、libZLToolKit.so） | 内嵌 server 发布 RTSP/WebRTC |
| 编译环境 | B build：CMake 3.16.3 / g++ / Ubuntu（板端 aarch64 原生） | 非交叉编译 |

## 5. 未决事项（审计边界）

- `流程图.png`（A 的设计流程图）无法以当前工具读取图像内容，未纳入分析；不影响结论。
- `demo_multhread_decode/main.cc` 与 `demo_multhread_decode_infer.cpp` 两个入口并存，
  整合时以 lib 层为准，入口代码只作参考。
- Ref 仓库为浅克隆（depth 1），无历史提交审计；按任务书要求检查了 README/AGENTS/.clang-format/
  CMakeLists/src/include 模块划分与关键类命名（已完成，见上）。

## 附：子审计索引

三份全文审计报告（各含 40+ 条逐行问题定位）作为本文件的支撑材料保存于
`docs/migration/_audit_reports/`：

- `audit_A_demo_multhread_decode_infer.md`
- `audit_B_demo_multhread_decode_infer_mulmodel.md`
- `audit_ref_RKMediaProject.md`
