#pragma once

#include <cstdint>
#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor
{

// 流发布抽象：ZLMediaKit 内嵌发布（目标板）、空发布（host 记录统计）。
class VideoPublisher
{
   public:
    struct Config
    {
        std::uint16_t rtsp_port     = 8554;
        std::uint16_t http_port     = 8000;
        std::uint16_t rtc_port      = 8001;
        std::string   app           = "live";
        std::string   stream        = "composite";
        bool          enable_webrtc = false;
        bool          enable_rtmp   = false;
    };

    virtual ~VideoPublisher() = default;

    virtual Status Initialize(const Config& config) = 0;

    // 喂入 H.264 Annex-B 码流（实现负责拆 NALU 与时间戳单调化）。
    virtual Status FeedH264(const std::uint8_t* data, std::size_t size, std::int64_t dts_ms,
                            std::int64_t pts_ms) = 0;

    virtual void Close() = 0;

    virtual std::string RtspUrl() const = 0;
};

}  // namespace animal_monitor
