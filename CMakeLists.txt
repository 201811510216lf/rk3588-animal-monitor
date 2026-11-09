cmake_minimum_required(VERSION 3.16)

project(animal_monitor
    VERSION 0.1.0
    DESCRIPTION "RK3588 Animal Behavior & Thermal Monitoring Platform"
    LANGUAGES CXX
)

# ============================================================
# 全局设置
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 输出目录：build/bin、build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ============================================================
# 选项
# ============================================================
# host 构建默认 OFF：不探测 Rockchip SDK，保证核心单元测试可在任意主机运行
option(ANIMAL_MONITOR_ENABLE_TARGET_DEPS
    "Enable Rockchip MPP/RGA/RKNN/ZLMediaKit dependencies (target builds)" OFF)
option(ANIMAL_MONITOR_BUILD_TESTS "Build unit tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================
# 编译告警（不得全局抑制告警）
# ============================================================
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

# ============================================================
# 核心库（host 可编译，不依赖 Rockchip SDK）
# ============================================================
add_library(animal_monitor_core STATIC
    src/common/status.cpp
    src/common/modbus_crc.cpp
    src/common/timestamp_aligner.cpp
    src/common/frame_meta.cpp
    src/logging/logger.cpp
    src/metrics/metrics.cpp
    src/config/config_loader.cpp
)

target_include_directories(animal_monitor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(animal_monitor_core PUBLIC Threads::Threads)

# ============================================================
# 应用
# ============================================================
add_executable(animal_monitor_app
    src/main.cpp
    src/app/application.cpp
    src/app/mock_pipeline_service.cpp
)
target_link_libraries(animal_monitor_app PRIVATE animal_monitor_core)

# ============================================================
# Rockchip 依赖探测（默认关闭；打开后缺失则报错，见 Find 模块）
# ============================================================
if(ANIMAL_MONITOR_ENABLE_TARGET_DEPS)
    find_package(RockchipMPP REQUIRED)
    target_link_libraries(animal_monitor_app PRIVATE RockchipMPP::rockchip_mpp)
    target_compile_definitions(animal_monitor_app PRIVATE ANIMAL_MONITOR_HAS_MPP=1)
    # RGA / RKNN / ZLMediaKit 的 Find 模块将在对应 Phase 迁移时补充（cmake/ 下预留）
    message(STATUS "Target deps enabled: RockchipMPP found at ${ROCKCHIP_MPP_INCLUDE_DIR}")
endif()

# ============================================================
# 单元测试（host）
# ============================================================
if(ANIMAL_MONITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
