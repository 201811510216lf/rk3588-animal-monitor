#pragma once

// animal_monitor/fusion/individual_baseline.h
// 状态：implemented（host 可编译，纯逻辑，无外部依赖）
// 依据：任务书 CODEX_MASTER_TASK.md §10（Individual baseline：按动物 ID 维护个体时间序列基线）；
//       AGENTS.md（C++17、k 常量、尾下划线成员、无全局可变状态）；
//       审计结论：源项目 A/B 无个体基线实现，本模块为全新实现。
// 说明：
//   - 按动物个体 ID 维护温度特征滚动基线（滑动窗口环形缓冲，每 ID 窗口样本数上限固定）。
//   - 纯逻辑：只做窗口内 count / mean / stddev（总体标准差，除以 n）统计，
//     不臆造生理模型；温度值本身仍是未标定估算（NOT_CALIBRATED）。
//   - 个体 ID 集合随运行增长；过期个体清理策略由调用方决定，本模块不自动清理。

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace animal_monitor::fusion
{

// 按个体 ID 的滑动窗口统计基线。
class IndividualBaseline
{
   public:
    // max_samples_per_id：每个 ID 保留的最近样本数上限（<1 时按 1 处理，保证窗口有界）。
    explicit IndividualBaseline(std::uint32_t max_samples_per_id);

    // 追加样本；非有限值忽略（不改变该 ID 窗口）。超过窗口上限丢弃最旧样本。
    void AddSample(std::int32_t id, float value);

    // 查询统计：count 为窗口内样本数，mean 为均值，stddev 为总体标准差（除以 n，n=1 时为 0）。
    // ID 不存在或无样本时返回 false，并将 *count/*mean/*stddev 置 0（指针非空时）。
    bool GetStats(std::int32_t id, std::uint32_t* count, float* mean, float* stddev) const;

    // 已建立基线的个体数量。
    std::size_t IdCount() const;

   private:
    const std::uint32_t                       max_samples_per_id_;
    std::map<std::int32_t, std::deque<float>> samples_;
};

}  // namespace animal_monitor::fusion
