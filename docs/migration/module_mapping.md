# 迁移矩阵（module_mapping）

> 说明：迁移策略定义 —— **原样迁移**（仅做构建适配与许可声明）、**重构**（保留算法/接口语义，
> 修审计缺陷并套新工程规范）、**重写**（只取设计思路）、**弃用**（不迁移）。
> 在用户确认本矩阵前，**不迁移任何大段业务代码**（任务书第 16 节第 7 条）。

## 1. 用户态视频/推理链

| 来源文件 | 来源 | 原功能 | 依赖 | 迁移策略 | 目标路径 | 验证方式 |
|---|---|---|---|---|---|---|
| lib/mpp_decoder.cpp + .h | A/B | MPP H.264/265 硬解封装（split_parse、EXT_BUF_GROUP、info-change） | librockchip_mpp | 重构（修局部变量遮蔽/错误上报/RAII/回调线程模型） | src/media/mpp_decoder.{h,cpp} | 目标板 mpi_dec 单路/多路测试；host 编译通过（依赖关时以 stub 编译） |
| lib/mpp_encoder.cpp + .h（B） | B | MPP H.264 编码封装 + RTMP 送包 | librockchip_mpp、FFmpeg(仅 muxing) | 重构（stride 对齐、PTS/关键帧、extradata 时机、缓冲复用、I420→NV12 决策） | src/media/mpp_encoder.{h,cpp} | 目标板编码→ffprobe/ffplay 验证 |
| lib/stream_loader.cpp + .h | A/B | FFmpeg RTSP/文件拉流 + BSF + 解码回调 + 重连 | ffmpeg-lite、OpenCV、MPP | 重构（RAII 管 AVFormat/Packet/BSF/Dict、URL 配置化去凭据、视频流索引、hvcC、有界队列替代单槽） | src/camera/stream_source.{h,cpp}（V4L2 之外的网络/文件输入源） | host 模拟/目标板 RTSP 拉流测试 |
| lib/postprocess.cpp + .h | A/B | yolov5 量化输出解码 + NMS + 标签 | 无（纯 C） | 重构（恢复 prop 赋值、修跨类 NMS、标签/anchor/阈值/类别数全参数化、去全局态） | src/inference/postprocess/yolov5_postprocess.{h,cpp} | host 单元测试（合成 tensor 向量） |
| include/rknnPool.hpp（rknn_lite） | A/B | RKNN 运行时封装（core mask、量化参数、want_float 输出） | librknnrt | 重构（去 exit(-1)、返回值检查、推理/绘制分离、letterbox/预处理策略化、n_output 校验） | src/inference/rknn_runtime.{h,cpp} + rknn_model.h | 目标板单模型/三模型推理测试；host 以接口 stub 编译 |
| lib/detection_fusion_manager.cpp + .h（B） | B | 多模型检测融合（IoU/IoM/加权/NMS） | 无（纯算法） | 重构（修类别无关 NMS、接入推流链路、时间戳用帧时钟、配置化权重） | src/inference/detection_fusion.{h,cpp} | host 单元测试（合成检测框） |
| lib/streaming_manager.cpp + .h（B） | B | 编码输出→RTMP 推流（FFmpeg muxing） | FFmpeg、MPP | 重写（RTSP 桩删除或实现；统一 PTS；单锁统计；失败路径清理） | src/media/stream_publisher.{h,cpp} + communication/rtmp_publisher | 目标板 ffplay 拉流 |
| include/ThreadPool.hpp | A/B | 简单线程池 | 无 | 重构（修 finishedThreadIDs_ 竞争、result_of→invoke_result）或改用 std::async 调度 | src/common/thread_pool.h | host 单元测试（并发提交/回收） |
| include/dma_alloc.hpp（B） | B | dma-heap 分配封装 | Linux dma-heap | 重构（路径配置化、fd 复用、错误置空、RAII） | src/buffer/dma_heap_allocator.{h,cpp} | 目标板分配/释放循环测试 |
| include/rga_func.h、drm_func.h | A/B | 死代码（无调用点） | librga | 弃用 | — | — |
| main.cc / demo_multhread_decode_infer.cpp / B main.cpp | A/B | demo 入口（全局队列、裸线程、硬编码） | — | 弃用（仅作行为参考；新入口 = src/main.cpp + Application） | — | — |
| Ref src/{v4l2Camera,rkmppdec,rkrga,rkmppenc,zlm_publisher}.cpp | Ref | V4L2 DMABUF 采集/MPP 零拷贝解码/RGA/编码/ZLM 发布 | MPP/RGA/ZLM | **风格与协议参考，不复制实现**；按需以新规范重写 | src/camera/v4l2_camera、src/media/{mpp_decoder,rga_processor,mpp_encoder,stream_publisher} | 目标板 Phase 2 端到端 |
| ffmpeg-lite / ffmpeg7-1 / opencv4-1 / mpp / rga / rknn 目录 | A | 板端 SDK/第三方库本地拷贝 | — | **不复制进仓库**（体积/许可）；构建时从 BSP/系统或 `third_party/` 外部提供 | third_party/（仅记录来源与版本） | 依赖矩阵核对 |

