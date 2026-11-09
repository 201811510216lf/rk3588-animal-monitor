#include <string>

#include "test_harness.h"

#include "animal_monitor/metrics/metrics.h"

using animal_monitor::Counter;
using animal_monitor::Gauge;
using animal_monitor::Histogram;
using animal_monitor::MetricsRegistry;

namespace
{

AM_TEST(counter_increment_and_value)
{
    Counter counter;
    AM_EXPECT_EQ(counter.Value(), 0u);
    counter.Increment();
    counter.Increment(4);
    AM_EXPECT_EQ(counter.Value(), 5u);
}

AM_TEST(gauge_set_and_value)
{
    Gauge gauge;
    AM_EXPECT_EQ(gauge.Value(), 0);
    gauge.Set(-42);
    AM_EXPECT_EQ(gauge.Value(), -42);
}

AM_TEST(histogram_empty_snapshot)
{
    Histogram  histogram(64);
    const auto snapshot = histogram.GetSnapshot();
    AM_EXPECT_EQ(snapshot.count, 0u);
}

AM_TEST(histogram_percentiles_on_known_data)
{
    Histogram histogram(100);
    for (std::int64_t i = 1; i <= 100; ++i)
    {
        histogram.Record(i);
    }
    const auto snapshot = histogram.GetSnapshot();
    AM_EXPECT_EQ(snapshot.count, 100u);
    AM_EXPECT_EQ(snapshot.min, 1);
    AM_EXPECT_EQ(snapshot.max, 100);
    AM_EXPECT(snapshot.p50 >= 50.0 && snapshot.p50 <= 51.0);
    AM_EXPECT(snapshot.p90 >= 90.0 && snapshot.p90 <= 91.0);
    AM_EXPECT(snapshot.p99 >= 99.0 && snapshot.p99 <= 100.0);
}

AM_TEST(histogram_wraps_to_recent_samples)
{
    Histogram histogram(4);
    histogram.Record(100);
    histogram.Record(100);
    histogram.Record(100);
    histogram.Record(100);
    histogram.Record(0);  // 覆盖最旧样本
    const auto snapshot = histogram.GetSnapshot();
    AM_EXPECT_EQ(snapshot.count, 4u);
    AM_EXPECT_EQ(snapshot.min, 0);
    AM_EXPECT_EQ(snapshot.max, 100);
}

AM_TEST(registry_returns_same_object_and_dumps)
{
    MetricsRegistry& registry = MetricsRegistry::Instance();
    registry.GetCounter("test_counter").Increment(3);
    registry.GetGauge("test_gauge").Set(7);

    AM_EXPECT_EQ(registry.GetCounter("test_counter").Value(), 3u);
    AM_EXPECT_EQ(registry.GetGauge("test_gauge").Value(), 7);

    const std::string dump = registry.Dump();
    AM_EXPECT(dump.find("test_counter=3") != std::string::npos);
    AM_EXPECT(dump.find("test_gauge=7") != std::string::npos);
}

}  // namespace

int main() { return am_test::RunAll(); }
