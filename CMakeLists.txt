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
    # Phase 2 可移植核心（host 可编译）
    src/media/frame_synchronizer.cpp
    src/media/frame_compositor.cpp
    src/media/mock_video_source.cpp
    src/media/mock_video_encoder.cpp
    src/media/null_video_publisher.cpp
    # Phase 3 推理链可移植部分（host 可编译）
    src/common/sha256.cpp
    src/inference/model_manifest.cpp
    src/inference/yolov5_postprocess.cpp
    src/inference/model_registry.cpp
    src/inference/inference_scheduler.cpp
    src/inference/mock_inference_model.cpp
    # Phase 4 传感器纯数学（host 可编译）
    src/thermal/thermal_frame.cpp
    src/thermal/mlx90640_math.cpp
    src/environment/htu21d_math.cpp
    # Phase 5 标定与融合（host 可编译）
    src/calibration/roi_mapper.cpp
    src/calibration/temperature_calibrator.cpp
    src/fusion/animal_tracker.cpp
    src/fusion/individual_baseline.cpp
    src/fusion/anomaly_detector.cpp
    src/fusion/feature_fusion.cpp
    # Phase 6 外设可移植部分（host 可编译）
    src/communication/modbus_rtu.cpp
    src/device/peripheral_manager.cpp
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
    src/app/video_pipeline_service.cpp
)
target_link_libraries(animal_monitor_app PRIVATE animal_monitor_core)

# 流水线工厂二选一：host 用 MOCK 实现，目标板用 V4L2/MPP/ZLM 真实实现
if(ANIMAL_MONITOR_ENABLE_TARGET_DEPS)
    target_sources(animal_monitor_app PRIVATE src/app/pipeline_factory_target.cpp)
else()
    target_sources(animal_monitor_app PRIVATE src/app/pipeline_factory_host.cpp)
endif()

# ============================================================
# Rockchip 依赖探测（默认关闭；打开后缺失则报错，见 Find 模块）
# ============================================================
if(ANIMAL_MONITOR_ENABLE_TARGET_DEPS)
    find_package(RockchipMPP REQUIRED)
    find_package(RockchipRGA REQUIRED)
    find_package(Rknn REQUIRED)
    find_package(Zlm REQUIRED)
    find_library(LIBGPIOD_LIBRARY NAMES gpiod libgpiod)
    if(NOT LIBGPIOD_LIBRARY)
        message(FATAL_ERROR "gpio_client 需要 libgpiod（gpiod），请安装开发包（UNVERIFIED 路径）")
    endif()

    # Phase 2 目标板媒体适配器（V4L2/MPP/RGA/ZLM；未经板卡编译验证）
    target_sources(animal_monitor_core PRIVATE
        src/camera/v4l2_camera.cpp
        src/media/mpp_decoder.cpp
        src/media/mpp_encoder.cpp
        src/media/rga_processor.cpp
        src/media/stream_publisher.cpp
    )
    # Phase 3 目标板推理运行时
    target_sources(animal_monitor_core PRIVATE
        src/inference/rknn_runtime.cpp
    )
    # Phase 4 传感器客户端（目标板）
    target_sources(animal_monitor_core PRIVATE
        src/thermal/mlx90640_client.cpp
        src/environment/htu21d_client.cpp
    )
    # Phase 6 外设客户端（目标板；优先复用内核驱动，UNVERIFIED）
    target_sources(animal_monitor_core PRIVATE
        src/communication/can_client.cpp
        src/communication/rs485_modbus_client.cpp
        src/device/gpio_client.cpp
        src/device/pwm_client.cpp
        src/device/adc_client.cpp
        src/device/rtc_client.cpp
        src/device/eeprom_client.cpp
        src/device/watchdog_client.cpp
    )

    target_link_libraries(animal_monitor_core PUBLIC
        RockchipMPP::rockchip_mpp
        RockchipRGA::librga
        Rknn::rknnrt
        Zlm::mk_api
        Zlm::ZLToolKit
        v4l2
        ${LIBGPIOD_LIBRARY}
    )
    # rknn_runtime.h 以该宏守卫（#error）；对 core 及依赖它的目标均可见。
    target_compile_definitions(animal_monitor_core PUBLIC ANIMAL_MONITOR_ENABLE_TARGET_DEPS=1)
    message(STATUS "Target deps enabled: MPP=${ROCKCHIP_MPP_INCLUDE_DIR} "
                   "RGA=${ROCKCHIP_RGA_INCLUDE_DIR} RKNN=${RKNN_INCLUDE_DIR} "
                   "ZLM=${ZLM_INCLUDE_DIR} gpiod=${LIBGPIOD_LIBRARY}")
endif()

# ============================================================
# 单元测试（host）
# ============================================================
if(ANIMAL_MONITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
