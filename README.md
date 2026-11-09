# RK3588 Animal Behavior & Thermal Monitoring Platform

RK3588 动物行为识别与红外监测平台（新整合工程）。

> 任务书：`RK3588_动物检测推理_Codex开发交接包/CODEX_MASTER_TASK.md`
> 规则文件：本目录 `AGENTS.md`（源自交接包，未删改任何规则）

## 项目定位

将两个源项目中经审计确认有价值的代码，整合为一个完整、可扩展、可维护的 RK3588
动物行为与红外监测平台：

- 双路 MIPI CSI-2 相机输入 + ISP/V4L2 采集（Sensor/开发板型号未定，`UNVERIFIED`）
- RGA 双画面合成（默认 side-by-side）+ MPP 单路 H.264 编码推流（RTSP 为主，WebRTC/RTMP 可选）
- 三行为模型 Grooming / Feeding / Rearing（模型文件尚未提供，仅接口与 manifest 模板）
- MLX90640 红外热成像（自定义内核 I2C 驱动，禁止 miscdevice）、HTU21D 温湿度（自定义 IIO 驱动）
- CAN / RS485 / GPIO+IRQ / PWM / ADC / RTC / EEPROM / USB / Ethernet / Wi-Fi / Watchdog
- MQTT / HTTP / WebSocket / RTSP / WebRTC / RTMP(可选) / Modbus RTU / SocketCAN
- 离线缓存补传、安全 OTA、设备管理、日志与指标

## 当前状态（Phase 0 / Phase 1）

| 阶段 | 状态 | 说明 |
|---|---|---|
| Phase 0 审计 | 已完成 | 见 `docs/migration/source_audit.md`、`docs/migration/module_mapping.md`、`docs/migration/risk_register.md` |
| Phase 1 骨架 | 已完成 | host 构建与单元测试通过；不含任何 Rockchip SDK 依赖 |
| Phase 2~8 | planned | 待用户确认迁移矩阵后继续；不得在确认前迁移大段业务代码 |

Phase 1 实测（2026-11-06）：MSVC 19.44 编译 0 告警；ctest 5/5 通过；示例运行退出码 0
（详见 `docs/deployment/build_host.md`）。

各功能模块状态一律标注 `implemented` / `stub` / `planned` / `hardware-blocked`；
未经真实板卡实测的硬件功能一律标注 `UNVERIFIED`。

## 目录结构

```text
├── include/animal_monitor/   # 公共头文件（按子系统划分）
├── src/                      # 用户态 C++17 源码（子系统类封装）
│   ├── main.cpp              # 仅负责配置加载、依赖注入、服务启动、信号处理、退出
│   ├── common/               # 错误码、有界队列、时间戳对齐、Modbus CRC 等基础设施
│   ├── config/  logging/  metrics/  buffer/
│   └── app/                  # Application / Service 生命周期
├── kernel/                   # 内核 C 驱动与 DTS（Phase 4+；与 CMake 分离）
├── configs/                  # YAML 配置与 board profiles（无硬件信息，一律 UNVERIFIED）
├── tests/unit/               # host 单元测试（不依赖 Rockchip SDK）
├── models/                   # 模型 manifest 与说明（不放模型大文件）
├── scripts/                  # 审计/构建/部署/测试脚本
├── third_party/              # 第三方依赖策略（不在仓库内放置大体积 SDK/模型）
├── deploy/                   # systemd / udev / buildroot / ota 模板
└── docs/                     # 架构、迁移、硬件、驱动、协议、部署、测试文档
```

## 构建（host）

Windows（本机已用 VS2022 验证）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1
```

或手动：

```powershell
& "D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G Ninja
& "D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C build
ctest --test-dir build --output-on-failure
```

host 构建**不依赖** Rockchip SDK；MPP/RGA/RKNN/ZLMediaKit 探测默认关闭
（`ANIMAL_MONITOR_ENABLE_TARGET_DEPS=OFF`），打开后按
`docs/deployment/dependency_matrix.md` 提供 SDK 路径。

## 构建（目标板 RK3588，Linux aarch64）

```bash
cmake -S . -B build-rk -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3588-aarch64-linux-gnu.cmake \
      -DANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON -DRK_SDK_ROOT=/path/to/sdk
cmake --build build-rk -j$(nproc)
```

工具链/交叉编译器与 sysroot 以实际板卡 BSP 为准（`UNVERIFIED`），详见
`docs/deployment/build_rk3588.md` 与 `docs/deployment/build_host.md`。

## 运行与测试

```powershell
# host 运行（默认仅加载配置并打印模块状态；mock 演示需显式开启）
build\bin\animal_monitor_app.exe --config configs\default.yaml --help
build\bin\animal_monitor_app.exe --config configs\default.yaml

# host 单元测试
ctest --test-dir build --output-on-failure
```

## 许可证与来源声明

- 本项目**新编写**的代码采用 Apache-2.0（见 `LICENSE`）。
- 源项目 A/B 代码的原始许可为**学习/研究限定**（禁止商用、再分发与衍生再分发），见
  `docs/migration/source_audit.md`；任何从 A/B 迁移的代码保留原许可与出处，商用前必须
  完成许可清理并取得版权方书面授权。
- 参考仓库 `JoeChen2me/RK-MediaProject` 仅作风格与架构参考，不直接复制。
- 仓库内**不**放置：模型大文件、SDK 动态库、凭据与密钥（配置示例一律使用占位符）。

## 免责声明

本仓库当前不包含任何经实测验证的硬件能力声明；所有板卡、Sensor、GPIO、DTS、
模型、性能数据均以 `UNVERIFIED` / `NOT_MEASURED` 标记，直到有原理图/BSP/实测支撑。
