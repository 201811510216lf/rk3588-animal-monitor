#!/bin/bash
# test_i2c.sh — I2C 总线与传感器测试（硬件测试，板卡到位后执行；当前 hardware-blocked）
# 用法: 板端 ./test_i2c.sh [总线] [地址]
set -euo pipefail
BUS="${1:-2}"          # UNVERIFIED：总线号以原理图/BSP 为准
ADDR="${2:-0x33}"      # UNVERIFIED：MLX90640 默认 0x33，以原理图为准

command -v i2cdetect >/dev/null || { echo "需要 i2c-tools"; exit 1; }

echo "== 总线探测 (i2c-${BUS}) =="
i2cdetect -y -r "${BUS}"

echo "== 读 MLX90640 EEPROM 前 16 字节 (地址 ${ADDR}) =="
i2cget -y "${BUS}" "${ADDR}" 0x24 i 16 2>/dev/null || echo "读取失败（检查器件/地址）"

echo "注: 完整 subpage/校准验证待驱动与脚本 Phase 4 补齐。"
