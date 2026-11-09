#pragma once

// animal_monitor/calibration/temperature_calibrator.h
// 状态：implemented（host 可编译，纯数学，无外部依赖）
// 依据：任务书 CODEX_MASTER_TASK.md §10（Environment compensation，物理模型需标定数据支撑）；
//       AGENTS.md（C++17、k 常量、无全局可变状态）；
//       审计结论：源项目 A/B 无温度标定逻辑，本模块为全新实现。
// 说明：
//   - 线性标定模型：校正值 = gain * raw + offset_c。
//   - NOT_CALIBRATED：标定系数必须来自参考测温设备与原始读数的实测数据对拟合，
//     禁止硬编码或宣称结果为“准确体温”。本模块只做最小二乘数学运算，不对系数来源
//     做任何物理假设。

#include <cstddef>
#include <utility>
#include <vector>

#include "animal_monitor/common/status.h"

namespace animal_monitor::calibration {

// 线性标定参数：校正值 = gain * raw_c + offset_c。
struct LinearCalibration
{
    float offset_c = 0.0f;  // 截距（°C）
    float gain     = 1.0f;  // 增益（无量纲）
};

// 应用标定：ref = gain * raw_c + offset_c。不做钳制（是否接受结果由调用方决策）。
float ApplyCalibration(float raw_c, const LinearCalibration& cal);

// 最小二乘拟合。reference_pairs 为 (参考温度, 原始读数)，参考温度来自参考测温设备实测。
// 约束：至少 2 对；原始读数不全相同（否则增益奇异）；任一数值非有限均拒绝。
// 失败返回 kInvalidArgument；out 必须非空。
Status FitCalibrationFromPairs(const std::vector<std::pair<float, float>>& reference_pairs,
                               LinearCalibration* out);

// 拟合质量指标。
struct CalibrationQuality
{
    float mae  = 0.0f;  // MAE  = mean(|ref - pred|)
    float rmse = 0.0f;  // RMSE = sqrt(mean((ref - pred)^2))
};

// 对给定数据对评估标定质量；空输入返回 {0, 0}。数据中含非有限值时结果未定义
// （调用方应保证输入为实测有效值）。
CalibrationQuality EvaluateCalibration(const std::vector<std::pair<float, float>>& reference_pairs,
                                       const LinearCalibration& cal);

}  // namespace animal_monitor::calibration
