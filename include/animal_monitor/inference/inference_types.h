#pragma once

// animal_monitor/inference/inference_types.h
// 状态：implemented（host 可编译，无外部依赖）
// 说明：推理模块公共数据类型。检测框坐标按模型输入尺寸输出（未缩放回原图），
//       还原到原始分辨率由应用层按预处理策略完成；坐标不裁剪（A 版在输出阶段
//       clamp，本实现保留原始值供应用决策，避免信息丢失）。

#include <cstdint>
#include <string>
#include <vector>

#include "animal_monitor/common/frame_meta.h"

namespace animal_monitor::inference {

// 单条检测结果。bbox 为左上角 (x, y) 与宽高 (w, h)，单位：模型输入像素。
struct Detection
{
    struct Bbox
    {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
    };

    std::int32_t class_id = 0;  // 类别索引（对应 manifest.classes；未映射时为原始 argmax 索引）
    Bbox         bbox;
    float        confidence = 0.0f;  // box_conf * class_conf（必须赋值，见审计 B 缺陷 #2）
};

// 一次推理的输出。valid=true 表示推理执行成功（即使 detections 为空）。
// 帧标识复用 common/frame_meta.h 的 FrameId（含 camera_id 与 sequence）。
// camera_id / monotonic_ns 字段由 InferenceScheduler 统一覆写，保证与提交帧一致。
struct InferenceResult
{
    bool                   valid = false;
    FrameId                frame_id;
    std::uint32_t          camera_id    = 0;
    std::int64_t           monotonic_ns = 0;
    std::string            model_id;
    std::string            model_version;
    std::vector<Detection> detections;
};

}  // namespace animal_monitor::inference
