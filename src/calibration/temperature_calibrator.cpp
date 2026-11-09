// ============================================================================
// 文件：src/calibration/temperature_calibrator.cpp
// 状态：implemented（纯数学，host 可编译；MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md
// §10；include/animal_monitor/calibration/temperature_calibrator.h 说明：最小二乘使用 double
// 累加以减小截断误差；拟合结果不代表真实标定（NOT_CALIBRATED）。
// ============================================================================

#include "animal_monitor/calibration/temperature_calibrator.h"

#include <cmath>

namespace animal_monitor::calibration
{

float ApplyCalibration(float raw_c, const LinearCalibration& cal)
{
    return cal.gain * raw_c + cal.offset_c;
}

Status FitCalibrationFromPairs(const std::vector<std::pair<float, float>>& reference_pairs,
                               LinearCalibration*                          out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "out 输出指针为空");
    }
    if (reference_pairs.size() < 2)
    {
        return Status(StatusCode::kInvalidArgument, "标定拟合至少需要 2 对数据");
    }

    // 模型 ref = gain * raw + offset；令 x = raw（原始读数）、y = ref（参考温度）。
    double            sum_x  = 0.0;
    double            sum_y  = 0.0;
    double            sum_xy = 0.0;
    double            sum_xx = 0.0;
    const std::size_t n      = reference_pairs.size();
    for (const auto& pair : reference_pairs)
    {
        const float ref = pair.first;
        const float raw = pair.second;
        if (!std::isfinite(ref) || !std::isfinite(raw))
        {
            return Status(StatusCode::kInvalidArgument, "标定数据对含非有限值");
        }
        const double x = static_cast<double>(raw);
        const double y = static_cast<double>(ref);
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    const double denom = static_cast<double>(n) * sum_xx - sum_x * sum_x;
    if (denom == 0.0)
    {
        return Status(StatusCode::kInvalidArgument, "原始读数全相同，增益奇异，无法拟合");
    }

    LinearCalibration result;
    result.gain     = static_cast<float>((static_cast<double>(n) * sum_xy - sum_x * sum_y) / denom);
    result.offset_c = static_cast<float>((sum_y - static_cast<double>(result.gain) * sum_x) /
                                         static_cast<double>(n));
    *out            = result;
    return Status::MakeOk();
}

CalibrationQuality EvaluateCalibration(const std::vector<std::pair<float, float>>& reference_pairs,
                                       const LinearCalibration&                    cal)
{
    CalibrationQuality quality;
    const std::size_t  n = reference_pairs.size();
    if (n == 0)
    {
        return quality;  // {0, 0}
    }
    double abs_sum = 0.0;
    double sq_sum  = 0.0;
    for (const auto& pair : reference_pairs)
    {
        const float  ref  = pair.first;
        const float  raw  = pair.second;
        const double pred = static_cast<double>(ApplyCalibration(raw, cal));
        const double err  = static_cast<double>(ref) - pred;
        abs_sum += std::fabs(err);
        sq_sum += err * err;
    }
    quality.mae  = static_cast<float>(abs_sum / static_cast<double>(n));
    quality.rmse = static_cast<float>(std::sqrt(sq_sum / static_cast<double>(n)));
    return quality;
}

}  // namespace animal_monitor::calibration
