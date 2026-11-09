// tests/unit/test_inference_scheduler.cpp
// 状态：implemented（host）
// 说明：调度路由（round_robin/priority/per_camera）、frame_id 去重、
//       有界结果队列 drop_oldest、超时与关闭语义。

#include <chrono>
#include <cstdint>
#include <vector>

#include "test_harness.h"

#include "animal_monitor/inference/inference_scheduler.h"
#include "animal_monitor/inference/mock_inference_model.h"
#include "animal_monitor/media/video_frame.h"

using animal_monitor::Status;
using animal_monitor::StatusCode;
using animal_monitor::VideoFrame;
using animal_monitor::inference::InferenceResult;
using animal_monitor::inference::InferenceScheduler;
using animal_monitor::inference::MockInferenceModel;
using animal_monitor::inference::ScheduleStrategy;

namespace
{

using namespace std::chrono_literals;

VideoFrame MakeFrame(std::uint32_t camera_id, std::uint64_t sequence)
{
    VideoFrame frame;
    frame.meta.id.camera_id            = camera_id;
    frame.meta.id.sequence             = sequence;
    frame.meta.width                   = 640;
    frame.meta.height                  = 480;
    frame.meta.timestamps.monotonic_ns = static_cast<std::int64_t>(sequence) * 1'000'000;
    return frame;
}

AM_TEST(round_robin_alternates_models)
{
    MockInferenceModel a("mock_a");
    MockInferenceModel b("mock_b");
    InferenceScheduler scheduler(ScheduleStrategy::kRoundRobin, {&a, &b});

    for (std::uint64_t seq = 0; seq < 4; ++seq)
    {
        AM_EXPECT(scheduler.Submit(MakeFrame(0, seq), 0).Ok());
    }
    const char* expected[] = {"mock_a", "mock_b", "mock_a", "mock_b"};
    for (int i = 0; i < 4; ++i)
    {
        InferenceResult result;
        AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());
        AM_EXPECT_EQ(result.model_id, expected[i]);
        AM_EXPECT_EQ(result.camera_id, 0u);
        AM_EXPECT_EQ(result.frame_id.sequence, static_cast<std::uint64_t>(i));
        AM_EXPECT(result.valid);
        AM_EXPECT_EQ(result.detections.size(), 1u);
    }
}

AM_TEST(dedup_skips_duplicate_frame)
{
    MockInferenceModel model("mock_dedup");
    InferenceScheduler scheduler(ScheduleStrategy::kRoundRobin, {&model});

    const VideoFrame frame = MakeFrame(0, 7);
    AM_EXPECT(scheduler.Submit(frame, 0).Ok());
    const Status duplicate = scheduler.Submit(frame, 0);
    AM_EXPECT_EQ(static_cast<int>(duplicate.Code()), static_cast<int>(StatusCode::kAlreadyExists));

    InferenceResult result;
    AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());
    AM_EXPECT_EQ(result.frame_id.sequence, 7u);
    // 无更多结果 → 超时。
    AM_EXPECT_EQ(static_cast<int>(scheduler.GetResult(&result, 30ms).Code()),
                 static_cast<int>(StatusCode::kTimeout));
}

AM_TEST(bounded_queue_drops_oldest_results)
{
    MockInferenceModel          model("mock_drop");
    InferenceScheduler::Options options;
    options.result_queue_capacity = 2;
    InferenceScheduler scheduler(ScheduleStrategy::kRoundRobin, {&model}, options);

    for (std::uint64_t seq = 0; seq < 4; ++seq)
    {
        AM_EXPECT(scheduler.Submit(MakeFrame(0, seq), 0).Ok());
    }
    const auto metrics = scheduler.GetMetrics();
    AM_EXPECT_EQ(metrics.dropped, 2u);

    InferenceResult result;
    AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());
    AM_EXPECT_EQ(result.frame_id.sequence, 2u);  // 0/1 被换出
    AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());
    AM_EXPECT_EQ(result.frame_id.sequence, 3u);
}

