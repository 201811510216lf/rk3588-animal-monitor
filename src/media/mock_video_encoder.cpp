#include "animal_monitor/media/mock_video_encoder.h"

#include <atomic>

#include "animal_monitor/common/bounded_queue.h"

namespace animal_monitor
{

class MockVideoEncoder::Impl
{
   public:
    BoundedQueue<EncodedPacket> packets{8, BoundedQueue<EncodedPacket>::PushPolicy::kDropOldest};
    std::atomic<std::uint64_t>  encoded{0};
    std::uint32_t               width  = 0;
    std::uint32_t               height = 0;
};

MockVideoEncoder::MockVideoEncoder() : impl_(std::make_unique<Impl>()) {}

MockVideoEncoder::~MockVideoEncoder() = default;

Status MockVideoEncoder::Initialize(const EncoderConfig& config)
{
    impl_->width  = config.width;
    impl_->height = config.height;
    return Status::MakeOk();
}

Status MockVideoEncoder::Encode(const VideoFrame& frame)
{
    // MOCK：只统计并生成 16 字节假包（带帧序号），不代表任何真实码流。
    const std::uint64_t seq = impl_->encoded.fetch_add(1, std::memory_order_relaxed);

    EncodedPacket packet;
    packet.data.resize(16, 0);
    packet.data[0] = 0x00;
    packet.data[1] = 0x00;
    packet.data[2] = 0x00;
    packet.data[3] = 0x01;  // 假 Annex-B 起始码（MOCK，非真实 NALU）
    for (int i = 0; i < 8; ++i)
    {
        packet.data[8 + i] = static_cast<std::uint8_t>((seq >> (i * 8)) & 0xFF);
    }
    packet.pts_us    = frame.meta.timestamps.monotonic_ns / 1000;
    packet.dts_us    = packet.pts_us;
    packet.key_frame = (seq % 30 == 0);  // MOCK GOP 节拍
    impl_->packets.TryPush(std::move(packet));
    return Status::MakeOk();
}

Status MockVideoEncoder::GetPacket(EncodedPacket* out, std::chrono::milliseconds timeout)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "null output");
    }
    if (!impl_->packets.Pop(*out, timeout))
    {
        if (impl_->packets.IsClosed())
        {
            return Status(StatusCode::kCancelled, "mock encoder closed");
        }
        return Status(StatusCode::kTimeout, "mock encoder packet timeout");
    }
    return Status::MakeOk();
}

MockVideoEncoder::Snapshot MockVideoEncoder::GetSnapshot() const
{
    Snapshot snapshot;
    snapshot.encoded_frames = impl_->encoded.load(std::memory_order_relaxed);
    snapshot.packets_out    = impl_->packets.GetMetrics().pops;
    return snapshot;
}

}  // namespace animal_monitor
