#pragma once

// animal_monitor/inference/mock_inference_model.h
// 状态：implemented（MOCK —— 仅宿主单元测试/流水线联调用，不代表真实推理，
//       不得用于任何板卡实测结论）
// 说明：确定性检测输出：中心 1/4 区域一个框，置信度 0.9，
//       类别 = 帧序号 % 类别数（manifest.classes 为空时按 3 类）。

#include <mutex>
#include <string>

#include "animal_monitor/inference/i_inference_model.h"

namespace animal_monitor::inference {

class MockInferenceModel : public IInferenceModel
{
  public:
    MockInferenceModel() = default;
    explicit MockInferenceModel(std::string model_id) : name_(std::move(model_id)) {}

    // 构造后稳定；Load 不改变 Name（避免调度指标键漂移）。
    const char* Name() const override { return name_.c_str(); }

    // 仅记录 manifest 副本，恒返回成功。
    Status Load(const ModelManifest& manifest) override;

    // 确定性输出（MOCK）。帧宽高为 0 时返回 ok + 空检测，保证链路可跑通。
    Status Infer(const VideoFrame& frame, InferenceResult* result) override;

    ModelManifest Manifest() const override;

  private:
    mutable std::mutex mutex_;
    std::string        name_ = "mock_detector";
    ModelManifest      manifest_;
};

}  // namespace animal_monitor::inference
