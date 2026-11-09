# 模型目录（models/）

> 仓库内**不放模型大文件**（体积/许可）。本目录只放 manifest 与说明。

## 现状（审计事实）

| 模型 | 位置（源项目 A） | 用途 | 处理 |
|---|---|---|---|
| person_relu.rknn / helmet_relu.rknn / tired_relu.rknn / callplay_relu.rknn | A\model | B 每路多模型检测 | 记录契约（legacy_detectors.yaml）；迁移需许可授权（R-01） |
| yolov5s-640-640.rknn / yolov5s.rknn | A\demo_multhread_decode_infer\model\RK3588 | 未被任何入口使用 | 仅记录存在性 |
| Grooming / Feeding / Rearing | **不存在** | 三行为模型 | 只建接口与 manifest 模板，禁止编造 |

## 推理契约（来自 rknnPool.hpp/postprocess.cpp，真实代码依据）

- 输入：UINT8 / NHWC / RGB / pass_through=0（内部拷贝）；无 letterbox；无归一化
  （量化参数烘焙）。
- 输出：3 头（stride 8/16/32）yolov5 风格 sigmoid 框解码 + 固定 anchor；int8 + zp/scale
  反量化；BOX_THRESH=0.25、NMS_THRESH=0.45。
- NPU：rknn_set_core_mask 0/1/2（RK3588 三核）。

## 目录约定

- `models/manifests/`：每个模型一个 YAML manifest（model_id、路径、SHA256、RKNN
  兼容信息、输入 shape/layout/格式、mean/std、量化、输出 tensor、类别映射、阈值、版本）。
- `models/grooming|feeding|rearing/`：留给三行为模型（到位后放置 manifest 与文档，
  模型文件放外部存储并只在 manifest 记路径）。
