# HTU21D 自定义 IIO 驱动

> 状态：implemented（初稿）/ hardware-blocked（未在目标内核编译验证，UNVERIFIED）。语言：**内核 C**。用户态经 IIO sysfs 读取。

## 接口决策（已定，2026-11-09）

- **IIO 路线**：2 通道（IIO_TEMP / IIO_HUMIDITYRELATIVE），INDIO_DIRECT_MODE；
  `read_raw` 发起单次测量（0xE3 温度 no-hold / 0xE5 湿度 no-hold），等待保守转换时间
  （T 85ms / RH 50ms，数据手册最大值），读 3 字节并 CRC-8 校验（失败返回 -EIO 并计数）。
- RAW（IIO_VAL_INT）+ SCALE（175.72/65536、125/65536，INT_PLUS_NANO）；
  常数项（-46.85/-6）并入公式；另提供 IIO_INFO_PROCESSED 便捷路径（毫单位）。
  分辨率寄存器（0xE6）probe 写默认 0x02（RH12/T14）。
- 不设计 IIO trigger/buffer（周期采样需求确认后再扩展，避免过度设计）。

## 实施前检查

1. 目标内核是否已启用/包含现有 HTU21 驱动（htu21.c）——避免重复设备绑定。
2. 器件实际型号确认（HTU21D vs SI7021 兼容性、分辨率寄存器）。

## 功能要求（源自任务书 §7.2）

1. 温度/湿度命令、CRC 校验（读失败计数与重试）、分辨率/转换时间处理。
2. IIO channels：in_temp_raw/in_temp_scale、in_humidityrelative_raw/scale、processed value。
3. 周期采样需求确认后再设计 IIO buffer（indio_dev + trigger），避免过度设计。

## 用户态消费

- EnvironmentService（src/environment/htu21d_client.cpp，Phase 4）读 IIO sysfs/buffer。
- 环境温湿度作为模型输入/补偿特征，不宣称单靠 HTU21D 修正 MLX90640 测温误差。

## 交付内容（Phase 4 初稿）

| 文件 | 说明 |
|---|---|
| `htu21d.c` / `htu21d.h` | IIO 驱动：单次测量 + CRC-8（多项式 0x131/初值 0/MSB 先）+ 失败计数；SCALE/PROCESSED；devm 资源 |
| `Kconfig` / `Makefile` | CONFIG_HTU21D_IIO；**与主线 SENSORS_HTU21（drivers/hwmon/htu21.c）二选一** |
| 用户态 | `include/animal_monitor/environment/htu21d_math.h`（CRC8 + Rh/T 公式，host 可测）、`htu21d_client.{h,cpp}`（IIO sysfs 客户端，仅目标构建编译） |

## 验证方法（板卡到位后）

1. 目标内核检查：`grep -r SENSORS_HTU21 <内核配置>`，确认 hwmon htu21 与本驱动不同时启用。
2. 总线探测（scripts/test_i2c.sh 扩展）：0x40 应答、软复位后读用户寄存器（0xE6）。
3. 驱动装载后：`cat /sys/bus/iio/devices/iio:device*/name` = "htu21d"；
   `cat in_temp_raw in_temp_scale`、`cat in_humidityrelative_raw in_humidityrelative_scale`，
   验证 CRC 失败计数（dmesg / 内部计数）。
4. 用户态 host 单元测试：`tests/unit/test_htu21d_math.cpp`
   （CRC 锚点 (0,0)→0 与数据手册示例 0x683A→0x7C、0x4E85→0x6B；Rh/T 端点；
   MSVC /W4 已验证 7/7 通过）。
5. 精度验证须用标准湿度源/参考温度计比对，未比对前不宣称校准精度（UNVERIFIED）。

## 遗留问题（板卡到位后处理）

- 器件型号确认（HTU21D 与 Si7021 兼容性、分辨率寄存器默认值）。
- sysfs 路径（iio:deviceN 编号、PROCESSED 形态下 in_*_input 的呈现方式）以目标
  内核实际导出为准；客户端已做 raw 优先 + input 回退，仍须实测确认（UNVERIFIED）。
- CRC 校验失败当前直接 -EIO + 计数，重试策略（1 次重读）待板卡实测决定。
- 分辨率动态修改（IIO 扩展属性）不在初稿范围。
