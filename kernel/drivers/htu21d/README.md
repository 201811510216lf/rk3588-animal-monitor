# HTU21D 自定义 IIO 驱动（设计备忘）

> 状态：planned（hardware-blocked）。语言：**内核 C**。用户态经 IIO sysfs/buffer 读取。

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
