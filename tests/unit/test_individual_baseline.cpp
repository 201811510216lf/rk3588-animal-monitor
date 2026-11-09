#include "test_harness.h"

// ============================================================================
// 文件：tests/unit/test_individual_baseline.cpp
// 依据：任务书 CODEX_MASTER_TASK.md §10（Individual baseline）；
//       include/animal_monitor/fusion/individual_baseline.h
// 覆盖：滚动窗口 count/mean/stddev 数值、最旧样本淘汰、个体隔离、非有限值忽略。
// ============================================================================

#include <cmath>
#include <cstdint>
#include <limits>

#include "animal_monitor/fusion/individual_baseline.h"

using animal_monitor::fusion::IndividualBaseline;

namespace
{

bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

AM_TEST(window_stats_known_values)
{
    // 窗口 {1,2,3,4}：count=4，mean=2.5，stddev=sqrt(1.25)≈1.118034
    IndividualBaseline baseline(4);
    baseline.AddSample(1, 1.0f);
    baseline.AddSample(1, 2.0f);
    baseline.AddSample(1, 3.0f);
    baseline.AddSample(1, 4.0f);

    std::uint32_t count  = 0;
    float         mean   = 0.0f;
    float         stddev = 0.0f;
    AM_EXPECT(baseline.GetStats(1, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 4u);
    AM_EXPECT(Near(mean, 2.5f));
    AM_EXPECT(Near(stddev, 1.118034f));
}

AM_TEST(sliding_window_evicts_oldest)
{
    // 上限 3：加入 {1,2,3,4} 后窗口为 {2,3,4}，mean=3，stddev=sqrt(2/3)≈0.816497
    IndividualBaseline baseline(3);
    baseline.AddSample(1, 1.0f);
    baseline.AddSample(1, 2.0f);
    baseline.AddSample(1, 3.0f);
    baseline.AddSample(1, 4.0f);

    std::uint32_t count  = 0;
    float         mean   = 0.0f;
    float         stddev = 0.0f;
    AM_EXPECT(baseline.GetStats(1, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 3u);
    AM_EXPECT(Near(mean, 3.0f));
    AM_EXPECT(Near(stddev, 0.816497f));
}

AM_TEST(per_id_isolation)
{
    IndividualBaseline baseline(8);
    baseline.AddSample(1, 1.0f);
    baseline.AddSample(1, 2.0f);
    baseline.AddSample(2, 10.0f);

    std::uint32_t count  = 0;
    float         mean   = 0.0f;
    float         stddev = 0.0f;
    AM_EXPECT(baseline.GetStats(1, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 2u);
    AM_EXPECT(Near(mean, 1.5f));

    AM_EXPECT(baseline.GetStats(2, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 1u);
    AM_EXPECT(Near(mean, 10.0f));
    AM_EXPECT_EQ(baseline.IdCount(), 2u);
}

AM_TEST(unknown_id_returns_false_and_zeroes)
{
    IndividualBaseline baseline(8);
    std::uint32_t      count  = 7u;
    float              mean   = 1.0f;
    float              stddev = 1.0f;
    AM_EXPECT(!baseline.GetStats(99, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 0u);
    AM_EXPECT_EQ(mean, 0.0f);
    AM_EXPECT_EQ(stddev, 0.0f);
}

AM_TEST(nonfinite_sample_ignored)
{
    IndividualBaseline baseline(8);
    baseline.AddSample(1, 5.0f);
    baseline.AddSample(1, std::numeric_limits<float>::quiet_NaN());
    std::uint32_t count  = 0;
    float         mean   = 0.0f;
    float         stddev = 0.0f;
    AM_EXPECT(baseline.GetStats(1, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 1u);
    AM_EXPECT(Near(mean, 5.0f));

    // 纯 NaN 输入不创建个体窗口
    baseline.AddSample(2, std::numeric_limits<float>::quiet_NaN());
    AM_EXPECT(!baseline.GetStats(2, &count, &mean, &stddev));
    AM_EXPECT_EQ(baseline.IdCount(), 1u);
}

AM_TEST(single_sample_stddev_zero)
{
    IndividualBaseline baseline(8);
    baseline.AddSample(1, 7.0f);
    std::uint32_t count  = 0;
    float         mean   = 0.0f;
    float         stddev = 0.0f;
    AM_EXPECT(baseline.GetStats(1, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 1u);
    AM_EXPECT(Near(mean, 7.0f));
    AM_EXPECT_EQ(stddev, 0.0f);
}

AM_TEST(ctor_clamps_zero_window)
{
    // max_samples_per_id=0 按 1 处理：只保留最新样本
    IndividualBaseline baseline(0);
    baseline.AddSample(1, 1.0f);
    baseline.AddSample(1, 2.0f);
    baseline.AddSample(1, 3.0f);
    std::uint32_t count  = 0;
    float         mean   = 0.0f;
    float         stddev = 0.0f;
    AM_EXPECT(baseline.GetStats(1, &count, &mean, &stddev));
    AM_EXPECT_EQ(count, 1u);
    AM_EXPECT(Near(mean, 3.0f));
}

}  // namespace

int main() { return am_test::RunAll(); }
