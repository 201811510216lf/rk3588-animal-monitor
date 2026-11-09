#include "animal_monitor/media/null_video_publisher.h"

namespace animal_monitor {

Status NullVideoPublisher::Initialize(const Config& config)
{
    config_ = config;
    return Status::MakeOk();
}

Status NullVideoPublisher::FeedH264(const std::uint8_t* data, std::size_t size,
                                    std::int64_t dts_ms, std::int64_t pts_ms)
{
    (void)dts_ms;
    (void)pts_ms;
    if (data == nullptr && size != 0)
    {
        return Status(StatusCode::kInvalidArgument, "null data");
    }
    packets_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(size, std::memory_order_relaxed);
    return Status::MakeOk();
}

void NullVideoPublisher::Close()
{
}

std::string NullVideoPublisher::RtspUrl() const
{
    return "mock://null_publisher/" + config_.app + "/" + config_.stream;
}

NullVideoPublisher::Snapshot NullVideoPublisher::GetSnapshot() const
{
    Snapshot snapshot;
    snapshot.packets = packets_.load(std::memory_order_relaxed);
    snapshot.bytes   = bytes_.load(std::memory_order_relaxed);
    return snapshot;
}

}  // namespace animal_monitor
