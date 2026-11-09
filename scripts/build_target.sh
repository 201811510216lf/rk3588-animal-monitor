#!/bin/bash
# build_target.sh — RK3588 aarch64 目标构建（模板，UNVERIFIED）
# 用法: ./scripts/build_target.sh [工具链前缀] [SDK根目录]
# 实际工具链三元组/ sysroot 以板卡 BSP 为准，禁止照抄默认值。
set -euo pipefail

PREFIX="${1:-aarch64-linux-gnu}"
RK_SDK_ROOT="${2:-}"

CMAKE_ARGS=(
  -S .
  -B build-rk
  -G Ninja
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3588-aarch64-linux-gnu.cmake
  -DRK_TOOLCHAIN_PREFIX="${PREFIX}"
  -DANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON
)
[ -n "${RK_SDK_ROOT}" ] && CMAKE_ARGS+=("-DRK_SDK_ROOT=${RK_SDK_ROOT}")

cmake "${CMAKE_ARGS[@]}"
cmake --build build-rk -j"$(nproc)"
echo "产物: build-rk/bin/animal_monitor_app"
