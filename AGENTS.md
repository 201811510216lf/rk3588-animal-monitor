# AGENTS.md — RK3588 Animal Monitoring Project

> 本文件源自 `RK3588_动物检测推理_Codex开发交接包/AGENTS.md`，规则原样保留。
> 末尾"项目状态附录"为本项目执行过程中追加，不覆盖、不删除任何原有规则。

## 1. Language and communication
- All explanations, plans, comments intended for maintainers, and documentation must be in Simplified Chinese.
- Code identifiers, API names, protocol names, and standard terminology remain in English.
- Before modifying code, inspect the repository and summarize the current state, assumptions, risks, and exact files to be changed.
- Work incrementally. After each stage, build/test what is possible and report exact commands and results.
- Never claim a hardware test passed unless it was actually run on the target hardware.

## 2. Project root and source projects
Target project root:
`E:\embedded\双路 ISP 采集与双画面单路推流重构\RK3588 动物检测推理`

Read-only source project A:
`E:\embedded\双路 ISP 采集与双画面单路推流重构\多路视频`

Read-only source project B:
`E:\embedded\双路 ISP 采集与双画面单路推流重构\多路视频\demo_multhread_decode_infer_mulmodel`

Style/reference repository:
`https://github.com/JoeChen2me/RK-MediaProject`

Do not modify, rename, delete, clean, or reformat either source project. Do not run destructive Git commands in any source or target repository. The target project is a new integration project. Preserve source provenance in `docs/migration/source_audit.md`.

## 3. Language boundary
- Linux kernel drivers and Device Tree are C / DTS only. Never write C++ in kernel space.
- User-space services use C++17 by default; C++20 only when the installed compiler and target sysroot support it.
- C APIs from V4L2, Rockchip MPP, RGA, RKNN, ZLMediaKit, SocketCAN, IIO, TTY, and Linux system calls must be wrapped in RAII C++ classes.
- Use CMake for user-space builds. Keep kernel/BSP patches separate from CMake.

## 4. Reference style
Use `JoeChen2me/RK-MediaProject` as a style and architecture reference, not as a source to copy blindly:
- Google-derived `.clang-format`: 4 spaces, no tabs, 100-column limit, preserved include order, aligned assignments/declarations, custom brace wrapping with opening braces on new lines.
- C++ source/header separation (`src/` and `include/`).
- Subsystem-oriented classes such as camera, MPP decoder/encoder, RGA, publisher.
- Class methods should be explicit and action-oriented. Follow naming already present in the source project when migrating code; do not gratuitously rename public interfaces.
- Constants use `kPascalCase`; private members use a consistent trailing underscore in new code; types use PascalCase; functions/methods use PascalCase for class APIs and lower_snake_case for free functions. Document deviations where compatibility requires them.
- Prefer RAII, `std::unique_ptr`, `std::shared_ptr` only where shared ownership is real, `std::atomic`, `std::mutex`, `std::condition_variable`, bounded queues, and explicit ownership contracts.
- No raw owning pointers in new user-space code.
- Avoid global mutable state and hard-coded device paths/configuration.

