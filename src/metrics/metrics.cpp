#include "animal_monitor/metrics/metrics.h"

#include <algorithm>
#include <sstream>

namespace animal_monitor
{

void Counter::Increment(std::uint64_t amount)
{
    value_.fetch_add(amount, std::memory_order_relaxed);
}

std::uint64_t Counter::Value() const { return value_.load(std::memory_order_relaxed); }

void Gauge::Set(std::int64_t value) { value_.store(value, std::memory_order_relaxed); }

std::int64_t Gauge::Value() const { return value_.load(std::memory_order_relaxed); }

Histogram::Histogram(std::size_t max_samples)
    : max_samples_(max_samples == 0 ? 1 : max_samples), samples_(max_samples_)
{}

void Histogram::Record(std::int64_t value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    samples_[next_] = value;
    next_           = (next_ + 1) % max_samples_;
    ++total_records_;
}

namespace
{

// 线性插值分位数；data 必须已排序。
double Percentile(const std::vector<std::int64_t>& data, double q)
{
    if (data.empty())
    {
        return 0.0;
    }
    if (data.size() == 1)
    {
        return static_cast<double>(data[0]);
    }
    const double pos  = q * static_cast<double>(data.size() - 1);
    const auto   lo   = static_cast<std::size_t>(pos);
    const auto   hi   = lo + 1 < data.size() ? lo + 1 : lo;
    const double frac = pos - static_cast<double>(lo);
    return static_cast<double>(data[lo]) * (1.0 - frac) + static_cast<double>(data[hi]) * frac;
}

}  // namespace

Histogram::Snapshot Histogram::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const std::size_t count =
        total_records_ < max_samples_ ? static_cast<std::size_t>(total_records_) : max_samples_;
    Snapshot snap;
    if (count == 0)
    {
        return snap;
    }

    std::vector<std::int64_t> sorted(samples_.begin(), samples_.end());
    std::sort(sorted.begin(), sorted.end());

    snap.count = count;
    snap.min   = sorted.front();
    snap.max   = sorted.back();
    double sum = 0.0;
    for (const auto v : sorted)
    {
        sum += static_cast<double>(v);
    }
    snap.mean = sum / static_cast<double>(count);
    snap.p50  = Percentile(sorted, 0.50);
    snap.p90  = Percentile(sorted, 0.90);
    snap.p99  = Percentile(sorted, 0.99);
    return snap;
}

MetricsRegistry& MetricsRegistry::Instance()
{
    static MetricsRegistry instance;
    return instance;
}

MetricsRegistry::MetricsRegistry() = default;

Counter& MetricsRegistry::GetCounter(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto&                       entry = counters_[name];
    if (!entry)
    {
        entry = std::make_unique<Counter>();
    }
    return *entry;
}

Gauge& MetricsRegistry::GetGauge(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto&                       entry = gauges_[name];
    if (!entry)
    {
        entry = std::make_unique<Gauge>();
    }
    return *entry;
}

std::string MetricsRegistry::Dump() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream          out;
    for (const auto& [name, counter] : counters_)
    {
        out << name << '=' << counter->Value() << '\n';
    }
    for (const auto& [name, gauge] : gauges_)
    {
        out << name << '=' << gauge->Value() << '\n';
    }
    return out.str();
}

}  // namespace animal_monitor
