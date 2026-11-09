// 文件名：src/app/pipeline_factory_target.cpp
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）。
// 说明：目标构建的流水线工厂：v4l2 源 → V4l2Camera，编码器 → MppEncoder，
//       发布器 → StreamPublisher；mock 源在目标板同样可用（便于分步联调）。
//       仅在 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与构建（与 host 工厂二选一）。
#include "animal_monitor/app/pipeline_factory.h"

#include <cstdint>
#include <memory>
#include <string>

#include "animal_monitor/camera/v4l2_camera.h"
#include "animal_monitor/media/mock_video_encoder.h"
#include "animal_monitor/media/mock_video_source.h"
#include "animal_monitor/media/mpp_encoder.h"
#include "animal_monitor/media/null_video_publisher.h"
#include "animal_monitor/media/stream_publisher.h"

namespace animal_monitor {

namespace {

std::int64_t AsInt(const config::ConfigValue& parent, const char* key, std::int64_t fallback)
{
    const config::ConfigValue* entry = parent.Find(key);
    std::int64_t                value = fallback;
    if (entry != nullptr)
    {
        entry->AsInt64(&value);
    }
    return value;
}

std::string AsString(const config::ConfigValue& parent, const char* key,
                     const std::string& fallback)
{
    const config::ConfigValue* entry = parent.Find(key);
    if (entry != nullptr && entry->IsScalar())
    {
        return entry->Scalar();
    }
    return fallback;
}

}  // namespace

std::unique_ptr<VideoSource> CreateVideoSource(const config::ConfigValue& camera_config,
                                               int camera_id)
{
    const std::string source = AsString(camera_config, "source", "v4l2");

    if (source == "mock")
    {
        MockVideoSource::Config config;
        config.width       = static_cast<std::uint32_t>(AsInt(camera_config, "width", 640));
        config.height      = static_cast<std::uint32_t>(AsInt(camera_config, "height", 480));
        config.fps         = static_cast<std::uint32_t>(AsInt(camera_config, "fps", 30));
        config.duration_ms = AsInt(camera_config, "duration_ms", 0);
        config.base_b = static_cast<std::uint8_t>(camera_id == 0 ? 128 : 0);
        config.base_g = static_cast<std::uint8_t>(camera_id == 0 ? 0 : 128);
        config.base_r = static_cast<std::uint8_t>(camera_id == 0 ? 0 : 64);
        config.name   = camera_id == 0 ? "mock_camera0" : "mock_camera1";
        return std::make_unique<MockVideoSource>(config);
    }

    if (source == "v4l2")
    {
        V4l2CameraConfig config;
        config.device   = AsString(camera_config, "device",
                                   camera_id == 0 ? "/dev/video0" : "/dev/video1");
        config.width    = static_cast<std::uint32_t>(AsInt(camera_config, "width", 1920));
        config.height   = static_cast<std::uint32_t>(AsInt(camera_config, "height", 1080));
        config.fps      = static_cast<std::uint32_t>(AsInt(camera_config, "fps", 30));
        config.exposure_ms = static_cast<std::int32_t>(AsInt(camera_config, "exposure_us", 0) /
                                                       1000);
        const std::string fmt = AsString(camera_config, "pixel_format", "nv12");
        config.pixel_format = (fmt == "mjpeg") ? PixelFormat::kMjpeg : PixelFormat::kNv12;
        config.buffer_count  = static_cast<std::uint32_t>(
            AsInt(camera_config, "buffer_count", 8));
        config.dma_heap_path  = AsString(camera_config, "dma_heap_path",
                                         "/dev/dma_heap/system");
        config.queue_capacity = static_cast<std::size_t>(
            AsInt(camera_config, "queue_capacity", 4));
        return std::make_unique<V4l2Camera>(config);
    }

    // 未知源类型：返回空，由 VideoPipelineService 报 kUnimplemented。
    return nullptr;
}

std::unique_ptr<VideoEncoder> CreateVideoEncoder(const EncoderConfig& config)
{
    auto encoder = std::make_unique<MppEncoder>();
    const Status status = encoder->Initialize(config);
    if (!status)
    {
        return nullptr;
    }
    return encoder;
}

std::unique_ptr<VideoPublisher> CreateVideoPublisher(const VideoPublisher::Config& config)
{
    auto publisher = std::make_unique<StreamPublisher>();
    const Status status = publisher->Initialize(config);
    if (!status)
    {
        return nullptr;
    }
    return publisher;
}

}  // namespace animal_monitor
