#pragma once

// animal_monitor/inference/i_inference_model.h
// 状态：implemented（接口定义，host 可编译）
// 说明：推理模型抽象接口。实现类：
//   - RknnYolov5Model（目标板专用，hardware-blocked / UNVERIFIED）；
//   - MockInferenceModel（宿主测试/流水线联调，MOCK）。

#include "animal_monitor/common/status.h"
#include "animal_monitor/inference/inference_types.h"
#include "animal_monitor/inference/model_manifest.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor::inference
{

// 推理模型抽象。所有权与并发约定：
//   - Load 失败后对象不可用，须重新 Load；
//   - Infer 为同步调用（Phase 3 约定）；frame 仅在调用期间有效；
//   - 调度器不拥有模型指针，生命周期由应用层管理。
class IInferenceModel
{
   public:
    virtual ~IInferenceModel() = default;

    // 模型名：用于日志与调度指标键，构造后应保持稳定（不得在运行中改名）。
    virtual const char* Name() const = 0;

    // 加载模型。manifest 由 ModelRegistry 校验后传入。
    virtual Status Load(const ModelManifest& manifest) = 0;

    // 执行推理。result 至少填写 valid / model_id / model_version / detections；
    // 帧标识字段由调度器统一覆写。
    virtual Status Infer(const VideoFrame& frame, InferenceResult* result) = 0;

    // 当前生效的 manifest 副本（Load 成功后有效；未加载时为默认值）。
    virtual ModelManifest Manifest() const = 0;
};

}  // namespace animal_monitor::inference
