#include "animal_monitor/media/mock_video_source.h"

#include <atomic>
#include <thread>

#include "animal_monitor/common/bounded_queue.h"

namespace animal_monitor {

class MockVideoSource::Impl
{
  public:
    BoundedQueue<VideoFramePtr> queue{8, BoundedQueue<VideoFramePtr>::PushPolicy::kDropOldest};
};

MockVideoSource::MockVideoSource(Config config) : impl_(std::make_unique<Impl>())
{
    config_   = config;  // 保存给 Name/NominalMeta 使用（见类尾私有扩展）
    nominal_.width  = config.width;
    nominal_.height = config.height;
    nominal_.stride = config.width * 3;
    nominal_.format = PixelFormat::kBgr888;
    nominal_.id.camera_id = 0;
}

MockVideoSource::~MockVideoSource()
{
    Stop();
}

const char* MockVideoSource::Name() const
{
    return config_.name.c_str();
}

const FrameMeta& MockVideoSource::NominalMeta() const
{
    return nominal_;
}

Status MockVideoSource::Start()
{
    if (producer_.joinable())
    {
        return Status(StatusCode::kAlreadyExists, "mock source already started");
    }
    stop_requested_.store(false, std::memory_order_relaxed);
    producer_ = std::thread(&MockVideoSource::ProduceLoop, this);
    return Status::MakeOk();
}

void MockVideoSource::Stop()
{
    stop_requested_.store(true, std::memory_order_relaxed);
    impl_->queue.Close();
    if (producer_.joinable())
    {
        producer_.join();
    }
}

Status MockVideoSource::WaitForFrame(VideoFramePtr* out, std::chrono::milliseconds timeout)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "null output");
    }
    if (!impl_->queue.Pop(*out, timeout))
    {
        if (impl_->queue.IsClosed())
        {
            return Status(StatusCode::kCancelled, "mock source closed");
        }
        return Status(StatusCode::kTimeout, "mock source frame timeout");
    }
    return Status::MakeOk();
}

MockVideoSource::Snapshot MockVideoSource::GetSnapshot() const
{
    Snapshot snapshot;
    snapshot.produced   = produced_.load(std::memory_order_relaxed);
    snapshot.consumed   = consumed_.load(std::memory_order_relaxed);
    const auto metrics  = impl_->queue.GetMetrics();
    snapshot.dropped    = metrics.drops;
    snapshot.queue_size = impl_->queue.Size();
    return snapshot;
}

void MockVideoSource::ProduceLoop()
{
    const std::chrono::milliseconds interval(
        config_.fps > 0 ? 1000 / config_.fps : 33);
    const auto deadline =
        std::chrono::steady_clock::now() +
        (config_.duration_ms > 0 ? std::chrono::milliseconds(config_.duration_ms)
                                 : std::chrono::hours(24 * 365));

    const std::size_t frame_bytes = static_cast<std::size_t>(config_.width) *
                                    config_.height * 3;
    std::uint64_t sequence = 0;

    while (!stop_requested_.load(std::memory_order_relaxed))
    {
        if (config_.duration_ms > 0 && std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }

        auto frame = std::make_shared<VideoFrame>(FrameMeta{});
        auto heap  = std::make_shared<HeapBuffer>(std::vector<std::uint8_t>(frame_bytes, 0));
        frame->owner = heap;
        frame->planes.push_back(
            PlaneView{heap->Bytes().data(), frame_bytes, config_.width * 3});
        frame->meta.width  = config_.width;
        frame->meta.height = config_.height;
        frame->meta.stride = config_.width * 3;
        frame->meta.format = PixelFormat::kBgr888;
        frame->meta.id.camera_id = 0;
        frame->meta.id.sequence  = sequence;
        frame->meta.timestamps.monotonic_ns =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());

        // 合成图案：纯色底 + 移动亮块（MOCK，无真实语义）。
        std::uint8_t* data = heap->Bytes().data();
        for (std::size_t i = 0; i < frame_bytes; i += 3)
        {
            data[i + 0] = config_.base_b;
            data[i + 1] = config_.base_g;
            data[i + 2] = config_.base_r;
        }
        const std::uint32_t block = config_.height / 8;
        const std::uint32_t bx = static_cast<std::uint32_t>(
            (sequence * 16) % (config_.width > block ? config_.width - block : 1));
        for (std::uint32_t y = block; y < block * 2; ++y)
        {
            for (std::uint32_t x = bx; x < bx + block && x < config_.width; ++x)
            {
                const std::size_t off = (static_cast<std::size_t>(y) * config_.width + x) * 3;
                data[off + 0] = 0;
                data[off + 1] = 255;
                data[off + 2] = 0;
            }
        }

        impl_->queue.Push(frame, std::chrono::milliseconds(50));
        produced_.fetch_add(1, std::memory_order_relaxed);
        ++sequence;
        std::this_thread::sleep_for(interval);
    }
}

}  // namespace animal_monitor
