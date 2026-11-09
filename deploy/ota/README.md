# OTA 策略（planned，hardware-blocked）

> 板卡 bootloader/分区表未确定前**只做架构与接口**，不执行实际刷写。

## 分层升级

| 层级 | 内容 | 校验 |
|---|---|---|
| application | animal_monitor_app + 配置 | SHA256 + 签名 |
| model | 推理模型文件 | SHA256 + 版本兼容（RKNN SDK/driver 匹配） |
| system | rootfs/内核镜像 | SHA256 + 签名 + A/B 分区或可靠 recovery |

## 流程（设计）

1. 元数据获取（HTTPS）→ 版本/兼容检查 → 空间检查。
2. 下载（断点续传）→ SHA256 校验 → 数字签名验证。
3. 写入备用分区/暂存区 → 启动健康检查 → 启动确认（否则回滚）。
4. 任何一步失败：保留旧版本、告警上报；**禁止盲 scp && reboot**。

## 板卡绑定项（UNVERIFIED）

- A/B rootfs 分区方案、U-Boot boot flow、recovery 分区：以厂商 BSP 为准。
