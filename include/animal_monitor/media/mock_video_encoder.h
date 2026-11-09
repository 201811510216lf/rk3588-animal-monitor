#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "animal_monitor/media/video_encoder.h"

namespace animal_monitor
{

// 明确标注的 MOCK 编码器：不产生真实码流。
// 每 Encode 一帧返回一个固定模式假包（仅用于验证流水线接线与队列/统计逻辑）。
class MockVideoEncoder : public VideoEncoder
{
   public:
    MockVideoEncoder();
    ~MockVideoEncoder() override;

    Status Initialize(const EncoderConfig& config) override;
    Status Encode(const VideoFrame& frame) override;
    Status GetPacket(EncodedPacket* out, std::chrono::milliseconds timeout) override;

    struct Snapshot
    {
        std::uint64_t encoded_frames = 0;
        std::uint64_t packets_out    = 0;
    };
    Snapshot GetSnapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
