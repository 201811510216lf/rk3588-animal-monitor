#include "test_harness.h"

// ============================================================================
// 文件：tests/unit/test_anomaly_detector.cpp
// 依据：任务书 CODEX_MASTER_TASK.md §10（Anomaly detector：先规则和个体基线）；
//       include/animal_monitor/fusion/anomaly_detector.h
// 覆盖：z 超阈值判异常、基线不足不判、绝对阈值命中、stddev=0 路径、非有限输入。
// ============================================================================

#include <cmath>
#include <limits>

#include "animal_monitor/fusion/anomaly_detector.h"

using animal_monitor::fusion::AnomalyConfig;
using animal_monitor::fusion::AnomalyResult;
using animal_monitor::fusion::Evaluate;

namespace {

bool Near(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f;
}

AM_TEST(zscore_exceeding_threshold_is_anomaly)
{
    const AnomalyConfig config;  // z_threshold=3，min_baseline_samples=10
    const AnomalyResult result = Evaluate(config, 39.0f, 20, 37.0f, 0.5f);
    AM_EXPECT(result.is_anomaly);
    AM_EXPECT(result.reason == AnomalyResult::Reason::kZScore);
    AM_EXPECT(Near(result.z_score, 4.0f));  // (39-37)/0.5
}

AM_TEST(zscore_within_threshold_is_normal)
{
    const AnomalyConfig config;
    const AnomalyResult result = Evaluate(config, 37.5f, 20, 37.0f, 0.5f);
    AM_EXPECT(!result.is_anomaly);
    AM_EXPECT(result.reason == AnomalyResult::Reason::kNone);
    AM_EXPECT(Near(result.z_score, 1.0f));
}

AM_TEST(zscore_exactly_at_threshold_is_anomaly)
{
    const AnomalyConfig config;
    const AnomalyResult result = Evaluate(config, 38.5f, 20, 37.0f, 0.5f);
    AM_EXPECT(result.is_anomaly);  // |z|=3 >= 3
    AM_EXPECT(result.reason == AnomalyResult::Reason::kZScore);
}

AM_TEST(baseline_too_small_not_judged)
{
    const AnomalyConfig config;  // min_baseline_samples=10
    const AnomalyResult result = Evaluate(config, 100.0f, 5, 37.0f, 0.5f);
    AM_EXPECT(!result.is_anomaly);
    AM_EXPECT(result.reason == AnomalyResult::Reason::kBaselineTooSmall);
    AM_EXPECT_EQ(result.z_score, 0.0f);
}

AM_TEST(absolute_min_hit_takes_precedence)
{
    AnomalyConfig config;
    config.abs_min = 35.0f;
    config.abs_max = 42.0f;
    const AnomalyResult result = Evaluate(config, 30.0f, 20, 38.0f, 0.5f);
    AM_EXPECT(result.is_anomaly);
    // z=(30-38)/0.5=-16 同样超阈值，但绝对阈值优先
    AM_EXPECT(result.reason == AnomalyResult::Reason::kAbsolute);
    AM_EXPECT_EQ(result.z_score, 0.0f);
}

AM_TEST(absolute_max_hit)
{
    AnomalyConfig config;
    config.abs_max = 42.0f;
    const AnomalyResult result = Evaluate(config, 45.0f, 20, 38.0f, 0.5f);
    AM_EXPECT(result.is_anomaly);
    AM_EXPECT(result.reason == AnomalyResult::Reason::kAbsolute);
}

AM_TEST(stddev_zero_only_absolute_rule_applies)
{
    AnomalyConfig config;
    config.abs_min = 35.0f;

    // stddev=0：与均值不同但无绝对越界 → 不判异常（z 记为 0）
    const AnomalyResult normal = Evaluate(config, 38.1f, 20, 38.0f, 0.0f);
    AM_EXPECT(!normal.is_anomaly);
    AM_EXPECT(normal.reason == AnomalyResult::Reason::kNone);
    AM_EXPECT_EQ(normal.z_score, 0.0f);

    // 绝对越界 → 判异常
    const AnomalyResult absolute = Evaluate(config, 20.0f, 20, 38.0f, 0.0f);
    AM_EXPECT(absolute.is_anomaly);
    AM_EXPECT(absolute.reason == AnomalyResult::Reason::kAbsolute);
}

AM_TEST(nonfinite_value_is_absolute_anomaly)
{
    const AnomalyConfig config;  // 绝对阈值未启用
    const AnomalyResult result =
        Evaluate(config, std::numeric_limits<float>::quiet_NaN(), 20, 37.0f, 0.5f);
    AM_EXPECT(result.is_anomaly);
    AM_EXPECT(result.reason == AnomalyResult::Reason::kAbsolute);
    AM_EXPECT_EQ(result.z_score, 0.0f);
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
