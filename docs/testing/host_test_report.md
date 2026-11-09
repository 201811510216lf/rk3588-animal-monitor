# Host 测试报告（Phase 2–6 集成验证）

> 验证时间：2026-11-09。环境：Windows 10 + VS2022 MSVC 19.44
> （`/W4 /permissive- /utf-8 /EHsc /std:c++17 /MD /O2`）。
>
> 说明：本会话 DSH 受限沙箱无法写工作区且 CMake 编译器 ABI 探测受管道限制卡住，
> 本次验证采用 **cl.exe 直连编译**（与 CMakeLists 中源文件清单一致）：
> 全部主机源码 + 22 个测试套件 + 应用，编译零告警、链接成功。

## 结果汇总

| 项 | 结果 |
|---|---|
| 主机源码编译（core 29 个 + app 4 个 + main） | 通过，0 告警 |
| 应用 animal_monitor_app.exe 链接与运行 | 通过，退出码 0 |
| 单元测试套件 | **22/22 通过**（0 失败） |
| composite 流水线 3 秒实测 | pairs=66、composed=66、encoded=66、published=66、错误 0 |

## 测试套件明细

| # | 套件 | 覆盖内容（Phase） |
|---|---|---|
| 1 | test_bounded_queue | 有界队列策略/超时/关停（1） |
| 2 | test_timestamp_aligner | 时间戳最近邻对齐（1） |
| 3 | test_modbus_crc | CRC-16/MODBUS 权威向量（1） |
| 4 | test_config | YAML 子集解析/校验（1） |
| 5 | test_metrics | 计数器/仪表/直方图（1） |
| 6 | test_frame_synchronizer | 双路同步配对/丢旧帧（2） |
| 7 | test_frame_compositor | 布局纯函数/CPU 合成（2） |
| 8 | test_sha256 | SHA-256 权威向量（3） |
| 9 | test_yolov5_postprocess | 量化解码/按类 NMS（3） |
| 10 | test_model_registry | manifest 注册/缺失模型（3） |
| 11 | test_inference_scheduler | 路由/去重/有界结果（3） |
| 12 | test_mlx90640_math | EEPROM 解析/subpage/TGC/CalcTa/CalcTo（4） |
| 13 | test_htu21d_math | CRC8/温湿度换算（4） |
| 14 | test_roi_mapper | RGB→热图仿射映射（5） |
| 15 | test_temperature_calibrator | 最小二乘拟合/MAE/RMSE（5） |
| 16 | test_animal_tracker | IoU 关联跟踪（5） |
| 17 | test_individual_baseline | 滑动窗口基线（5） |
| 18 | test_anomaly_detector | 规则+z-score 异常检测（5） |
| 19 | test_feature_fusion | 行为-温度时间窗融合（5） |
| 20 | test_modbus_rtu | Modbus 帧编解码（6） |
| 21 | test_can_frame | CAN 帧结构校验（6） |
| 22 | test_peripheral_manager | 外设状态注册表（6） |

## 未验证项（诚实声明）

- 目标板代码（V4L2/MPP/RGA/ZLM/RKNN 客户端、CAN/RS485/GPIO/PWM/ADC/RTC/EEPROM/Watchdog
  客户端、MLX90640/HTU21D 内核驱动）在 Windows 无法编译：全部标注
  `hardware-blocked / UNVERIFIED`，待板卡到位后按 scripts/test_*.sh 验证。
- 温度/标定数值均为 `NOT_CALIBRATED`（无参考测温实测）。
- 常规环境下请用 CMake（`scripts\build_host.ps1`）构建：CMakeLists 源文件清单与本报告
  直连编译清单一致。
