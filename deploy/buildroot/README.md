# Buildroot 集成（模板）

板卡 SDK 未确定。此处预留 Buildroot package 配方位置：

- `animal_monitor/`：package 配方（Config.in + .mk，版本/源码路径待定）。
- rootfs 配置与 systemd/init 脚本：见 deploy/systemd/。

**不伪造任何厂商 Buildroot 路径或板级配置**；板卡 BSP 到位后按其 external tree 结构落地。
