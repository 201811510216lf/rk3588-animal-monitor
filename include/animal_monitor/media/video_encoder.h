#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "animal_monitor/common/status.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor {

enum class VideoCodec : std::uint32_t
{
    kUnknown = 0,
    kH264    = 1,
    kH265    = 2,
};

struct EncoderConfig
{
    std::uint32_t width        = 1920;
    std::uint32_t height       = 1080;
    std::uint32_t fps          = 30;
    std::int64_t  bitrate_bps  = 2'000'000;
    std::uint32_t gop          = 60;
    VideoCodec    codec        = VideoCodec::kH264;
    PixelFormat   input_format = PixelFormat::kNv12;  // 编码器输入像素格式
    std::uint32_t hor_stride   = 0;                   // 0 = 由实现按宽对齐
    std::uint32_t ver_stride   = 0;
};

// 编码输出包（Annex-B 码流）。
struct EncodedPacket
{
    std::vector<std::uint8_t> data;
    std::int64_t              pts_us = 0;
    std::int64_t              dts_us = 0;
    bool                      key_frame = false;
};

// 视频编码器抽象：MPP 硬编码（目标板）、mock 编码器（host）。
class VideoEncoder
{
  public:
    virtual ~VideoEncoder() = default;

    virtual Status Initialize(const EncoderConfig& config) = 0;

    // 送入一帧；阻塞语义：返回即视为编码器已消费输入帧，
    // 调用方可归还输入帧的底层缓冲（见 buffer_lifecycle.md §5）。
    virtual Status Encode(const VideoFrame& frame) = 0;

    // 取出编码包；超时返回 kTimeout。关停后返回 kCancelled。
    virtual Status GetPacket(EncodedPacket* out, std::chrono::milliseconds timeout) = 0;
};

}  // namespace animal_monitor
