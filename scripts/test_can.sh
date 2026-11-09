#!/bin/bash
# test_can.sh — SocketCAN 双节点收发测试（硬件测试；当前 hardware-blocked）
# 用法: 板端 ./test_can.sh [接口] [比特率]
set -euo pipefail
IFACE="${1:-can0}"        # UNVERIFIED
BITRATE="${2:-500000}"

command -v cansend >/dev/null || { echo "需要 can-utils"; exit 1; }

echo "== 配置 ${IFACE} @ ${BITRATE} =="
sudo ip link set "${IFACE}" down 2>/dev/null || true
sudo ip link set "${IFACE}" type can bitrate "${BITRATE}" restart-ms 100
sudo ip link set "${IFACE}" up

echo "== 自发自收 10 帧（需总线另一端有节点/回环接线，以实际接线为准）=="
sudo candump "${IFACE}" -n 10 > /tmp/candump.log 2>&1 &
DUMP_PID=$!
sleep 1
for i in $(seq 1 10); do
    cansend "${IFACE}" "123#DEADBEEF$(printf '%02X' "$i")"
    sleep 0.1
done
wait "${DUMP_PID}"
cat /tmp/candump.log

echo "注: 错误恢复/长期收发见 docs/testing/（待补充）。"