## 5. Engineering rules
- First audit both source projects and reference repository. Produce a migration matrix before copying or rewriting anything.
- Preserve the existing working V4L2 -> MPP -> RGA -> MPP encode -> ZLMediaKit pipeline where it is correct.
- Preserve the second project's multi-thread/multi-model inference logic only after understanding model input/output contracts, tensor layouts, RKNN context ownership, and buffer lifetime.
- The product behavior is two camera inputs, optional side-by-side composition, and one encoded/published composite stream by default. Also allow independent per-camera streams as a configurable mode.
- Never assume the selected RK3588 board, MIPI topology, sensor model, I2C bus, GPIO, clock, regulator, lane count, ISP routing, or pinmux. Hardware is TBD. Put all such values in board profiles and mark them UNVERIFIED until backed by schematic/BSP.
- Do not invent sensor models, model files, calibration coefficients, measured performance, or test results.
- No fake hardware backends presented as real. A mock backend must be clearly labelled and isolated.
- Hardware-dependent features must have interfaces, configuration schema, diagnostics, and mock/unit tests where practical.
- Prefer existing in-kernel drivers/subsystems for MCP2515, RTC, EEPROM, GPIO keys, PWM, ADC, watchdog, USB, Ethernet and Wi-Fi. Write custom drivers only when there is a justified gap.
- MLX90640 custom I2C driver must be Linux kernel C. Do not use `miscdevice`; implement a standard char device only if the chosen interface is not better represented by V4L2 or IIO. Document why. A V4L2 thermal capture interface is a valid alternative and must be evaluated before implementation.
- HTU21D custom IIO driver must be Linux kernel C, with CRC and conversion handling. First inspect the target kernel for an existing driver to avoid duplicate registration.
- Camera Sensor drivers, V4L2 subdev, Media Controller, CSI/D-PHY/ISP graph and Device Tree are BSP-specific. Do not fabricate a complete DTS from generic examples.
- Respect DMA-BUF ownership, fences/synchronization, cache coherency, MPP task lifecycle, RGA import/release and V4L2 QBUF/DQBUF semantics. Never recycle a buffer while a downstream consumer still holds it.
- Queues must be bounded. Define backpressure and drop policies per pipeline edge.
- Use monotonic timestamps internally; preserve sensor/hardware timestamps where available. Store wall-clock time separately.
- Thread shutdown must be deterministic: stop source, wake waiters, drain or cancel queues, release downstream references, then destroy hardware contexts.
- Errors must propagate as typed status/results and be visible in logs/metrics.
- Never embed secrets, credentials, Wi-Fi passwords, private keys, or production tokens in source/config examples.
- Network services must bind to configurable interfaces/ports and document firewall/authentication expectations.
- OTA must include integrity/signature verification, version checks, rollback/recovery strategy and explicit board-specific boot partition integration. Do not implement a dangerous blind `scp && reboot`.

## 6. Quality gates
- `clang-format` for user-space C/C++.
- `clang-tidy` where available.
- Compiler warnings enabled; do not suppress warnings globally.
- Unit tests for pure logic and parsers.
- Integration tests with mock devices where feasible.
- Hardware test scripts for target board; label unexecuted tests clearly.
- Add README, architecture, data-flow, device-tree, driver, build, deployment, troubleshooting, performance and test documents.
- Each feature must state: status (`implemented`, `stub`, `planned`, `hardware-blocked`), dependencies, how to configure, how to test, and known limitations.

---

## 项目状态附录（本工程执行记录，非规则）

- 执行任务书：`RK3588_动物检测推理_Codex开发交接包/CODEX_MASTER_TASK.md`。
- 当前阶段：Phase 0（审计）与 Phase 1（骨架）已按第 16 节执行完毕；Phase 2+ 待用户确认迁移矩阵后继续。
- 状态标记约定：`implemented` / `stub` / `planned` / `hardware-blocked`，硬件结论一律标注 `UNVERIFIED` 直到有原理图/BSP/实测支撑。

### Phase 0/1 实测记录（2026-11-06）

- 审计产出：`docs/migration/{source_audit,module_mapping,risk_register}.md`、
  `docs/architecture/{initial_architecture,data_flow,thread_model,buffer_lifecycle}.md`、
  `docs/decisions/ADR-000{1,2,3}-*.md`；子审计存档 `docs/migration/_audit_reports/`。
- 构建（Windows + VS2022 MSVC 19.44，/W4 /permissive-，Ninja）：全部目标编译通过，0 告警。
- 单元测试：`ctest` 5/5 套件通过（bounded_queue / timestamp_aligner / modbus_crc /
  config / metrics）。
- 示例运行：`build\bin\animal_monitor_app.exe --config configs\default.yaml`
  启动 MOCK 流水线，2 秒自动退出，退出码 0；统计 produced=33 consumed=33 dropped=0 queue_left=0。
- 未做任何目标板硬件测试（无板卡）；所有硬件项保持 `hardware-blocked` / `UNVERIFIED`。
