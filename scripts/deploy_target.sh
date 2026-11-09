#!/bin/bash
# deploy_target.sh — 手工部署模板（非破坏性；无刷写/无自动重启）
# 用法: ./scripts/deploy_target.sh <设备IP> [SSH用户]
# 仅复制应用与配置；不执行 systemctl restart，不执行任何 OTA/刷写。
set -euo pipefail

DEVICE_IP="${1:?用法: deploy_target.sh <设备IP> [用户]}"
SSH_USER="${2:-root}"

BIN="build-rk/bin/animal_monitor_app"
CONF="configs/default.yaml"

[ -f "${BIN}" ] || { echo "未找到 ${BIN}，请先 build_target.sh"; exit 1; }

scp "${BIN}" "${SSH_USER}@${DEVICE_IP}:/usr/local/bin/"
ssh "${SSH_USER}@${DEVICE_IP}" "mkdir -p /etc/animal_monitor"
scp "${CONF}" "${SSH_USER}@${DEVICE_IP}:/etc/animal_monitor/"

echo "已复制。启动/重启由人工执行（禁止自动重启目标设备）:"
echo "  ssh ${SSH_USER}@${DEVICE_IP} 'systemctl restart animal-monitor'  # 仅当已安装 unit"
