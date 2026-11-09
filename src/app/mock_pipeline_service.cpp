#include "animal_monitor/app/mock_pipeline_service.h"

#include <mutex>

#include "animal_monitor/common/bounded_queue.h"
#include "animal_monitor/common/frame_meta.h"
#include "animal_monitor/logging/logger.h"
#include "animal_monitor/metrics/metrics.h"

namespace animal_monitor
{

// 队列放在 Impl 中，避免把模板细节泄漏到头文件。
class MockPipelineService::Impl
{
   public:
    BoundedQueue<FrameMeta> queue{8, BoundedQueue<FrameMeta>::PushPolicy::kDropOldest};
};

MockPipelineService::MockPipelineService(std::chrono::milliseconds producer_interval,
                                         std::chrono::milliseconds duration)
    : producer_interval_(producer_interval), duration_(duration), impl_(std::make_unique<Impl>())
{}

MockPipelineService::~MockPipelineService() { Stop(); }

const char* MockPipelineService::Name() const { return "mock_pipeline"; }

Status MockPipelineService::Start()
{
    if (producer_.joinable())
    {
        return Status(StatusCode::kAlreadyExists, "mock_pipeline already started");
    }
    stop_requested_.store(false, std::memory_order_relaxed);
    producer_ = std::thread(&MockPipelineService::ProducerLoop, this);
    consumer_ = std::thread(&MockPipelineService::ConsumerLoop, this);
    return Status::MakeOk();
}

void MockPipelineService::Stop()
{
    stop_requested_.store(true, std::memory_order_relaxed);
    impl_->queue.Close();  // 唤醒等待者
    if (producer_.joinable())
    {
        producer_.join();
    }
    if (consumer_.joinable())
    {
        consumer_.join();
    }
}

MockPipelineService::Snapshot MockPipelineService::GetSnapshot() const
{
    Snapshot snapshot;
    snapshot.produced   = produced_.load(std::memory_order_relaxed);
    snapshot.consumed   = consumed_.load(std::memory_order_relaxed);
    const auto metrics  = impl_->queue.GetMetrics();
    snapshot.dropped    = metrics.drops;
    snapshot.queue_size = impl_->queue.Size();
    return snapshot;
}

void MockPipelineService::ProducerLoop()
{
    const auto deadline = std::chrono::steady_clock::now() +
                          (duration_.count() > 0 ? duration_ : std::chrono::hours(24 * 365));

    FrameMeta frame;
    frame.format = PixelFormat::kUnknown;  // MOCK 帧，无真实像素数据

    while (!stop_requested_.load(std::memory_order_relaxed))
    {
        if (duration_.count() > 0 && std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        frame.id.sequence = produced_.load(std::memory_order_relaxed);
        frame.timestamps.monotonic_ns =
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
        impl_->queue.Push(frame, std::chrono::milliseconds(50));
        produced_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(producer_interval_);
    }
}

void MockPipelineService::ConsumerLoop()
{
    FrameMeta frame;
    while (impl_->queue.Pop(frame, std::chrono::milliseconds(200)))
    {
        consumed_.fetch_add(1, std::memory_order_relaxed);
        MetricsRegistry::Instance().GetCounter("mock_frames_consumed").Increment();
    }
    // 队列 Close 且排空后正常退出
}

}  // namespace animal_monitor
