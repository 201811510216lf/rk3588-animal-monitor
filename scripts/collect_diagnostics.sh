#!/bin/bash
# collect_diagnostics.sh — 目标板诊断信息收集（只读）
# 用法: 板端 ./collect_diagnostics.sh [输出目录]
set -euo pipefail
OUT="${1:-./diagnostics}"
mkdir -p "${OUT}"

echo "== 内核与系统 =="        | tee    "${OUT}/00_system.txt"
uname -a                      | tee -a "${OUT}/00_system.txt"
cat /proc/version             | tee -a "${OUT}/00_system.txt"

echo "== 设备清单 =="           | tee    "${OUT}/01_devices.txt"
ls -l /dev/video* /dev/i2c-* /dev/spidev* /dev/tty* /dev/gpiochip* /dev/watchdog* 2>/dev/null | tee -a "${OUT}/01_devices.txt"

echo "== 温度/频率 =="          | tee    "${OUT}/02_thermal.txt"
cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | tee -a "${OUT}/02_thermal.txt"

echo "== dmesg(最近200行) =="  | tee    "${OUT}/03_dmesg.txt"
dmesg | tail -n 200          | tee -a "${OUT}/03_dmesg.txt"

echo "完成: ${OUT}/"
