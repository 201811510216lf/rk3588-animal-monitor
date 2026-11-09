#include <atomic>
#include <chrono>
#include <thread>

#include "test_harness.h"

#include "animal_monitor/media/frame_synchronizer.h"

using animal_monitor::FrameSynchronizer;
using animal_monitor::VideoFrame;
using animal_monitor::VideoFramePtr;

namespace
{

using namespace std::chrono_literals;

VideoFramePtr MakeFrame(std::int64_t monotonic_ns)
{
    auto frame                          = std::make_shared<VideoFrame>();
    frame->meta.timestamps.monotonic_ns = monotonic_ns;
    return frame;
}

AM_TEST(pairs_frames_within_tolerance)
{
    FrameSynchronizer::Config config;
    config.tolerance_ns = 5'000'000;
    FrameSynchronizer sync(config);

    AM_EXPECT(sync.PushFrame(0, MakeFrame(1'000'000'000)));
    AM_EXPECT(sync.PushFrame(1, MakeFrame(1'002'000'000)));

    VideoFramePtr left;
    VideoFramePtr right;
    const auto    status = sync.PopPair(&left, &right, 100ms);
    AM_EXPECT(status.Ok());
    AM_EXPECT(left != nullptr && right != nullptr);
    AM_EXPECT_EQ(left->meta.timestamps.monotonic_ns, 1'000'000'000LL);
    AM_EXPECT_EQ(right->meta.timestamps.monotonic_ns, 1'002'000'000LL);
}

AM_TEST(drops_older_sample_on_desync)
{
    FrameSynchronizer::Config config;
    config.tolerance_ns = 5'000'000;
    FrameSynchronizer sync(config);

    // camera0 的帧比 camera1 晚 20ms：先到的 camera1 样本应被丢弃（旧帧）
    AM_EXPECT(sync.PushFrame(0, MakeFrame(1'020'000'000)));
    AM_EXPECT(sync.PushFrame(1, MakeFrame(1'000'000'000)));

    // camera1 的新样本与 camera0 差 2ms → 配对成功
    AM_EXPECT(sync.PushFrame(1, MakeFrame(1'022'000'000)));

    VideoFramePtr left;
    VideoFramePtr right;
    const auto    status = sync.PopPair(&left, &right, 100ms);
    AM_EXPECT(status.Ok());
    AM_EXPECT_EQ(left->meta.timestamps.monotonic_ns, 1'020'000'000LL);
    AM_EXPECT_EQ(right->meta.timestamps.monotonic_ns, 1'022'000'000LL);

    const auto metrics = sync.GetMetrics();
    AM_EXPECT_EQ(metrics.desync_drops, 1u);
}

AM_TEST(pop_timeout_when_only_one_side)
{
    FrameSynchronizer::Config config;
    config.tolerance_ns = 5'000'000;
    FrameSynchronizer sync(config);

    AM_EXPECT(sync.PushFrame(0, MakeFrame(1'000'000'000)));

    VideoFramePtr left;
    VideoFramePtr right;
    const auto    status = sync.PopPair(&left, &right, 80ms);
    AM_EXPECT_EQ(status.Code(), animal_monitor::StatusCode::kTimeout);

    const auto metrics = sync.GetMetrics();
    AM_EXPECT_EQ(metrics.timeouts, 1u);
}

AM_TEST(close_cancels_pending_pop)
{
    FrameSynchronizer::Config config;
    FrameSynchronizer         sync(config);

    std::atomic<bool> done{false};
    std::thread       popper(
        [&sync, &done]
        {
            VideoFramePtr left;
            VideoFramePtr right;
            const auto    status = sync.PopPair(&left, &right, 10s);
            done.store(status.Code() == animal_monitor::StatusCode::kCancelled);
        });

    std::this_thread::sleep_for(50ms);
    sync.Close();
    popper.join();
    AM_EXPECT(done.load());
}

AM_TEST(queue_full_drops_oldest)
{
    FrameSynchronizer::Config config;
    config.queue_capacity = 2;
    FrameSynchronizer sync(config);

    AM_EXPECT(sync.PushFrame(0, MakeFrame(1'000'000'000)));
    AM_EXPECT(sync.PushFrame(0, MakeFrame(1'100'000'000)));
    AM_EXPECT(sync.PushFrame(0, MakeFrame(1'200'000'000)));  // 满：丢最旧(1.0s)
    AM_EXPECT(sync.PushFrame(1, MakeFrame(1'200'000'000)));

    VideoFramePtr left;
    VideoFramePtr right;
    const auto    status = sync.PopPair(&left, &right, 100ms);
    AM_EXPECT(status.Ok());
    // q0 中 1.1s 帧与 q1 的 1.2s 帧超容差 → 1.1s 被丢弃，最终以 1.2s 配对
    AM_EXPECT_EQ(left->meta.timestamps.monotonic_ns, 1'200'000'000LL);
    AM_EXPECT_EQ(right->meta.timestamps.monotonic_ns, 1'200'000'000LL);

    const auto metrics = sync.GetMetrics();
    AM_EXPECT_EQ(metrics.drops[0], 1u);      // 队列满换出 1.0s
    AM_EXPECT_EQ(metrics.desync_drops, 1u);  // 1.1s 超容差被弃
}

}  // namespace

int main() { return am_test::RunAll(); }
