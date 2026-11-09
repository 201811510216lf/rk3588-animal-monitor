#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace animal_monitor
{

// 单调递增计数器（线程安全）。
class Counter
{
   public:
    void          Increment(std::uint64_t amount = 1);
    std::uint64_t Value() const;

   private:
    std::atomic<std::uint64_t> value_{0};
};

// 数值仪表（线程安全）。
class Gauge
{
   public:
    void         Set(std::int64_t value);
    std::int64_t Value() const;

   private:
    std::atomic<std::int64_t> value_{0};
};

// 有界样本直方图：延迟分位数（p50/p90/p99）等 KPI 统计。
// 采样数超过上限时循环覆盖（保留最近样本）。分位数采用线性插值
// （等价于 numpy percentile 默认算法），样本不足时按可用样本计算。
class Histogram
{
   public:
    struct Snapshot
    {
        std::uint64_t count = 0;
        double        mean  = 0.0;
        std::int64_t  min   = 0;
        std::int64_t  max   = 0;
        double        p50   = 0.0;
        double        p90   = 0.0;
        double        p99   = 0.0;
    };

    explicit Histogram(std::size_t max_samples);

    void     Record(std::int64_t value);
    Snapshot GetSnapshot() const;

   private:
    mutable std::mutex        mutex_;
    const std::size_t         max_samples_;
    std::vector<std::int64_t> samples_;
    std::size_t               next_          = 0;
    std::uint64_t             total_records_ = 0;
};

// 全局指标注册表（单例）：计数器与仪表按名字索引，可整体导出文本快照。
class MetricsRegistry
{
   public:
    static MetricsRegistry& Instance();

    Counter& GetCounter(const std::string& name);
    Gauge&   GetGauge(const std::string& name);

    // 导出 "name=value" 逐行文本快照（计数器/仪表，排序稳定）。
    std::string Dump() const;

   private:
    MetricsRegistry();

    mutable std::mutex                              mutex_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>>   gauges_;
};

}  // namespace animal_monitor
