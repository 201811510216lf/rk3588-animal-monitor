#include "animal_monitor/media/frame_synchronizer.h"

#include <cstdlib>

namespace animal_monitor {

FrameSynchronizer::FrameSynchronizer(Config config)
    : config_(config),
      queue_{BoundedQueue<VideoFramePtr>(config.queue_capacity,
                                         BoundedQueue<VideoFramePtr>::PushPolicy::kDropOldest),
             BoundedQueue<VideoFramePtr>(config.queue_capacity,
                                         BoundedQueue<VideoFramePtr>::PushPolicy::kDropOldest)}
{
}

bool FrameSynchronizer::PushFrame(int camera_id, VideoFramePtr frame)
{
    const int index = (camera_id == 0) ? 0 : 1;

    // drop_oldest 策略下 TryPush 恒返回 true（换出旧帧也算成功），
    // 队列内指标差值才是真实的满队列丢帧数。
    const auto before = queue_[index].GetMetrics();
    const bool queued = queue_[index].TryPush(std::move(frame));
    const auto after  = queue_[index].GetMetrics();

    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ++metrics_.pushes[index];
    const std::uint64_t queue_drops = after.drops - before.drops;
    if (!queued || queue_drops > 0)
    {
        ++metrics_.drops[index];
    }
    return queued;
}

Status FrameSynchronizer::PopWithHold(BoundedQueue<VideoFramePtr>& queue, VideoFramePtr* hold,
                                      std::chrono::milliseconds timeout)
{
    if (*hold != nullptr)
    {
        return Status::MakeOk();
    }
    if (!queue.Pop(*hold, timeout))
    {
        if (queue.IsClosed())
        {
            return Status(StatusCode::kCancelled, "synchronizer closed");
        }
        return Status(StatusCode::kTimeout, "frame synchronizer pop timeout");
    }
    return Status::MakeOk();
}

Status FrameSynchronizer::PopPair(VideoFramePtr* left, VideoFramePtr* right,
                                  std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true)
    {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            ++metrics_.timeouts;
            return Status(StatusCode::kTimeout, "frame synchronizer pair timeout");
        }

        Status status0 = PopWithHold(queue_[0], &hold_[0], remaining);
        if (!status0)
        {
            if (status0.Code() == StatusCode::kTimeout)
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.timeouts;
            }
            return status0;
        }
        Status status1 = PopWithHold(queue_[1], &hold_[1], remaining);
        if (!status1)
        {
            if (status1.Code() == StatusCode::kTimeout)
            {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                ++metrics_.timeouts;
            }
            return status1;
        }

        const std::int64_t delta =
            hold_[0]->meta.timestamps.monotonic_ns - hold_[1]->meta.timestamps.monotonic_ns;
        if (std::llabs(delta) <= config_.tolerance_ns)
        {
            *left  = std::move(hold_[0]);
            *right = std::move(hold_[1]);
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            ++metrics_.pairs;
            return Status::MakeOk();
        }

        // 超出容差：丢弃较旧样本，保留较新样本，继续等另一路新帧。
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        ++metrics_.desync_drops;
        if (hold_[0]->meta.timestamps.monotonic_ns < hold_[1]->meta.timestamps.monotonic_ns)
        {
            hold_[0].reset();
        }
        else
        {
            hold_[1].reset();
        }
    }
}

void FrameSynchronizer::Close()
{
    queue_[0].Close();
    queue_[1].Close();
}

FrameSynchronizer::Metrics FrameSynchronizer::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
}

}  // namespace animal_monitor
