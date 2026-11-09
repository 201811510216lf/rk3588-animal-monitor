// ============================================================================
// 文件：src/calibration/roi_mapper.cpp
// 状态：implemented（纯逻辑，host 可编译；MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md §10；include/animal_monitor/calibration/roi_mapper.h
// ============================================================================

#include "animal_monitor/calibration/roi_mapper.h"

#include <cmath>

namespace animal_monitor::calibration {

namespace {

// 工程合理性阈值（非标定值，UNVERIFIED）：防止明显错误的配置进入运行期。
constexpr float kMinMappingScale  = 1e-6f;  // scale 必须为有限正数
constexpr float kMaxMappingScale  = 100.0f; // 缩放上界（热图仅 32x24，正常相机映射远小于此）
constexpr float kMaxMappingOffset = 1e4f;   // 偏移绝对值上界

const char* const kConfigKeyScaleX  = "scale_x";
const char* const kConfigKeyScaleY  = "scale_y";
const char* const kConfigKeyOffsetX = "offset_x";
const char* const kConfigKeyOffsetY = "offset_y";

}  // namespace

Status ValidateMapping(const CameraThermalMapping& mapping, std::string* err)
{
    if (!std::isfinite(mapping.scale_x) || mapping.scale_x <= 0.0f)
    {
        if (err != nullptr)
        {
            *err = "scale_x 必须为有限正数";
        }
        return Status(StatusCode::kInvalidArgument, "invalid scale_x");
    }
    if (mapping.scale_x > kMaxMappingScale)
    {
        if (err != nullptr)
        {
            *err = "scale_x 超出工程合理性上限";
        }
        return Status(StatusCode::kInvalidArgument, "scale_x out of plausible range");
    }
    if (!std::isfinite(mapping.scale_y) || mapping.scale_y <= 0.0f)
    {
        if (err != nullptr)
        {
            *err = "scale_y 必须为有限正数";
        }
        return Status(StatusCode::kInvalidArgument, "invalid scale_y");
    }
    if (mapping.scale_y > kMaxMappingScale)
    {
        if (err != nullptr)
        {
            *err = "scale_y 超出工程合理性上限";
        }
        return Status(StatusCode::kInvalidArgument, "scale_y out of plausible range");
    }
    if (!std::isfinite(mapping.offset_x) || std::fabs(mapping.offset_x) > kMaxMappingOffset)
    {
        if (err != nullptr)
        {
            *err = "offset_x 非有限或超出工程合理性范围";
        }
        return Status(StatusCode::kInvalidArgument, "offset_x out of plausible range");
    }
    if (!std::isfinite(mapping.offset_y) || std::fabs(mapping.offset_y) > kMaxMappingOffset)
    {
        if (err != nullptr)
        {
            *err = "offset_y 非有限或超出工程合理性范围";
        }
        return Status(StatusCode::kInvalidArgument, "offset_y out of plausible range");
    }
    return Status::MakeOk();
}

Status MapPointToThermal(const CameraThermalMapping& mapping, float x, float y, float* tx,
                         float* ty)
{
    if (tx == nullptr || ty == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "tx/ty 输出指针为空");
    }
    if (!ValidateMapping(mapping, nullptr).Ok())
    {
        return Status(StatusCode::kInvalidArgument, "映射参数非法");
    }
    if (!std::isfinite(x) || !std::isfinite(y))
    {
        return Status(StatusCode::kInvalidArgument, "输入坐标为非有限值");
    }
    *tx = x * mapping.scale_x + mapping.offset_x;
    *ty = y * mapping.scale_y + mapping.offset_y;
    return Status::MakeOk();
}

Status MapBboxToThermal(const CameraThermalMapping& mapping, const float bbox[4], ThermalRoi* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "out 输出指针为空");
    }
    if (!ValidateMapping(mapping, nullptr).Ok())
    {
        return Status(StatusCode::kInvalidArgument, "映射参数非法");
    }
    const float x = bbox[0];
    const float y = bbox[1];
    const float w = bbox[2];
    const float h = bbox[3];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h))
    {
        return Status(StatusCode::kInvalidArgument, "bbox 含非有限值");
    }
    if (w < 0.0f || h < 0.0f)
    {
        return Status(StatusCode::kInvalidArgument, "bbox 宽高为负");
    }
    out->x0 = x * mapping.scale_x + mapping.offset_x;
    out->y0 = y * mapping.scale_y + mapping.offset_y;
    out->x1 = (x + w) * mapping.scale_x + mapping.offset_x;
    out->y1 = (y + h) * mapping.scale_y + mapping.offset_y;
    return Status::MakeOk();
}

Status LoadMappingFromConfig(const config::ConfigValue& node, CameraThermalMapping* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "out 输出指针为空");
    }
    if (!node.IsMap())
    {
        return Status(StatusCode::kConfigurationError, "标定映射配置必须是 map 节点");
    }
    const char* const keys[4] = {kConfigKeyScaleX, kConfigKeyScaleY, kConfigKeyOffsetX,
                                 kConfigKeyOffsetY};
    double            values[4] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i)
    {
        const config::ConfigValue* item = node.Find(keys[i]);
        if (item == nullptr)
        {
            return Status(StatusCode::kConfigurationError,
                          std::string("标定映射配置缺少键 ") + keys[i]);
        }
        double parsed = 0.0;
        if (!item->AsDouble(&parsed))
        {
            return Status(StatusCode::kConfigurationError,
                          std::string("标定映射配置键 ") + keys[i] + " 不是数值标量");
        }
        values[i] = parsed;
    }
    CameraThermalMapping mapping;
    mapping.scale_x  = static_cast<float>(values[0]);
    mapping.scale_y  = static_cast<float>(values[1]);
    mapping.offset_x = static_cast<float>(values[2]);
    mapping.offset_y = static_cast<float>(values[3]);

    std::string err;
    const Status valid = ValidateMapping(mapping, &err);
    if (!valid.Ok())
    {
        return Status(valid.Code(), err);
    }
    *out = mapping;
    return Status::MakeOk();
}

}  // namespace animal_monitor::calibration
