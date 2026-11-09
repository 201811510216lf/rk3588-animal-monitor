# device 模块状态

> 状态约定：`implemented` / `stub` / `planned` / `hardware-blocked`；硬件结论一律
> 标注 `UNVERIFIED` 直到有原理图/BSP/板卡实测支撑（AGENTS.md 约定）。

## 组件状态表

| 组件 | 状态 | 编译条件 | 依赖 | 验证方式 |
|---|---|---|---|---|
| `peripheral_manager`（外设状态注册表，纯逻辑） | implemented（已测） | host | 无（std 库） | `ctest -R test_peripheral_manager` |
| `gpio_client`（libgpiod v1） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建（`ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON`） | libgpiod v1（`<gpiod.h>`，链接 `-lgpiod`）；内核 gpio character device | 板卡到位后 `scripts/test_gpio.sh`（脚本尚未创建） |
| `pwm_client`（sysfs PWM） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建 | PWM framework + `CONFIG_PWM_SYSFS`（sysfs 为过渡方案，新内核推荐 chardev） | 板卡到位后 `scripts/test_pwm.sh`（示波器/蜂鸣器验证，脚本尚未创建） |
| `adc_client`（IIO sysfs） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建 | IIO ADC 驱动（内核设备树绑定） | 板卡到位后 `scripts/test_adc.sh`（脚本尚未创建） |
| `rtc_client`（RTC class） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建 | RTC class + 内核 RTC 驱动（如 DS3231） | 板卡到位后 `scripts/test_rtc.sh`（脚本尚未创建） |
| `eeprom_client`（at24 sysfs） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建 | at24 驱动 + I2C 设备树节点（只读标志由设备树决定） | 板卡到位后 `scripts/test_eeprom.sh`（脚本尚未创建） |
| `watchdog_client`（/dev/watchdog） | implemented（初稿）/ hardware-blocked / UNVERIFIED | 仅目标构建 | watchdog core（SoC watchdog 驱动） | 板卡到位后 `scripts/test_watchdog.sh`（喂狗/超时复位验证，脚本尚未创建） |

## 关键决策与已知限制

- **全部路径/编号配置化**：chip 路径、GPIO 行号、PWM 通道、IIO 设备名、EEPROM sysfs
  路径均通过构造参数（板卡配置）传入；仅 `/dev/rtc0`、`/dev/watchdog` 提供 Linux
  标准节点默认值（非板卡专属编号，仍可配置覆盖）。**本仓库不伪造任何板卡 GPIO/I2C/
  PWM 编号**，所有板卡值待原理图/BSP 确认（UNVERIFIED）。
- **复用内核驱动**：不写自定义 GPIO/PWM/ADC/RTC/EEPROM/Watchdog 驱动，全部走既有
  内核框架（任务书 §8 策略）。
- **去抖与电气配置**：`GpioClient` 不配置去抖/上下拉/边沿策略，由内核（设备树）与
  上层策略决定；`WaitForEvent` 仅做 `gpiod_line_event_wait`（IRQ 唤醒用途），事件内容
  读取可后续扩展 `gpiod_line_event_read`。
- **PWM 顺序约束**：必须先写 period 再写 duty_cycle（内核约束 duty ≤ period）。
- **EEPROM 寿命**：at24 无磨损均衡，写合并/日志式存储策略由上层控制；本客户端仅裸读写。
- **看门狗安全**：打开后必须周期性 KeepAlive；Close 写 'V' 魔术字仅用于正常关停，
  关闭前必须确认系统 watchdog 守护已接管，否则系统复位（见头文件警告）。
- **Unix 秒按 UTC 解释**：RTC 时间换算用 days_from_civil（Howard Hinnant 算法），
  不依赖时区；若 RTC 存本地时间需上层转换。

## 板卡到位后的硬件自检（尚未执行）

1. `ls /dev/gpiochip* /sys/class/pwm/pwmchip* /sys/bus/iio/devices/ /dev/rtc* /dev/watchdog`。
2. EEPROM：`cat /sys/bus/i2c/devices/*/name` 确认 at24 绑定与节点地址。
3. 创建并执行 `scripts/test_gpio.sh`、`test_pwm.sh`、`test_adc.sh`、`test_rtc.sh`、
   `test_eeprom.sh`、`test_watchdog.sh`；结果回填本表并摘除 UNVERIFIED 标记。
