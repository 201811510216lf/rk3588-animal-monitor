# ADR-0001：语言与分层

- 状态：accepted
- 日期：2026-11-06

## 背景

任务要求整合 V4L2/MPP/RGA 视频链路、RKNN 推理、两类自定义传感器驱动与众多外设。
需要确定各层语言、构建系统与封装边界，避免内核/用户态混乱和 C API 裸用。

## 决策

1. **内核/BSP 层只用 C 与 DTS**（内核不支持 C++）：MLX90640 自定义 I2C 驱动、
   HTU21D 自定义 IIO 驱动、Sensor 驱动（仅当确定型号且 BSP 缺驱动时）、DTS/pinctrl/
   regulator/clock；其余外设优先复用内核既有驱动与框架（at24、mcp251x、RTC class、
   PWM framework、IIO ADC、SoC watchdog 等），不重复造轮子。
2. **用户态统一 C++17**（C++20 仅在工具链验证后启用）；CMake 构建；
   `src/` 与 `include/` 分离；子系统类封装（camera/media/inference/thermal/...），
   命名遵守 AGENTS.md（类 PascalCase、方法 PascalCase、自由函数 lower_snake_case、
   常量 kPascalCase、私有成员尾下划线）。
3. **C API 适配层**：V4L2/MPP/RGA/RKNN/ZLM/SocketCAN/IIO/TTY/系统调用全部 RAII 封装；
   句柄生命周期由构造/析构管理，初始化失败可安全回滚。
4. **host 可编译性**：核心库与单元测试不依赖 Rockchip SDK（`ANIMAL_MONITOR_ENABLE_TARGET_DEPS=OFF`
   默认关闭）；目标依赖用 Find 模块探测，缺失时清晰报错。
5. Python 仅用于离线模型转换/数据分析和辅助工具，不进入实时主流水线。

## 后果

- 正向：内核驱动与用户态服务可独立演进；host 持续集成不依赖板卡；错误处理与资源
  生命周期可测试。
- 代价：C API 适配层需要前期投入；每接入一个 C API 都要先写 RAII 封装与 mock。
- 审计支撑：源项目 A/B 的问题（裸指针、局部变量遮蔽句柄、exit(-1)、无 RAII 的
  AVFormatContext 管理）均被本决策覆盖。
