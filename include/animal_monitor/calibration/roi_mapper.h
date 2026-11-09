#pragma once

// animal_monitor/calibration/roi_mapper.h
// 状态：implemented（host 可编译，纯逻辑，无外部依赖）
// 依据：任务书 CODEX_MASTER_TASK.md §10（Camera-to-thermal calibration / Dynamic ROI）；
//       AGENTS.md（C++17、k 常量、Status 错误传播、无全局可变状态）；
//       审计结论：源项目 A/B 无相机-热像坐标映射实现，本模块为全新实现
//       （docs/migration/module_mapping.md 未收录该能力）。
// 说明：
//   - RGB 检测框 → 热图(32x24) ROI 的仿射映射：tx = x*scale_x + offset_x，y 同理。
//   - NOT_CALIBRATED：本模块是配置驱动的工程近似。真实投影必须由标定实验数据支撑
//     （视角、安装位置、镜头视场、距离等），禁止宣称按图像尺寸比例缩放即为正确映射
//     （任务书 §10 明确禁止仅按尺寸比例缩放 bbox）。当前系数来源一律视为未标定
//     （UNVERIFIED）。
//   - 输出坐标不做裁剪：超出热图边界的 ROI 保留原始值，由调用方决策（与
//     inference_types.h 对检测框坐标不裁剪的策略一致）。

#include <string>

#include "animal_monitor/common/status.h"
#include "animal_monitor/config/config_loader.h"

namespace animal_monitor::calibration {

// RGB(相机像素) → 热图(32x24) 仿射映射参数。
// 工程近似，未经标定实验验证（NOT_CALIBRATED / UNVERIFIED）。
struct CameraThermalMapping
{
    float scale_x  = 0.0f;  // 相机像素 x * scale_x + offset_x = 热图列坐标
    float scale_y  = 0.0f;  // 相机像素 y * scale_y + offset_y = 热图行坐标
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};

// 热图坐标系（32x24，单位：像素，允许小数；不裁剪到边界）上的 ROI。
struct ThermalRoi
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

// 映射参数合法性校验（纯参数检查，不依赖热图尺寸常数）：
//   - scale_x / scale_y 必须为有限正数（上限 kMaxMappingScale，拦截明显错误的配置）；
//   - offset_x / offset_y 必须有限且绝对值不超过 kMaxMappingOffset。
// 阈值为工程合理性判断，不是标定结论（UNVERIFIED）。
// 失败时若 err 非空则写入简体中文原因。
Status ValidateMapping(const CameraThermalMapping& mapping, std::string* err);

// 单点映射。tx / ty 必须非空；映射参数非法或输入非有限返回 kInvalidArgument。
Status MapPointToThermal(const CameraThermalMapping& mapping, float x, float y, float* tx,
                         float* ty);

// 检测框映射。bbox[4] = {x, y, w, h}（左上角 + 宽高，与 inference::Detection::Bbox 同序）。
// out 必须非空；输出 (x0,y0) 为左上角映射、(x1,y1) 为右下角(x+w, y+h)映射，不裁剪。
// w/h 为负或任一坐标为非有限返回 kInvalidArgument。
Status MapBboxToThermal(const CameraThermalMapping& mapping, const float bbox[4], ThermalRoi* out);

// 从配置 map 节点加载映射。键：scale_x / scale_y / offset_x / offset_y（数值标量，四项必填）。
// 节点非 map、缺键或值非数值返回 kConfigurationError；参数超范围返回 kInvalidArgument。
Status LoadMappingFromConfig(const config::ConfigValue& node, CameraThermalMapping* out);

}  // namespace animal_monitor::calibration
