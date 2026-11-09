#include "test_harness.h"

// ============================================================================
// 文件：tests/unit/test_feature_fusion.cpp
// 依据：任务书 CODEX_MASTER_TASK.md §10（Behavior-temperature fusion）；
//       include/animal_monitor/fusion/feature_fusion.h
// 覆盖：窗口内聚合数值、过期样本淘汰、无事件返回空、去重、行为-样本关联、容量有界。
// 温度数值为合成数据（NOT_CALIBRATED）。
// ============================================================================

#include <cmath>
#include <string>

#include "animal_monitor/fusion/feature_fusion.h"

using animal_monitor::fusion::BehaviorEvent;
using animal_monitor::fusion::FeatureFusion;
using animal_monitor::fusion::ThermalSample;

namespace
{

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

AM_TEST(aggregates_events_and_thermal_in_window)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{1'000, "eat", 0});
    fusion.AddThermalSample(ThermalSample{800, 30.0f, 31.0f});
    fusion.AddThermalSample(ThermalSample{900, 32.0f, 34.0f});

    // Snapshot(5000)：窗口 [-5000, 5000]，事件 1000 与两个样本均在窗口内，
    // 且样本落在事件尾随时间窗 [-9000, 1000] 内。
    const auto stats = fusion.Snapshot(5'000);
    AM_EXPECT_EQ(stats.size(), 1u);
    AM_EXPECT_EQ(stats[0].behavior, std::string("eat"));
    AM_EXPECT_EQ(stats[0].event_count, 1u);
    AM_EXPECT_EQ(stats[0].thermal_count, 2u);
    AM_EXPECT(Near(stats[0].thermal_mean, 31.0f));  // (30+32)/2
    AM_EXPECT(Near(stats[0].thermal_max, 34.0f));
}

AM_TEST(expired_event_excluded_at_snapshot)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{1'000, "eat", 0});
    // Snapshot(20000)：窗口 [10000, 20000]，事件 1000 过期
    AM_EXPECT_EQ(fusion.Snapshot(20'000).size(), 0u);
}

AM_TEST(expired_samples_excluded_at_snapshot)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{15'000, "eat", 0});
    fusion.AddThermalSample(ThermalSample{14'000, 30.0f, 31.0f});
    fusion.AddThermalSample(ThermalSample{12'000, 40.0f, 41.0f});

    // Snapshot(25000)：窗口 [15000, 25000]。事件 15000 保留；
    // 两个样本虽在事件尾随时间窗 [5000, 15000] 内，但已出全局窗口 → 不参与聚合。
    const auto stats = fusion.Snapshot(25'000);
    AM_EXPECT_EQ(stats.size(), 1u);
    AM_EXPECT_EQ(stats[0].event_count, 1u);
    AM_EXPECT_EQ(stats[0].thermal_count, 0u);
    AM_EXPECT_EQ(stats[0].thermal_mean, 0.0f);
    AM_EXPECT_EQ(stats[0].thermal_max, 0.0f);
}

AM_TEST(add_time_eviction_removes_expired_entries)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{1'000, "old", 0});
    fusion.AddBehaviorEvent(BehaviorEvent{20'000, "new", 0});

    // 加入 20000 事件时淘汰 1000 事件（1000 < 20000-10000）。
    const auto stats = fusion.Snapshot(20'000);
    AM_EXPECT_EQ(stats.size(), 1u);
    AM_EXPECT_EQ(stats[0].behavior, std::string("new"));
}

AM_TEST(no_events_returns_empty)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddThermalSample(ThermalSample{800, 30.0f, 31.0f});
    AM_EXPECT_EQ(fusion.Snapshot(5'000).size(), 0u);
}

AM_TEST(sample_shared_by_multiple_events_counted_once)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{9'000, "eat", 0});
    fusion.AddBehaviorEvent(BehaviorEvent{9'500, "eat", 0});
    fusion.AddThermalSample(ThermalSample{8'000, 30.0f, 31.0f});

    // 样本 8000 同时落在两个事件的尾随时间窗内：同行为内去重，只计一次。
    const auto stats = fusion.Snapshot(10'000);
    AM_EXPECT_EQ(stats.size(), 1u);
    AM_EXPECT_EQ(stats[0].event_count, 2u);
    AM_EXPECT_EQ(stats[0].thermal_count, 1u);
}

AM_TEST(thermal_sample_associated_with_matching_behavior)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{1'000, "eat", 0});
    fusion.AddBehaviorEvent(BehaviorEvent{11'000, "sleep", 0});
    fusion.AddThermalSample(ThermalSample{1'500, 20.0f, 25.0f});

    // Snapshot(11000)：窗口 [1000, 11000]。
    // 样本 1500 在 sleep 尾随时间窗 [1000, 11000] 内，但不在 eat 尾随时间窗
    // [-9000, 1000] 内（1500 > 1000）→ 只关联 sleep。
    const auto stats = fusion.Snapshot(11'000);
    AM_EXPECT_EQ(stats.size(), 2u);
    AM_EXPECT_EQ(stats[0].behavior, std::string("eat"));  // 字典序
    AM_EXPECT_EQ(stats[1].behavior, std::string("sleep"));
    AM_EXPECT_EQ(stats[0].thermal_count, 0u);
    AM_EXPECT_EQ(stats[1].thermal_count, 1u);
    AM_EXPECT(Near(stats[1].thermal_mean, 20.0f));
    AM_EXPECT(Near(stats[1].thermal_max, 25.0f));
}

AM_TEST(behaviors_sorted_alphabetically)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{100, "zebra", 0});
    fusion.AddBehaviorEvent(BehaviorEvent{200, "alpha", 0});
    const auto stats = fusion.Snapshot(1'000);
    AM_EXPECT_EQ(stats.size(), 2u);
    AM_EXPECT_EQ(stats[0].behavior, std::string("alpha"));
    AM_EXPECT_EQ(stats[1].behavior, std::string("zebra"));
}

AM_TEST(max_events_capacity_bounded)
{
    FeatureFusion fusion(10'000, 2);
    fusion.AddBehaviorEvent(BehaviorEvent{1'000, "a", 0});
    fusion.AddBehaviorEvent(BehaviorEvent{2'000, "a", 0});
    fusion.AddBehaviorEvent(BehaviorEvent{3'000, "b", 0});

    // 容量 2：最旧的 a@1000 被丢弃；Snapshot(3000) 窗口 [-7000, 3000]。
    const auto stats = fusion.Snapshot(3'000);
    AM_EXPECT_EQ(stats.size(), 2u);
    AM_EXPECT_EQ(stats[0].behavior, std::string("a"));
    AM_EXPECT_EQ(stats[0].event_count, 1u);
    AM_EXPECT_EQ(stats[1].behavior, std::string("b"));
    AM_EXPECT_EQ(stats[1].event_count, 1u);
}

AM_TEST(future_events_excluded)
{
    FeatureFusion fusion(10'000, 16);
    fusion.AddBehaviorEvent(BehaviorEvent{6'000, "eat", 0});
    // 事件时间戳晚于快照时刻：不计入
    AM_EXPECT_EQ(fusion.Snapshot(5'000).size(), 0u);
}

}  // namespace

int main() { return am_test::RunAll(); }
