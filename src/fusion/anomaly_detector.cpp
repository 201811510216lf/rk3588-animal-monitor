// ============================================================================
// 文件：src/fusion/anomaly_detector.cpp
// 状态：implemented（纯规则逻辑，host 可编译；MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md §10；include/animal_monitor/fusion/anomaly_detector.h
// 说明：仅规则引擎，不接入模型（任务书 §10：先规则和个体基线，后续再加模型）。
// ============================================================================

#include "animal_monitor/fusion/anomaly_detector.h"

#include <cmath>

namespace animal_monitor::fusion
{

AnomalyResult Evaluate(const AnomalyConfig& config, float value, std::uint32_t baseline_count,
                       float baseline_mean, float baseline_stddev)
{
    AnomalyResult result;

    // 基线样本不足或基线统计量非有限：无法判定，不报异常。
    if (baseline_count < config.min_baseline_samples || !std::isfinite(baseline_mean) ||
        !std::isfinite(baseline_stddev))
    {
        result.reason = AnomalyResult::Reason::kBaselineTooSmall;
        return result;
    }

    // 绝对阈值规则（含非有限输入值，一律视为绝对异常）。
    const bool below_min = config.abs_min != 0.0f && value < config.abs_min;
    const bool above_max = config.abs_max != 0.0f && value > config.abs_max;
    if (!std::isfinite(value) || below_min || above_max)
    {
        result.is_anomaly = true;
        result.reason     = AnomalyResult::Reason::kAbsolute;
        return result;
    }

    // z-score 规则。stddev==0 时 z 记为 0，仅绝对阈值生效（无法区分偏差）。
    if (baseline_stddev > 0.0f)
    {
        const float z  = (value - baseline_mean) / baseline_stddev;
        result.z_score = z;
        if (std::fabs(z) >= config.z_threshold)
        {
            result.is_anomaly = true;
            result.reason     = AnomalyResult::Reason::kZScore;
        }
    }
    return result;
}

}  // namespace animal_monitor::fusion