AM_TEST(per_camera_routes_by_camera_id)
{
    MockInferenceModel a("mock_a");
    MockInferenceModel b("mock_b");
    InferenceScheduler scheduler(ScheduleStrategy::kPerCamera, {&a, &b});

    AM_EXPECT(scheduler.Submit(MakeFrame(0, 0), 0).Ok());
    AM_EXPECT(scheduler.Submit(MakeFrame(1, 0), 1).Ok());
    AM_EXPECT(scheduler.Submit(MakeFrame(2, 0), 2).Ok());  // 2 % 2 == 0 → mock_a

    const char* expected[] = {"mock_a", "mock_b", "mock_a"};
    for (int i = 0; i < 3; ++i)
    {
        InferenceResult result;
        AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());
        AM_EXPECT_EQ(result.model_id, expected[i]);
    }
}

AM_TEST(priority_strategy_uses_first_model)
{
    MockInferenceModel a("mock_a");
    MockInferenceModel b("mock_b");
    InferenceScheduler scheduler(ScheduleStrategy::kPriority, {&a, &b});

    for (std::uint64_t seq = 0; seq < 2; ++seq)
    {
        AM_EXPECT(scheduler.Submit(MakeFrame(0, seq), 0).Ok());
    }
    for (int i = 0; i < 2; ++i)
    {
        InferenceResult result;
        AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());
        AM_EXPECT_EQ(result.model_id, "mock_a");
    }
}

AM_TEST(empty_models_rejected)
{
    InferenceScheduler scheduler(ScheduleStrategy::kRoundRobin, {});
    const Status       status = scheduler.Submit(MakeFrame(0, 0), 0);
    AM_EXPECT_EQ(static_cast<int>(status.Code()),
                 static_cast<int>(StatusCode::kConfigurationError));
}

AM_TEST(close_blocks_submit_and_drains_queue)
{
    MockInferenceModel model("mock_close");
    InferenceScheduler scheduler(ScheduleStrategy::kRoundRobin, {&model});

    AM_EXPECT(scheduler.Submit(MakeFrame(0, 1), 0).Ok());
    scheduler.Close();
    AM_EXPECT_EQ(static_cast<int>(scheduler.Submit(MakeFrame(0, 2), 0).Code()),
                 static_cast<int>(StatusCode::kCancelled));

    InferenceResult result;
    AM_EXPECT(scheduler.GetResult(&result, 100ms).Ok());  // 已入队结果仍可取
    AM_EXPECT_EQ(result.frame_id.sequence, 1u);
    AM_EXPECT_EQ(static_cast<int>(scheduler.GetResult(&result, 100ms).Code()),
                 static_cast<int>(StatusCode::kCancelled));
}

AM_TEST(metrics_count_per_model)
{
    MockInferenceModel a("mock_a");
    MockInferenceModel b("mock_b");
    InferenceScheduler scheduler(ScheduleStrategy::kRoundRobin, {&a, &b});

    for (std::uint64_t seq = 0; seq < 4; ++seq)
    {
        AM_EXPECT(scheduler.Submit(MakeFrame(0, seq), 0).Ok());
    }
    const auto metrics = scheduler.GetMetrics();
    AM_EXPECT_EQ(metrics.submitted, 4u);
    AM_EXPECT_EQ(metrics.dedup_skipped, 0u);
    AM_EXPECT_EQ(metrics.per_model.at("mock_a").routed, 2u);
    AM_EXPECT_EQ(metrics.per_model.at("mock_a").executed, 2u);
    AM_EXPECT_EQ(metrics.per_model.at("mock_a").queued, 2u);
    AM_EXPECT_EQ(metrics.per_model.at("mock_b").routed, 2u);
}

}  // namespace

int main() { return am_test::RunAll(); }
