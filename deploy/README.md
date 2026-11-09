# deploy/ — 部署资产

| 目录 | 内容 | 状态 |
|---|---|---|
| systemd/ | animal-monitor.service 模板（ExecStart 以实际部署路径为准） | template |
| udev/ | 设备权限规则模板（视频/GPIO/看门狗等，以板卡 BSP 为准） | template |
| buildroot/ | Buildroot package 配方模板（板卡 SDK 确定后填写，不伪造厂商路径） | template |
| docker/ | 可选构建/测试容器 | planned |
| ota/ | OTA 策略文档（安全要求） | planned |

## 安全红线（OTA）

- 禁止盲 `scp && reboot`；必须 SHA256 + 数字签名校验、版本检查、空间检查、断点续传、
  健康检查与启动确认、失败回滚；A/B 分区/恢复方案以真实 bootloader/分区表为准。
- 未经用户确认，不执行任何破坏性操作、系统刷写、OTA、重启目标设备。