## 2. 模型资产

| 来源 | 原用途 | 迁移策略 | 目标路径 | 验证方式 |
|---|---|---|---|---|
| A\model\{person,helmet,tired,callplay}_relu.rknn + coco_80_labels_list.txt | B 每路多模型检测 | **暂不迁移文件**；在 models/manifests/ 记录其 SHA256 与推理契约（迁移需取得许可） | models/manifests/legacy_detectors.yaml | manifest 校验 |
| A\demo_*\model\RK3588\{yolov5s*.rknn} | 未被任何入口使用 | 弃用（仅记录存在性） | — | — |
| Grooming / Feeding / Rearing 模型 | **不存在** | 只建接口 `IInferenceModel` + manifest 模板，禁止编造 | models/manifests/model_template.yaml | host 校验 manifest 加载 |

## 3. 内核/BSP（Phase 4+，本节仅登记）

| 来源 | 原功能 | 迁移策略 | 目标路径 | 验证方式 |
|---|---|---|---|---|
| （无现成驱动源码）MLX90640 | — | 新写内核 C I2C 驱动（禁止 miscdevice；先评估 V4L2 thermal capture vs 标准 cdev，见 ADR 待补） | kernel/drivers/mlx90640/ | 目标板 I2C probe/EEPROM/subpage 测试 |
| （无现成驱动源码）HTU21D | — | 新写内核 C IIO 驱动（CRC + 转换时间；先查目标内核是否已有 htu21 驱动避免重复绑定） | kernel/drivers/htu21d/ | 目标板 IIO sysfs 读值 + CRC 负例 |
| CAN/RS485/GPIO/PWM/ADC/RTC/EEPROM/Watchdog | — | 优先复用内核现有驱动/框架；只做 Board Profile 配置与文档 | configs/boards/、docs/hardware/ | 目标板外设测试脚本（scripts/test_*.sh） |
| Device Tree | — | 不臆造；板卡确定后基于 BSP DTS 增量 | kernel/dts/ | 板卡 BSP 编译+启动 |

## 4. 迁移顺序（Phase 2 起，待用户确认本矩阵后执行）

1. 基础设施（已随 Phase 1 完成：Status/日志/指标/有界队列/时间戳对齐/配置校验）。
2. 单路 V4L2→MPP dec→RGA→MPP enc→ZLM（Ref 架构模板 + A/B 解码器重构产物）。
3. 双路 capture + 时间戳同步 + side-by-side RGA 合成 + 单路编码推流。
4. 三模型推理（先 IInferenceModel + RknnRuntime + yolov5 后处理重构产物；Grooming/Feeding/Rearing
   模型到位前以 person_relu 契约做接口验证，不宣称行为识别已实现）。
5. MLX90640/HTU21D 用户态客户端 + mock 测试，再内核驱动。
