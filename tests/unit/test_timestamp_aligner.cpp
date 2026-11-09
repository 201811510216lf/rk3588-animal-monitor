#include "test_harness.h"

#include "animal_monitor/common/timestamp_aligner.h"

using animal_monitor::TimestampAligner;

namespace
{

AM_TEST(aligns_pairs_within_tolerance)
{
    TimestampAligner aligner(5'000);  // 5ms
    const auto       first = aligner.AddSample(0, 1'000'000);
    AM_EXPECT(!first.has_value());  // 只有一路，无结果

    const auto pair = aligner.AddSample(1, 1'003'000);
    AM_EXPECT(pair.has_value());
    AM_EXPECT_EQ(pair->timestamp_a_ns, 1'000'000LL);
    AM_EXPECT_EQ(pair->timestamp_b_ns, 1'003'000LL);
    AM_EXPECT_EQ(pair->delta_ns, -3'000LL);
}

AM_TEST(drops_older_sample_when_beyond_tolerance)
{
    TimestampAligner aligner(5'000);
    AM_EXPECT(!aligner.AddSample(0, 1'000'000).has_value());

    // B 路样本比 A 路晚 20ms（超阈值）：A 路旧样本被丢弃
    AM_EXPECT(!aligner.AddSample(1, 1'020'000).has_value());

    // A 路新样本与 B 路差 3ms：配对成功
    const auto pair = aligner.AddSample(0, 1'023'000);
    AM_EXPECT(pair.has_value());
    AM_EXPECT_EQ(pair->delta_ns, 3'000LL);
}

AM_TEST(boundary_exactly_at_tolerance_aligns)
{
    TimestampAligner aligner(5'000);
    AM_EXPECT(!aligner.AddSample(0, 1'000'000).has_value());
    const auto pair = aligner.AddSample(1, 1'005'000);
    AM_EXPECT(pair.has_value());
    AM_EXPECT_EQ(pair->delta_ns, -5'000LL);
}

AM_TEST(reset_clears_pending_samples)
{
    TimestampAligner aligner(5'000);
    AM_EXPECT(!aligner.AddSample(0, 1'000'000).has_value());
    aligner.Reset();

    // 重置后旧样本不再参与配对：只保留 B 路新样本
    AM_EXPECT(!aligner.AddSample(1, 2'000'000).has_value());
    AM_EXPECT(!aligner.AddSample(1, 2'010'000).has_value());
}

AM_TEST(monotonic_stream_sequence)
{
    TimestampAligner   aligner(2'000);
    const std::int64_t kInterval = 33'333;  // 约 30fps
    int                matches   = 0;
    for (int i = 0; i < 100; ++i)
    {
        const std::int64_t ts = 1'000'000 + i * kInterval;
        aligner.AddSample(0, ts);
        const auto pair = aligner.AddSample(1, ts + 500);
        if (pair.has_value())
        {
            ++matches;
            AM_EXPECT(pair->timestamp_a_ns == ts);
        }
    }
    AM_EXPECT(matches > 90);  // 漂移 500ns << 2ms 容差，几乎全部配对
}

}  // namespace

int main() { return am_test::RunAll(); }
