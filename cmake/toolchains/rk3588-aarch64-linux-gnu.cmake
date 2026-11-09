# RK3588 aarch64 Linux 交叉编译工具链模板
#
# 状态：UNVERIFIED —— 板卡与 BSP 尚未确定，编译器名称/前缀/sysroot 均为常见默认值，
# 使用前必须按实际板卡 SDK 修正（编译器三元组可能为 aarch64-rockchip-linux-gnu、
# aarch64-none-linux-gnu、aarch64-buildroot-linux-gnu 等）。
#
# 用法：
#   cmake -S . -B build-rk -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3588-aarch64-linux-gnu.cmake \
#         -DANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON -DRK_SDK_ROOT=/path/to/bsp/sdk
#
# 建议通过 CMakePresets.json 的 target-rk3588 preset 使用，并配合缓存变量覆盖：
#   -DRK_TOOLCHAIN_PREFIX=aarch64-rockchip-linux-gnu
#   -DRK_SYSROOT=/path/to/sysroot

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ---- 编译器（可用 -DRK_TOOLCHAIN_PREFIX=... 覆盖） ----
if(NOT DEFINED RK_TOOLCHAIN_PREFIX)
    set(RK_TOOLCHAIN_PREFIX "aarch64-linux-gnu" CACHE STRING
        "Cross compiler prefix (UNVERIFIED: adjust to the actual board SDK)")
endif()

find_program(RK_CC  NAMES ${RK_TOOLCHAIN_PREFIX}-gcc)
find_program(RK_CXX NAMES ${RK_TOOLCHAIN_PREFIX}-g++)

if(NOT RK_CC OR NOT RK_CXX)
    message(FATAL_ERROR
        "Cross compiler '${RK_TOOLCHAIN_PREFIX}-gcc/g++' not found. "
        "Install the BSP toolchain and/or set RK_TOOLCHAIN_PREFIX to the real triplet.")
endif()

set(CMAKE_C_COMPILER   ${RK_CC})
set(CMAKE_CXX_COMPILER ${RK_CXX})

# ---- sysroot（可选，UNVERIFIED） ----
if(DEFINED RK_SYSROOT)
    set(CMAKE_SYSROOT ${RK_SYSROOT})
    set(CMAKE_FIND_ROOT_PATH ${RK_SYSROOT})
endif()

# ---- Rockchip SDK 根目录（Find 模块使用） ----
set(RK_SDK_ROOT "" CACHE PATH
    "Root of the Rockchip BSP SDK (contains mpp/rga/rknn headers and libs), UNVERIFIED")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
