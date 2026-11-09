#include "test_harness.h"

// ============================================================================
// 文件：tests/unit/test_animal_tracker.cpp
// 依据：任务书 CODEX_MASTER_TASK.md §10（Dynamic ROI / 跟踪）；
//       include/animal_monitor/fusion/animal_tracker.h
// 覆盖：两帧重叠框保持同 ID、新框新 ID、miss 超限移除、IoU 阈值外不关联、容量裁剪。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "animal_monitor/fusion/animal_tracker.h"

using animal_monitor::fusion::AnimalTracker;
using animal_monitor::fusion::Track;
using animal_monitor::inference::Detection;

namespace {

Detection MakeDetection(float x, float y, float w, float h)
{
    Detection detection;
    detection.bbox.x      = x;
    detection.bbox.y      = y;
    detection.bbox.w      = w;
    detection.bbox.h      = h;
    detection.confidence  = 1.0f;
    return detection;
}

AM_TEST(keeps_id_across_overlapping_frames)
{
    AnimalTracker tracker;
    const std::vector<Detection> first = {MakeDetection(0.0f, 0.0f, 10.0f, 10.0f)};
    std::vector<Track>           tracks = tracker.Update(first, 0);
    AM_EXPECT_EQ(tracks.size(), 1u);
    const std::int32_t first_id = tracks[0].track_id;

    // 第二帧轻微位移，IoU 远高于默认阈值 0.3：应保持同一 ID
    const std::vector<Detection> second = {MakeDetection(1.0f, 1.0f, 10.0f, 10.0f)};
    tracks = tracker.Update(second, 1);
    AM_EXPECT_EQ(tracks.size(), 1u);
    AM_EXPECT_EQ(tracks[0].track_id, first_id);
    AM_EXPECT_EQ(tracks[0].miss_count, 0u);
    AM_EXPECT_EQ(tracks[0].last_seen_seq, 1ULL);
}

AM_TEST(new_box_gets_new_id)
{
    AnimalTracker tracker;
    const std::vector<Detection> detections = {MakeDetection(0.0f, 0.0f, 10.0f, 10.0f),
                                               MakeDetection(100.0f, 100.0f, 10.0f, 10.0f)};
    const std::vector<Track>     tracks     = tracker.Update(detections, 0);
    AM_EXPECT_EQ(tracks.size(), 2u);
    AM_EXPECT(tracks[0].track_id != tracks[1].track_id);
}

AM_TEST(miss_exceeding_max_removed)
{
    AnimalTracker::Config config;
    config.max_miss = 2;
    AnimalTracker tracker(config);

    const std::vector<Detection> detections = {MakeDetection(0.0f, 0.0f, 10.0f, 10.0f)};
    tracker.Update(detections, 0);
    AM_EXPECT_EQ(tracker.Update({}, 1).size(), 1u);  // miss=1
    AM_EXPECT_EQ(tracker.Update({}, 2).size(), 1u);  // miss=2（未超）
    AM_EXPECT_EQ(tracker.Update({}, 3).size(), 0u);  // miss=3 > 2 → 移除
}

AM_TEST(no_association_below_iou_threshold)
{
    AnimalTracker::Config config;
    config.iou_threshold = 0.5f;
    AnimalTracker tracker(config);

    tracker.Update({MakeDetection(0.0f, 0.0f, 10.0f, 10.0f)}, 0);
    // 与旧框 IoU=0，低于阈值：不关联，生成新 track，旧 track 记 miss
    const std::vector<Track> tracks =
        tracker.Update({MakeDetection(100.0f, 100.0f, 10.0f, 10.0f)}, 1);
    AM_EXPECT_EQ(tracks.size(), 2u);
    AM_EXPECT(tracks[0].track_id != tracks[1].track_id);
    AM_EXPECT_EQ(tracks[0].miss_count, 1u);  // 旧 track 未匹配
    AM_EXPECT_EQ(tracks[1].miss_count, 0u);  // 新 track 本帧不计 miss
}

AM_TEST(partial_overlap_above_threshold_associates)
{
    AnimalTracker tracker;  // 默认阈值 0.3
    tracker.Update({MakeDetection(0.0f, 0.0f, 10.0f, 10.0f)}, 0);
    // 重叠 IoU = 64/136 ≈ 0.47 ≥ 0.3：应关联
    const std::vector<Track> tracks = tracker.Update({MakeDetection(2.0f, 2.0f, 10.0f, 10.0f)}, 1);
    AM_EXPECT_EQ(tracks.size(), 1u);
    AM_EXPECT_EQ(tracks[0].miss_count, 0u);
}

AM_TEST(max_tracks_capacity_trimmed)
{
    AnimalTracker::Config config;
    config.max_tracks = 3;
    AnimalTracker tracker(config);

    const std::vector<Detection> detections = {
        MakeDetection(0.0f, 0.0f, 10.0f, 10.0f),     MakeDetection(100.0f, 0.0f, 10.0f, 10.0f),
        MakeDetection(200.0f, 0.0f, 10.0f, 10.0f),   MakeDetection(300.0f, 0.0f, 10.0f, 10.0f),
        MakeDetection(400.0f, 0.0f, 10.0f, 10.0f)};
    const std::vector<Track> tracks = tracker.Update(detections, 0);
    AM_EXPECT_EQ(tracks.size(), 3u);
}

AM_TEST(invalid_detection_skipped)
{
    AnimalTracker tracker;

    // 宽为负：跳过
    AM_EXPECT_EQ(tracker.Update({MakeDetection(0.0f, 0.0f, -1.0f, 10.0f)}, 0).size(), 0u);

    // 含 NaN：跳过
    Detection bad = MakeDetection(0.0f, 0.0f, 10.0f, 10.0f);
    bad.bbox.w    = std::numeric_limits<float>::quiet_NaN();
    AM_EXPECT_EQ(tracker.Update({bad}, 1).size(), 0u);
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
