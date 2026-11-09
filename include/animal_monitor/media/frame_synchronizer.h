#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include "animal_monitor/common/bounded_queue.h"
#include "animal_monitor/common/status.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor
{

// 双路帧同步器：两路各持有有界队列（满则丢最旧），按时间戳最近邻 + 容差配对。
// 超出容差时丢弃较旧样本（记录 desync 指标）并等待另一路新样本。
// 时间戳语义见 docs/architecture/thread_model.md（单调时钟 ns）。
class FrameSynchronizer
{
   public:
    struct Config
    {
        std::int64_t tolerance_ns   = 40'000'000;  // 40ms
        std::size_t  queue_capacity = 8;
    };

    struct Metrics
    {
        std::uint64_t pushes[2]    = {0, 0};
        std::uint64_t drops[2]     = {0, 0};  // 队列满丢弃
        std::uint64_t pairs        = 0;
        std::uint64_t desync_drops = 0;  // 超容差丢弃旧帧
        std::uint64_t timeouts     = 0;
    };

    explicit FrameSynchronizer(Config config);

    FrameSynchronizer(const FrameSynchronizer&)            = delete;
    FrameSynchronizer& operator=(const FrameSynchronizer&) = delete;

    // 相机线程调用；camera_id 取 0/1。队列满按 drop_oldest 处理（返回 false）。
    bool PushFrame(int camera_id, VideoFramePtr frame);

    // 取一对对齐帧；超时返回 kTimeout，关停返回 kCancelled。
    Status PopPair(VideoFramePtr* left, VideoFramePtr* right, std::chrono::milliseconds timeout);

    // 关停：唤醒等待者；此后 PopPair 排空后返回 kCancelled。
    void Close();

    Metrics GetMetrics() const;

   private:
    Status PopWithHold(BoundedQueue<VideoFramePtr>& queue, VideoFramePtr* hold,
                       std::chrono::milliseconds timeout);

    Config                      config_;
    BoundedQueue<VideoFramePtr> queue_[2];
    VideoFramePtr               hold_[2];
    mutable std::mutex          metrics_mutex_;
    Metrics                     metrics_;
};

}  // namespace animal_monitor
