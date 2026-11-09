#!/bin/bash
# test_camera.sh — V4L2 相机能力与流测试（硬件测试；当前 hardware-blocked）
# 用法: 板端 ./test_camera.sh [设备]
set -euo pipefail
DEV="${1:-/dev/video0}"   # UNVERIFIED

for tool in v4l2-ctl ffprobe; do
    command -v "${tool}" >/dev/null || { echo "需要 ${tool}"; exit 1; }
done

echo "== 设备能力 =="
v4l2-ctl -d "${DEV}" --all

echo "== 采集 3 秒验证 =="
v4l2-ctl -d "${DEV}" --stream-mmap --stream-count=90 --stream-to=/tmp/cam_test.yuv
ls -l /tmp/cam_test.yuv

echo "注: ISP/合成/编码验证见 Phase 2 测试清单（docs/testing/，待补充）。"
