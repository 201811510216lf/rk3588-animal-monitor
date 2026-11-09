#include "animal_monitor/inference/inference_scheduler.h"

namespace animal_monitor::inference {

InferenceScheduler::InferenceScheduler(ScheduleStrategy strategy,
                                       std::vector<IInferenceModel*> models, Options options)
    : strategy_(strategy),
      models_(std::move(models)),
      options_(options),
      results_(options_.result_queue_capacity, BoundedQueue<InferenceResult>::PushPolicy::kDropOldest)
{
}

Status InferenceScheduler::Submit(const VideoFrame& frame, std::uint32_t camera_id)
{
    if (closed_.load(std::memory_order_acquire))
    {
        return Status(StatusCode::kCancelled, "调度器已关闭");
    }

    std::size_t model_index = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (models_.empty())
        {
            return Status(StatusCode::kConfigurationError, "未配置推理模型");
        }
        ++metrics_.submitted;

        // 同一帧不得重复推理：按 frame_id 去重，每路仅保留最近一次提交。
        const auto it = last_frame_by_camera_.find(frame.meta.id.camera_id);
        if (it != last_frame_by_camera_.end() &&
            it->second.sequence == frame.meta.id.sequence)
        {
            ++metrics_.dedup_skipped;
            return Status(StatusCode::kAlreadyExists, "帧已提交过（frame_id 去重），跳过重复推理");
        }
        last_frame_by_camera_[frame.meta.id.camera_id] = frame.meta.id;
        model_index = RouteLocked(camera_id);
    }

    // Phase 3：同步直调。并发策略待目标板实测后替换为线程池 + 异步提交队列。
    IInferenceModel* model  = models_[model_index];
    InferenceResult  result;
    Status           status = model->Infer(frame, &result);

    // 调度器统一覆写帧标识，保证与提交帧一致。
    result.frame_id     = frame.meta.id;
    result.camera_id    = camera_id;
    result.monotonic_ns = frame.meta.timestamps.monotonic_ns;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ModelMetrics& metrics = metrics_.per_model[model->Name()];
        ++metrics.executed;
        if (!status)
        {
            ++metrics.failed;
            return status;
        }
    }
    if (!results_.TryPush(std::move(result)))
    {
        // kDropOldest 策略下仅关闭时失败。
        std::lock_guard<std::mutex> lock(state_mutex_);
        ModelMetrics& metrics = metrics_.per_model[model->Name()];
        ++metrics.failed;
        return Status(StatusCode::kUnavailable, "结果队列已关闭");
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++metrics_.per_model[model->Name()].queued;
    }
    return Status::MakeOk();
}

Status InferenceScheduler::GetResult(InferenceResult* result, std::chrono::milliseconds timeout)
{
    if (result == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "GetResult: result 输出参数为空");
    }
    if (timeout <= std::chrono::milliseconds::zero())
    {
        if (results_.TryPop(*result))
        {
            return Status::MakeOk();
        }
    }
    else if (results_.Pop(*result, timeout))
    {
        return Status::MakeOk();
    }
    if (results_.IsClosed())
    {
        return Status(StatusCode::kCancelled, "调度器已关闭且无剩余结果");
    }
    return Status(StatusCode::kTimeout, "等待结果超时");
}

void InferenceScheduler::Close()
{
    closed_.store(true, std::memory_order_release);
    results_.Close();
}

InferenceScheduler::Metrics InferenceScheduler::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    Metrics                       metrics = metrics_;
    metrics.dropped = results_.GetMetrics().drops;
    return metrics;
}

std::size_t InferenceScheduler::RouteLocked(std::uint32_t camera_id)
{
    std::size_t index = 0;
    switch (strategy_)
    {
        case ScheduleStrategy::kRoundRobin:
            index = static_cast<std::size_t>(round_robin_counter_ % models_.size());
            ++round_robin_counter_;
            break;
        case ScheduleStrategy::kPriority:
            // Phase 3 简化：固定首选 models[0]；权重/失败降级策略后续扩展。
            index = 0;
            break;
        case ScheduleStrategy::kPerCamera:
            index = static_cast<std::size_t>(camera_id % models_.size());
            break;
    }
    ++metrics_.per_model[models_[index]->Name()].routed;
    return index;
}

}  // namespace animal_monitor::inference
