# kernel/ — 内核驱动与 BSP

> 状态：`hardware-blocked`（板卡/BSP 未定）。目录仅承载设计、模板与文档；
> 不臆造任何 DTS/GPIO/I2C 总线/clock/regulator 信息。
> 内核代码一律 **C**（禁止 C++），与 CMake 构建分离。

## 目录

| 路径 | 内容 | 状态 |
|---|---|---|
| drivers/mlx90640/ | MLX90640 自定义 I2C 驱动设计文档（禁止 miscdevice；cdev vs V4L2 评估） | planned |
| drivers/htu21d/ | HTU21D 自定义 IIO 驱动设计文档（CRC/转换时间；先查目标内核已有驱动） | planned |
| dts/overlays/ | 驱动 overlay 模板（板卡确定后填写） | planned |
| dts/board-template/ | 板卡 DTS 增量模板 | planned |
| patches/ | 内核补丁（按需） | 空 |
| configs/ | 内核 defconfig 片段（按需） | 空 |

## 驱动策略（摘要）

- 复用优先：MCP2515 CAN → mcp251x(+fd)；RTC DS3231 → RTC class；24C64 → at24；
  GPIO/IRQ → gpiolib/gpio character device/gpio-keys；PWM → PWM framework；
  ADC → IIO SARADC；Watchdog → SoC watchdog + /dev/watchdog；
  USB/Ethernet/Wi-Fi → 板级 BSP 驱动与固件。
- 仅 MLX90640（I2C）与 HTU21D（IIO）为自定义驱动，实施前先调查目标内核是否已有
  video-i2c/MLX90640 与 HTU21 支持，避免重复绑定。
- 用户态接口：MLX90640 优先评估 V4L2 thermal capture，其次标准 cdev
  （固定宽度 UAPI、ABI 版本、frame_id、timestamp、单位、状态位，32/64 位兼容）；
  HTU21D 用 IIO channel + read_raw/scale。
