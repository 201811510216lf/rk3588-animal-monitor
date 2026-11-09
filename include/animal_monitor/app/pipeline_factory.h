#pragma once

#include <memory>

#include "animal_monitor/camera/video_source.h"
#include "animal_monitor/config/config_loader.h"
#include "animal_monitor/media/video_encoder.h"
#include "animal_monitor/media/video_publisher.h"

namespace animal_monitor {

// 流水线组件工厂：按配置创建视频源/编码器/发布器。
// host 构建返回 MOCK 实现（pipeline_factory_host.cpp）；
// 目标构建（ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON）返回真实实现
// （pipeline_factory_target.cpp，V4L2/MPP/ZLM，UNVERIFIED）。
std::unique_ptr<VideoSource> CreateVideoSource(const config::ConfigValue& camera_config,
                                               int camera_id);

std::unique_ptr<VideoEncoder> CreateVideoEncoder(const EncoderConfig& config);

std::unique_ptr<VideoPublisher> CreateVideoPublisher(const VideoPublisher::Config& config);

}  // namespace animal_monitor
