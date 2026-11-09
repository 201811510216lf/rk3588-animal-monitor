#pragma once

// animal_monitor/fusion/anomaly_detector.h
// 状态：implemented（host 可编译，纯逻辑，无外部依赖）
// 依据：任务书 CODEX_MASTER_TASK.md §10（Anomaly detector：先规则和个体基线，后续再加模型）；
//       AGENTS.md（C++17、k 常量、无全局可变状态）；
//       审计结论：源项目 A/B 无异常检测实现，本模块为全新实现。
// 说明：
//   - 规则 + z-score 判定：基线样本数不足不判异常；stddev==0 时仅走绝对阈值规则；
//     接入模型前只做规则（任务书 §10 要求）。行为相关不等于因果，异常仅为统计提示。
//   - 阈值语义：abs_min / abs_max 为 0 表示不启用（无法表达“恰为 0 的阈值”，
//     调用方如需 0 阈值可用极小正/负数近似）。
//   - 输入温度仍是未标定估算（NOT_CALIBRATED），阈值需按实测数据设定。

#include <cstdint>

namespace animal_monitor::fusion {

// 异常判定配置。
struct AnomalyConfig
{
    float         z_threshold          = 3.0f;  // |z| >= z_threshold 判异常；调用方应保证 > 0
    std::uint32_t min_baseline_samples = 10;    // 基线样本数低于该值不判异常
    float         abs_min              = 0.0f;  // 绝对下限（°C）；0 = 不启用
    float         abs_max              = 0.0f;  // 绝对上限（°C）；0 = 不启用
};

// 判定结果。
struct AnomalyResult
{
    bool   is_anomaly = false;
    enum class Reason
    {
        kNone,              // 无异常
        kZScore,            // z 分数超阈值
        kAbsolute,          // 绝对阈值越界（含非有限输入值）
        kBaselineTooSmall,  // 基线样本不足，不判定
    };
    Reason reason  = Reason::kNone;
    float  z_score = 0.0f;  // (value - mean) / stddev；stddev==0 或样本不足时为 0
};

// 纯规则判定。判定优先级：样本不足 > 绝对阈值 > z-score。
// 输入值非有限视为绝对异常（is_anomaly=true, reason=kAbsolute, z_score=0）；
// 基线 mean / stddev 非有限视为无法判定（reason=kBaselineTooSmall）。
AnomalyResult Evaluate(const AnomalyConfig& config, float value, std::uint32_t baseline_count,
                       float baseline_mean, float baseline_stddev);

}  // namespace animal_monitor::fusion
