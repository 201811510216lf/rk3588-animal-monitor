#pragma once

// animal_monitor/inference/inference_scheduler.h
// 状态：implemented（host 可编译）
// 说明：多模型推理调度。
//   - Phase 3 采用同步直调（提交线程内执行推理）；并发策略待目标板实测后
//     替换为线程池 + 异步队列（RK3588 NPU 三核与多路并发的配比需板卡数据）；
//   - 结果写入有界队列（容量配置化，drop_oldest：结果侧优先保留最新推理输出）；
//   - 同一帧按 frame_id 去重（每路仅保留最近一次提交），重复提交返回 kAlreadyExists
//     （幂等，非致命）；
//   - 每模型计数指标（routed/executed/failed/queued）；
//   - 调度器不拥有模型指针，生命周期由调用方管理。

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "animal_monitor/common/bounded_queue.h"
#include "animal_monitor/common/frame_meta.h"
#include "animal_monitor/common/status.h"
#include "animal_monitor/inference/i_inference_model.h"
#include "animal_monitor/inference/inference_types.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor::inference
{

// 路由策略。kPriority 当前为固定选择 models[0]（多级优先级/降级策略后续扩展）。
enum class ScheduleStrategy
{
    kRoundRobin,  // 模型间轮转
    kPriority,    // 首个模型优先（Phase 3 简化实现）
    kPerCamera,   // camera_id % 模型数
};

class InferenceScheduler
{
   public:
    struct Options
    {
        std::size_t result_queue_capacity = 16;  // 有界队列容量（满策略固定 drop_oldest）
    };

    // 单模型计数指标。
    struct ModelMetrics
    {
        std::uint64_t routed   = 0;  // 路由命中次数
        std::uint64_t executed = 0;  // 实际执行推理次数
        std::uint64_t failed   = 0;  // 推理返回错误次数
        std::uint64_t queued   = 0;  // 结果入队次数
    };

    struct Metrics
    {
        std::uint64_t                       submitted     = 0;  // 受理的提交数（含去重命中）
        std::uint64_t                       dedup_skipped = 0;  // frame_id 去重跳过次数
        std::uint64_t                       dropped = 0;  // 结果队列满丢弃（drop_oldest 换出）
        std::map<std::string, ModelMetrics> per_model;    // 键 = 模型 Name()
    };

    // models 可为空（Submit 将返回 kConfigurationError）。
    InferenceScheduler(ScheduleStrategy strategy, std::vector<IInferenceModel*> models,
                       Options options = Options());

    InferenceScheduler(const InferenceScheduler&)            = delete;
    InferenceScheduler& operator=(const InferenceScheduler&) = delete;

    // 提交一帧：去重 → 路由 → 同步推理 → 结果入队。
    // 返回：kOk 成功；kAlreadyExists 去重命中（幂等）；kCancelled 已关闭；
    // kConfigurationError 未配置模型；其余为模型推理错误（结果不入队）。
    Status Submit(const VideoFrame& frame, std::uint32_t camera_id);

    // 取结果；超时返回 kTimeout；已关闭且队列空返回 kCancelled。timeout<=0 表示非阻塞。
    Status GetResult(InferenceResult* result, std::chrono::milliseconds timeout);

    // 关闭：不再受理新提交；已入队结果仍可取出。
    void Close();

    Metrics GetMetrics() const;

   private:
    std::size_t RouteLocked(std::uint32_t camera_id);  // 调用方须持有 state_mutex_

    ScheduleStrategy                 strategy_;
    std::vector<IInferenceModel*>    models_;
    Options                          options_;
    BoundedQueue<InferenceResult>    results_;
    std::atomic<bool>                closed_{false};
    mutable std::mutex               state_mutex_;
    std::uint64_t                    round_robin_counter_ = 0;
    std::map<std::uint32_t, FrameId> last_frame_by_camera_;  // 每路最近提交的 frame_id
    Metrics                          metrics_;
};

}  // namespace animal_monitor::inference
