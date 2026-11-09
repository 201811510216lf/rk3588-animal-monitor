# 初始架构（initial_architecture）

> 依据：CODEX_MASTER_TASK.md 第 3~13 节 + 源项目审计结论（source_audit.md）。
> 状态：Phase 1 架构基线；随迁移矩阵确认后逐 Phase 细化。

## 1. 分层与语言边界

```text
┌────────────────────────────────────────────────────────────┐
│ 用户态 C++17（src/ + include/animal_monitor/）              │
│  application / camera_service / video_pipeline /            │
│  inference_service / thermal_service / environment_service  │
│  calibration_service / fusion_service / device_manager      │
│  can_service / rs485_service / telemetry_service /          │
│  network_service / storage_service / ota_service /          │
│  diagnostics_service                                        │
├────────────────────────────────────────────────────────────┤
│ 基础设施（C++17，无平台依赖）：Status/日志/指标/有界队列/     │
│ 时间戳对齐/配置(YAML 子集)/RAII 句柄                         │
├────────────────────────────────────────────────────────────┤
│ C API 适配层（RAII 封装，目标板专用，stub 化保证 host 编译） │
│  V4L2 / MPP / RGA / RKNN / ZLMediaKit / SocketCAN / IIO /    │
│  TTY / libgpiod / at24 / RTC / watchdog                      │
├────────────────────────────────────────────────────────────┤
│ 内核/BSP（C + DTS，kernel/，与 CMake 分离）                  │
│  MLX90640 I2C 驱动（禁 miscdevice）/ HTU21D IIO 驱动 /       │
│  其余外设优先复用内核现有驱动 / DTS 基于真实 BSP（不臆造）   │
└────────────────────────────────────────────────────────────┘
```

规则：内核只有 C/DTS；用户态 C++17；Python 仅限离线模型转换/分析工具。
C API 一律 RAII 封装；禁止裸 owning 指针；禁止全局可变状态。

## 2. 核心设计决策（ADR 索引）

- [ADR-0001](decisions/ADR-0001-language-and-layering.md)：C++17/CMake/子系统类分层；
  内核 C 驱动；C API RAII 适配层。
- [ADR-0002](decisions/ADR-0002-dual-camera-composition.md)：双路采集与合成/独立/单路三种
  输出模式；默认 composite 单路编码推流。
- [ADR-0003](decisions/ADR-0003-buffer-ownership.md)：DMABUF 所有权与回收协议（沿用参考仓库
  holder 延迟归还模式，修复其无界队列问题）。

## 3. 子系统现状（Phase 1）

| 模块 | 状态 | 说明 |
|---|---|---|
| common（Status/BoundedQueue/TimestampAligner/ModbusCrc/FrameMeta） | implemented | host 单元测试通过 |
| config（YAML 子集解析+校验） | implemented | 子集语义见 config_loader.h；后续可换 yaml-cpp |
| logging / metrics | implemented | stderr 输出；文件轮转 planned |
| app（Application/Service/MockPipelineService） | implemented | MOCK 明确标注，仅验证接线 |
| camera/video_pipeline/media/inference/thermal/environment/calibration/fusion/device/communication/storage/ota | planned | 接口先行 + stub；随 Phase 2~7 落地 |
| kernel 驱动与 DTS | hardware-blocked（部分 planned） | 板卡/BSP 未定 |
| 硬件相关外设 | hardware-blocked | 仅 Board Profile 与文档 |

## 4. 配置与硬件声明策略

- 所有设备路径/分辨率/码率/队列容量/端口/模型路径一律 YAML（configs/default.yaml），
  代码内禁止硬编码。
- 板卡/Sensor/GPIO/总线/DTS 一律 `configs/boards/<board>.yaml` + `UNVERIFIED` 标记，
  直到有原理图/BSP/实测依据。
- 凭据禁止入库（占位符 + 运行时注入）。
- 每条流水线边必须声明：队列容量、满策略、超时、关停行为、指标。

## 5. 可靠性策略（贯穿各 Phase）

- 错误传播：Status 类型化返回 + 日志/指标可见；失败可降级（如 AI 不可用但视频继续）。
- 线程关停：stop 源 → 唤醒等待者 → 排空/取消队列 → 释放下游引用 → 销毁硬件上下文
  （见 thread_model.md）。
- 时间：内部一律 CLOCK_MONOTONIC；墙上时间单独存；保留硬件时间戳。
- 指标：每模型延迟 p50/p90/p99、FPS、丢帧、队列深度、重连时长、磁盘水位等；
  未实测一律 NOT_MEASURED。

## 6. 下一阶段（Phase 2）准备清单

1. 用户确认迁移矩阵（module_mapping.md）。
2. 取得/确认源代码许可授权路径（R-01）。
3. 板卡选型输入（哪怕只定板卡型号）→ 填 board profile。
4. host 端：video_pipeline 骨架 + stub 设备 + 队列/同步器单元测试。
5. 目标端：单路 V4L2→MPP→RGA→MPP→ZLM 端到端（硬件测试脚本先行）。
