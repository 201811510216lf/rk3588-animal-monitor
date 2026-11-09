// ============================================================================
// 文件：src/fusion/individual_baseline.cpp
// 状态：implemented（纯逻辑，host 可编译；MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md §10；include/animal_monitor/fusion/individual_baseline.h
// 说明：均值/标准差使用 double 累加以减小截断误差；窗口有界（每 ID 至多 max_samples_per_id 个）。
// ============================================================================

#include "animal_monitor/fusion/individual_baseline.h"

#include <cmath>

namespace animal_monitor::fusion {

IndividualBaseline::IndividualBaseline(std::uint32_t max_samples_per_id)
    : max_samples_per_id_(max_samples_per_id < 1u ? 1u : max_samples_per_id)
{
}

void IndividualBaseline::AddSample(std::int32_t id, float value)
{
    if (!std::isfinite(value))
    {
        return;  // 非有限值忽略；该 ID 若不存在也不创建空窗口
    }
    std::deque<float>& window = samples_[id];
    window.push_back(value);
    while (window.size() > max_samples_per_id_)
    {
        window.pop_front();
    }
}

bool IndividualBaseline::GetStats(std::int32_t id, std::uint32_t* count, float* mean,
                                  float* stddev) const
{
    const auto it = samples_.find(id);
    if (it == samples_.end() || it->second.empty())
    {
        if (count != nullptr)
        {
            *count = 0u;
        }
        if (mean != nullptr)
        {
            *mean = 0.0f;
        }
        if (stddev != nullptr)
        {
            *stddev = 0.0f;
        }
        return false;
    }

    const std::deque<float>& window = it->second;
    const std::size_t        n      = window.size();
    double                   sum    = 0.0;
    for (const float sample : window)
    {
        sum += static_cast<double>(sample);
    }
    const double mean_value = sum / static_cast<double>(n);

    double sq_sum = 0.0;
    for (const float sample : window)
    {
        const double delta = static_cast<double>(sample) - mean_value;
        sq_sum += delta * delta;
    }

    if (count != nullptr)
    {
        *count = static_cast<std::uint32_t>(n);
    }
    if (mean != nullptr)
    {
        *mean = static_cast<float>(mean_value);
    }
    if (stddev != nullptr)
    {
        *stddev = static_cast<float>(std::sqrt(sq_sum / static_cast<double>(n)));
    }
    return true;
}

std::size_t IndividualBaseline::IdCount() const
{
    return samples_.size();
}

}  // namespace animal_monitor::fusion
