# Device Tree（UNVERIFIED）

> 板卡/Sensor/GPIO/I2C/SPI/UART/MIPI lane/clock/regulator 全部未定。
> **禁止从通用示例臆造完整 DTS**。本目录在板卡确定后用于：
>
> 1. `board-template/`：基于真实 BSP 基板 DTS 的增量修改说明（先有 BSP 再谈 DTS）。
> 2. `overlays/`：MLX90640/HTU21D 等外设的 overlay 模板（总线/地址以原理图为准）。
>
> Camera Sensor 驱动、V4L2 subdev、Media Controller、CSI/D-PHY/ISP graph 均为 BSP 专属，
> 由板卡厂商 SDK 提供或按官方文档适配，不自行虚构。
