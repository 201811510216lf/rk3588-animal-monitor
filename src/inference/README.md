# 推理模块（Phase 3：多模型推理链）

本目录实现多模型推理链：manifest 契约与完整性校验、量化 yolov5 后处理（修复版）、
模型注册表、推理调度器、RKNN 运行时封装（目标板专用）与宿主 MOCK 模型。
宿主可编译部分零 Rockchip/OpenCV 依赖；`rknn_runtime.{h,cpp}` 仅目标构建。

## 状态总表

| 文件 | 状态 | 说明 |
|---|---|---|
| `include/.../inference/inference_types.h` | implemented | Detection / InferenceResult（复用 FrameId） |
| `include/.../inference/i_inference_model.h` | implemented | 推理模型抽象接口 |
| `include/.../inference/model_manifest.h` + `src/inference/model_manifest.cpp` | implemented | 对应 model_template.yaml；复用 config::ConfigLoader YAML 子集；sha256 校验 |
| `include/.../common/sha256.h` + `src/common/sha256.cpp` | implemented | FIPS 180-4，纯 C++17，无外部依赖 |
| `include/.../inference/yolov5_postprocess.h` + `src/inference/yolov5_postprocess.cpp` | implemented | 量化解码 + 按类 NMS（修复 A/B 缺陷）；host 纯逻辑 |
| `include/.../inference/model_registry.h` + `src/inference/model_registry.cpp` | implemented | manifests 目录扫描、逐模型校验、状态表 |
| `include/.../inference/inference_scheduler.h` + `src/inference/inference_scheduler.cpp` | implemented | 同步直调（Phase 3）；有界结果队列；frame_id 去重；每模型指标 |
| `include/.../inference/rknn_runtime.h` + `src/inference/rknn_runtime.cpp` | **hardware-blocked / UNVERIFIED** | rknn_api RAII 封装 + RknnYolov5Model；未在板卡编译运行；仅 `ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON` 时编译 |
| `include/.../inference/mock_inference_model.h` + `src/inference/mock_inference_model.cpp` | implemented（**MOCK**） | 确定性检测输出，仅宿主测试/联调，不代表真实推理 |

## 关键设计决策

1. **后处理解码公式采用任务书 sigmoid 版本**（与官方 rknn_model_zoo 一致）：
   `cx=(sigmoid(tx)*2-0.5+grid)*stride`、`w=(sigmoid(tw)*2)^2*anchor`、
   `confidence=sigmoid(box_conf)*sigmoid(class_conf)`。
   **注意**：源项目 A/B 实际代码省略了 sigmoid（`postprocess.cpp:243-249`），
   且其阈值比较在 int8 域直接量化 conf_threshold，说明 relu 模型输出疑似已内置
   sigmoid 激活。二者差异必须由目标板首测裁决（框中心/尺寸系统性偏移即为信号）；
   若实测与 A 版一致，需要为 `DecodeQuantizedHead` 增加"是否施加 sigmoid"开关。
2. 类别 argmax 在 int8 域比较（反量化是单调仿射变换，等价），与 A/B 一致。
3. NMS 按类别分组（修复跨类抑制 bug）；IoU 无 +1 像素修正（A 版有，差异可忽略）。
4. 输出头 stride（8/16/32）由输出张量字节数与输入尺寸**反推**，不依赖输出顺序；
   锚点使用 A/B 实测 yolov5s 锚点（COCO），UNVERIFIED。
5. 调度器 Phase 3 为同步直调；并发策略待目标板实测后替换为线程池 + 异步队列。
6. 结果队列 drop_oldest（结果侧优先保留最新输出）；同一帧按 frame_id 去重（每路保留
   最近一次提交），重复提交返回 kAlreadyExists（幂等，非致命）。

## 模型缺失语义

`VerifyManifestFileSha256` 在模型文件缺失时返回 `kNotFound`，消息为
"模型文件缺失，应用可按无 AI 模式运行"。注册表对缺失模型照常注册，
状态记为 `model_missing`（`AvailableModels()` 不含此类模型），应用据此降级为
无 AI 运行，不得阻断主链路。

## 状态取值（ModelRegistry::StatusRecords）

`ok` / `model_missing` / `sha256_mismatch` / `sha256_unknown`（空或 NOT_COMPUTED）/
`manifest_invalid`（含 legacy_detectors.yaml 列表式登记）/
`duplicate_model_id` / `skipped_non_yaml`。

## 测试与接线

- host 单元测试：`tests/unit/test_yolov5_postprocess.cpp`、`test_sha256.cpp`、
  `test_model_registry.cpp`、`test_inference_scheduler.cpp`（测试 CMake 注册待统一接线）。
- 目标板验证清单（未执行，禁止声称通过）：
  1. `RknnYolov5Model::Load` 的输出张量反推是否与 `rknn_query` 实测一致；
  2. sigmoid 取舍裁决（见决策 1）；
  3. NPU core mask、wants_float、letterbox 实测；
  4. 预处理（RGB 假设、双线性拉伸）与 RGA 零拷贝路径的替换评估。

## 已知限制

- 不支持 NV12 直接输入（待 RGA 路径）；不支持 mean/std 归一化与 float 输出后处理；
- `legacy_detectors.yaml` 为列表式登记格式，不属于 manifest 模板，注册表按
  `manifest_invalid` 记录并跳过（迁移前须完成许可授权，风险 R-01）。
