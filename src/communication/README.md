# communication 模块状态

> 状态约定：`implemented` / `stub` / `planned` / `hardware-blocked`；硬件结论一律
> 标注 `UNVERIFIED` 直到有原理图/BSP/板卡实测支撑（AGENTS.md 约定）。

## 组件状态表

| 组件 | 状态 | 编译条件 | 依赖 | 验证方式 |
|---|---|---|---|---|
| `modbus_rtu`（帧编解码，纯逻辑） | implemented（已测） | host | `common/modbus_crc.h`（复用已测 CRC） | `ctest -R test_modbus_rtu` |
| `can_frame`（CAN 帧模型，header-only） | implemented（已测） | host | 无 | `ctest -R test_can_frame` |
| `can_client`（SocketCAN CAN_RAW 客户端） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建（`ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON`） | Linux SocketCAN（CONFIG_CAN）；板卡侧需内核 mcp251x/mcp251x_fd 驱动 + 设备树 can0 节点 | 板卡到位后 `scripts/test_can.sh`（`ip link set can0 up` + `cansend`/`candump` 回环，脚本尚未创建） |
| `rs485_modbus_client`（RS485 半双工 Modbus RTU 主站） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建 | UART + 内核 RS485 模式（`TIOCSRS485`，`<linux/serial.h>`）；板卡侧需收发器与使能引脚设备树配置 | 板卡到位后 `scripts/test_rs485.sh`（对接真实 Modbus 从站或 USB-RS485 回环，脚本尚未创建） |

## 关键决策与已知限制

- **CRC 字节序**：Modbus RTU 线上 CRC 低字节在前，统一由 `AppendModbusCrc`/
  `VerifyModbusCrc` 处理（与 `test_modbus_crc` 权威向量 01 03 00 00 00 0A → C5 CD 一致）。
- **从站/功能码一致性**：`ParseFrame` 拒绝从站地址 0（广播仅请求方向）、功能码低 7 位为 0、
  异常帧数据区非 1 字节的帧；`Transact` 对从站/功能码不匹配的响应直接报错不重试。
- **RS485 事务策略**：按 Modbus 响应最小长度分段读取（从站+功能码 → 按功能码确定剩余
  长度：0x03 读字节数域、0x06 固定 6 字节、异常固定 3 字节、未知功能码按字节计数假设）；
  超时/EOF/CRC/长度错误自动重试一次。帧间静默间隔（3.5 字符）依赖内核串口驱动与
  主站串行调度，用户态不自行计算。
- **TIOCSRS485 失败非致命**：部分板卡收发器自带自动方向控制；失败记录到
  `LastRs485Warning()` 供诊断。
- **CAN 仅经典帧**：不支持 CAN FD；`can_client.cpp` 使用 `can_frame.len8_dlc` 字段，
  目标内核需 ≥ 5.4 的 UAPI 头（RK3588 BSP 满足；更老头文件需去掉该字段）。
- 所有接口名/串口路径/波特率均由构造参数（板卡配置）传入，本仓库不含任何板卡编号
  默认值（UNVERIFIED）。

## 板卡到位后的硬件自检（尚未执行）

1. `dmesg | grep -E "mcp251|can0"` 确认 CAN 驱动绑定；`ip -details link show can0`。
2. RS485：核对设备树 uart 节点 `rs485-rts-active-low` 等属性与收发器方向控制。
3. 创建并执行 `scripts/test_can.sh`、`scripts/test_rs485.sh`（含 Modbus 从站联调）。
