// ============================================================================
// 文件：src/fusion/feature_fusion.cpp
// 状态：implemented（纯逻辑，host 可编译；MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md §10；include/animal_monitor/fusion/feature_fusion.h
// ============================================================================

#include "animal_monitor/fusion/feature_fusion.h"

#include <cmath>
#include <map>

namespace animal_monitor::fusion
{

namespace
{

// 过期判断：时间戳早于 (now_ns - window_ns) 即过期。
bool IsExpired(std::int64_t ts, std::int64_t now_ns, std::int64_t window_ns)
{
    return ts < now_ns - window_ns;
}

}  // namespace

FeatureFusion::FeatureFusion(std::int64_t window_ns, std::uint32_t max_events)
    : window_ns_(window_ns < 1 ? 1 : window_ns), max_events_(max_events < 1u ? 1u : max_events)
{}

void FeatureFusion::AddBehaviorEvent(const BehaviorEvent& event)
{
    const std::int64_t now =
        (has_latest_ && latest_ns_ > event.monotonic_ns) ? latest_ns_ : event.monotonic_ns;
    has_latest_ = true;
    latest_ns_  = now;

    // 过期淘汰：从队头移除早于 (now - window) 的条目（期望时间戳非递减）。
    while (!events_.empty() && IsExpired(events_.front().monotonic_ns, now, window_ns_))
    {
        events_.pop_front();
    }
    events_.push_back(event);
    while (events_.size() > max_events_)
    {
        events_.pop_front();  // 容量上限：丢弃最旧
    }
}

void FeatureFusion::AddThermalSample(const ThermalSample& sample)
{
    const std::int64_t now =
        (has_latest_ && latest_ns_ > sample.monotonic_ns) ? latest_ns_ : sample.monotonic_ns;
    has_latest_ = true;
    latest_ns_  = now;

    while (!samples_.empty() && IsExpired(samples_.front().monotonic_ns, now, window_ns_))
    {
        samples_.pop_front();
    }
    samples_.push_back(sample);
    while (samples_.size() > max_events_)
    {
        samples_.pop_front();
    }
}

std::vector<FeatureFusion::BehaviorStats> FeatureFusion::Snapshot(std::int64_t now_ns) const
{
    std::vector<BehaviorStats> result;
    const std::int64_t         window_start = now_ns - window_ns_;

    // 收集窗口内行为事件，按行为分组（std::map 键序 = 名称字典序，保证输出确定性）。
    std::map<std::string, std::vector<std::int64_t>> events_by_behavior;
    for (const BehaviorEvent& event : events_)
    {
        if (event.monotonic_ns < window_start || event.monotonic_ns > now_ns)
        {
            continue;
        }
        events_by_behavior[event.behavior].push_back(event.monotonic_ns);
    }
    if (events_by_behavior.empty())
    {
        return result;  // 无窗口内行为事件：返回空
    }

    // 收集窗口内、数值有限的热样本（Snapshot 只读，快照期间索引稳定）。
    std::vector<std::size_t> valid_samples;
    for (std::size_t i = 0; i < samples_.size(); ++i)
    {
        const ThermalSample& sample = samples_[i];
        if (sample.monotonic_ns < window_start || sample.monotonic_ns > now_ns)
        {
            continue;
        }
        if (!std::isfinite(sample.mean_c) || !std::isfinite(sample.max_c))
        {
            continue;
        }
        valid_samples.push_back(i);
    }

    for (const auto& entry : events_by_behavior)
    {
        const std::vector<std::int64_t>& event_times = entry.second;

        std::uint32_t thermal_count = 0;
        double        mean_sum      = 0.0;
        float         max_value     = 0.0f;
        bool          has_max       = false;

        for (const std::size_t index : valid_samples)
        {
            const ThermalSample& sample  = samples_[index];
            bool                 covered = false;
            for (const std::int64_t event_ns : event_times)
            {
                // 样本落在该事件尾随时间窗 [event_ns - window, event_ns] 内。
                if (sample.monotonic_ns >= event_ns - window_ns_ && sample.monotonic_ns <= event_ns)
                {
                    covered = true;
                    break;
                }
            }
            if (!covered)
            {
                continue;
            }
            // 每个有效样本对本行为只计一次（去重）。
            ++thermal_count;
            mean_sum += static_cast<double>(sample.mean_c);
            if (!has_max || sample.max_c > max_value)
            {
                max_value = sample.max_c;
                has_max   = true;
            }
        }

        BehaviorStats stats;
        stats.behavior      = entry.first;
        stats.event_count   = static_cast<std::uint32_t>(event_times.size());
        stats.thermal_count = thermal_count;
        if (thermal_count > 0u)
        {
            stats.thermal_mean = static_cast<float>(mean_sum / static_cast<double>(thermal_count));
            stats.thermal_max  = max_value;
        }
        result.push_back(std::move(stats));
    }
    return result;
}

}  // namespace animal_monitor::fusion
