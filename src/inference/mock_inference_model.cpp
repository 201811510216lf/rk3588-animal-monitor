#include "animal_monitor/inference/mock_inference_model.h"

#include <cstddef>

namespace animal_monitor::inference {

namespace {

constexpr float       kMockConfidence        = 0.9f;
constexpr std::size_t kMockDefaultClassCount = 3;

}  // namespace

Status MockInferenceModel::Load(const ModelManifest& manifest)
{
    std::lock_guard<std::mutex> lock(mutex_);
    manifest_ = manifest;
    return Status::MakeOk();
}

Status MockInferenceModel::Infer(const VideoFrame& frame, InferenceResult* result)
{
    if (result == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "MockInferenceModel::Infer: result 为空");
    }
    *result       = InferenceResult{};
    result->valid = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result->model_id      = manifest_.model_id.empty() ? name_ : manifest_.model_id;
        result->model_version = manifest_.version.empty() ? "0.0.1-mock" : manifest_.version;
    }

    const float width  = static_cast<float>(frame.meta.width);
    const float height = static_cast<float>(frame.meta.height);
    if (width > 0.0f && height > 0.0f)
    {
        std::size_t class_count = kMockDefaultClassCount;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!manifest_.classes.empty())
            {
                class_count = manifest_.classes.size();
            }
        }
        Detection detection;
        detection.class_id = static_cast<std::int32_t>(frame.meta.id.sequence % class_count);
        detection.bbox =
            Detection::Bbox{width * 0.375f, height * 0.375f, width * 0.25f, height * 0.25f};
        detection.confidence = kMockConfidence;
        result->detections.push_back(detection);
    }
    return Status::MakeOk();
}

ModelManifest MockInferenceModel::Manifest() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return manifest_;
}

}  // namespace animal_monitor::inference
