#include "animal_monitor/app/pipeline_factory.h"

#include "animal_monitor/media/mock_video_encoder.h"
#include "animal_monitor/media/mock_video_source.h"
#include "animal_monitor/media/null_video_publisher.h"

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
    const std::string source = AsString(camera_config, "source", "mock");

    if (source == "mock")
    {
        MockVideoSource::Config config;
        config.width    = static_cast<std::uint32_t>(AsInt(camera_config, "width", 640));
        config.height   = static_cast<std::uint32_t>(AsInt(camera_config, "height", 480));
        config.fps      = static_cast<std::uint32_t>(AsInt(camera_config, "fps", 30));
        config.duration_ms = AsInt(camera_config, "duration_ms", 0);
        config.base_b = static_cast<std::uint8_t>(camera_id == 0 ? 128 : 0);
        config.base_g = static_cast<std::uint8_t>(camera_id == 0 ? 0 : 128);
        config.base_r = static_cast<std::uint8_t>(camera_id == 0 ? 0 : 64);
        config.name   = camera_id == 0 ? "mock_camera0" : "mock_camera1";
        return std::make_unique<MockVideoSource>(config);
    }

    // v4l2 等真实源仅在目标构建可用（pipeline_factory_target.cpp）。
    return nullptr;
}

std::unique_ptr<VideoEncoder> CreateVideoEncoder(const EncoderConfig& config)
{
    auto encoder = std::make_unique<MockVideoEncoder>();
    const Status status = encoder->Initialize(config);
    if (!status)
    {
        return nullptr;
    }
    return encoder;
}

std::unique_ptr<VideoPublisher> CreateVideoPublisher(const VideoPublisher::Config& config)
{
    auto publisher = std::make_unique<NullVideoPublisher>();
    const Status status = publisher->Initialize(config);
    if (!status)
    {
        return nullptr;
    }
    return publisher;
}

}  // namespace animal_monitor
