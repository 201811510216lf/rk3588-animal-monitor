#!/bin/bash
# test_rs485.sh — RS485/Modbus RTU 测试（硬件测试；当前 hardware-blocked）
# 用法: 板端 ./test_rs485.sh [TTY] [波特率]
set -euo pipefail
TTY="${1:-/dev/ttyS0}"   # UNVERIFIED；RS485 方向控制以 BSP UART 驱动为准
BAUD="${2:-9600}"

echo "== 配置 ${TTY} @ ${BAUD} (RS485) =="
sudo stty -F "${TTY}" "${BAUD}" raw -echo -echoe -echok -echoctl

echo "== 发送读保持寄存器请求 01 03 00 00 00 0A (CRC C5 CD) =="
printf '\x01\x03\x00\x00\x00\x0A\xC5\xCD' | sudo tee "${TTY}" > /dev/null
echo "已发送；请在上位机/从机侧核对响应（CRC 校验见 tests/unit/test_modbus_crc.cpp）。"

echo "注: TIOCSRS485 半双工方向切换由 user-space rs485_service 配置（Phase 6）。"
