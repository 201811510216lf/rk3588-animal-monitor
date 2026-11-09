#pragma once

// animal_monitor/fusion/feature_fusion.h
// 状态：implemented（host 可编译，纯逻辑，无外部依赖）
// 依据：任务书 CODEX_MASTER_TASK.md §10（Behavior-temperature fusion：行为类别与温度特征
//       时间窗口关联，不要求每个 RGB 帧都有红外帧）；
//       AGENTS.md（C++17、k 常量、尾下划线成员、单调时间戳、无全局可变状态）；
//       审计结论：源项目 A/B 无行为-温度融合实现，本模块为全新实现。
// 说明：
//   - Snapshot(now) 统计滑窗 [now - window_ns, now] 内的行为事件；对每个行为，
//     把落在其任一事件尾随时间窗 [event_ns - window_ns, event_ns] 内、且仍在快照窗口内
//     的热样本去重后聚合（thermal_mean = 样本 mean_c 的均值，thermal_max = 样本 max_c
//     的最大值）；同一热样本被同行为多个事件覆盖时只计一次。
//   - 过期淘汰：Add* 时以“最新见到的样本时间戳”为参考淘汰过期条目；Snapshot 为只读
//     过滤，不修改内部状态。事件与热样本队列均有界（max_events），超限丢弃最旧。
//   - 时间戳语义为单调时钟纳秒（与流水线一致），期望非递减输入；乱序输入不检测，
//     淘汰结果可能不确定。
//   - 行为字符串不做校验（空串由调用方避免）；热样本 mean_c/max_c 非有限时忽略。
//   - 温度值仍是未标定估算（NOT_CALIBRATED）；行为相关不等于因果（任务书 §10）。

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace animal_monitor::fusion
{

// 一次行为事件（来自行为检测/跟踪模块）。
struct BehaviorEvent
{
    std::int64_t  monotonic_ns = 0;  // 单调时钟纳秒
    std::string   behavior;
    std::uint32_t camera_id = 0;
};

// 一次热像样本（按个体/ROI 汇总后的温度特征，NOT_CALIBRATED）。
struct ThermalSample
{
    std::int64_t monotonic_ns = 0;
    float        mean_c       = 0.0f;
    float        max_c        = 0.0f;
};

// 行为-温度时间窗融合（纯逻辑，无外部依赖）。
class FeatureFusion
{
   public:
    // 单行为聚合结果。
    struct BehaviorStats
    {
        std::string   behavior;
        std::uint32_t event_count   = 0;     // 窗口内该行为事件数
        std::uint32_t thermal_count = 0;     // 去重后的热样本数
        float         thermal_mean  = 0.0f;  // 热样本 mean_c 的均值；无样本为 0
        float         thermal_max   = 0.0f;  // 热样本 max_c 的最大值；无样本为 0
    };

    // window_ns：滑窗长度（<1 按 1 处理）；max_events：事件与热样本各自的容量上限
    // （<1 按 1 处理，保证内存有界）。
    FeatureFusion(std::int64_t window_ns, std::uint32_t max_events);

    void AddBehaviorEvent(const BehaviorEvent& event);
    void AddThermalSample(const ThermalSample& sample);

    // 生成快照（只读，不修改内部状态）：行为按名称字典序排列；
    // 无窗口内行为事件时返回空。
    std::vector<BehaviorStats> Snapshot(std::int64_t now_ns) const;

   private:
    const std::int64_t        window_ns_;
    const std::uint32_t       max_events_;
    std::deque<BehaviorEvent> events_;
    std::deque<ThermalSample> samples_;
    std::int64_t              latest_ns_  = 0;  // 最新见到的样本时间戳（Add 时淘汰参考）
    bool                      has_latest_ = false;
};

}  // namespace animal_monitor::fusion
