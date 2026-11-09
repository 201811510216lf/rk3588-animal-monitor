# MLX90640 自定义 I2C 驱动（设计备忘）

> 状态：planned（hardware-blocked：板卡/内核/BSP 未定）。语言：**内核 C**。禁止 miscdevice。

## 待评估的接口选择（实现前必须先决策并记录 ADR）

| 方案 | 优点 | 缺点 |
|---|---|---|
| V4L2 热成像捕获（video-i2c 路线） | 与视频链统一（QBUF/DQBUF/流控）；生态兼容 | 需评估 video-i2c 对 32×24 RAW 的支持与维护成本 |
| 标准字符设备（cdev） | 简单直接；固定宽度 UAPI 可控 | 需要自定义 UAPI/流控/多 reader 语义 |

## 功能要求（源自任务书 §7.1）

1. 读取 EEPROM 校准参数并验证提取过程（禁止硬编码校准）。
2. 按协议读两个 subpage，检查 subpage/frame 状态，组成 32×24 完整热场。
3. I2C 分块、传输长度、字节序、错误重试、超时、设备移除处理。
4. 输出原始数据与校准后温度数据，单位定义明确。
5. UAPI（若 cdev）：固定宽度结构 + ABI 版本 + frame_id + timestamp + pixel_count +
   温度单位 + 状态位；ioctl 编号防冲突；read/poll/nonblock 行为明确；
   多 reader 语义明确；remove 时唤醒阻塞 reader，防 UAF。
6. 复杂温度算法放用户态（内核只做采集与搬运）。

## 验证脚本

- scripts/test_i2c.sh（总线探测与分块读测试，板卡到位后执行）。
