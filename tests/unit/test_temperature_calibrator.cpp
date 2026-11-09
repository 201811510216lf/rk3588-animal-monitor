#include "test_harness.h"

// ============================================================================
// 文件：tests/unit/test_temperature_calibrator.cpp
// 依据：任务书 CODEX_MASTER_TASK.md §10（Environment compensation）；
//       include/animal_monitor/calibration/temperature_calibrator.h
// 覆盖：精确线性数据拟合、两对数据精确解、MAE/RMSE 已知值、退化输入拒绝。
// 注意：测试数据为合成数学数据，不代表任何真实标定（NOT_CALIBRATED）。
// ============================================================================

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "animal_monitor/calibration/temperature_calibrator.h"
#include "animal_monitor/common/status.h"

using animal_monitor::StatusCode;
using animal_monitor::Status;
using animal_monitor::calibration::ApplyCalibration;
using animal_monitor::calibration::CalibrationQuality;
using animal_monitor::calibration::EvaluateCalibration;
using animal_monitor::calibration::FitCalibrationFromPairs;
using animal_monitor::calibration::LinearCalibration;

namespace {

bool Near(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f;
}

AM_TEST(fit_exact_linear_data)
{
    // 参考温度 = 1.5 * 原始读数 + 2（合成数据，非实测标定）
    const std::vector<std::pair<float, float>> pairs = {
        {2.0f, 0.0f}, {3.5f, 1.0f}, {5.0f, 2.0f}, {6.5f, 3.0f}};
    LinearCalibration cal;
    const Status      status = FitCalibrationFromPairs(pairs, &cal);
    AM_EXPECT(status.Ok());
    AM_EXPECT(Near(cal.gain, 1.5f));
    AM_EXPECT(Near(cal.offset_c, 2.0f));
}

AM_TEST(fit_two_pairs_exact)
{
    // 参考温度 = 1.0 * 原始读数 + 10
    const std::vector<std::pair<float, float>> pairs = {{10.0f, 0.0f}, {20.0f, 10.0f}};
    LinearCalibration cal;
    const Status      status = FitCalibrationFromPairs(pairs, &cal);
    AM_EXPECT(status.Ok());
    AM_EXPECT(Near(cal.gain, 1.0f));
    AM_EXPECT(Near(cal.offset_c, 10.0f));
}

AM_TEST(apply_calibration)
{
    const LinearCalibration cal{2.0f, 1.5f};  // offset_c=2, gain=1.5
    AM_EXPECT(Near(ApplyCalibration(10.0f, cal), 17.0f));
}

AM_TEST(quality_known_values)
{
    const std::vector<std::pair<float, float>> pairs = {
        {2.0f, 0.0f}, {3.5f, 1.0f}, {5.0f, 2.0f}, {6.5f, 3.0f}};

    // 正确标定：误差全零
    const LinearCalibration exact{2.0f, 1.5f};
    const CalibrationQuality perfect = EvaluateCalibration(pairs, exact);
    AM_EXPECT(Near(perfect.mae, 0.0f));
    AM_EXPECT(Near(perfect.rmse, 0.0f));

    // 错误标定（增益 1、截距 0）：误差 {2, 2.5, 3, 3.5}
    // MAE = 2.75；MSE = (4+6.25+9+12.25)/4 = 7.875；RMSE = sqrt(7.875) ≈ 2.806243
    const LinearCalibration wrong{0.0f, 1.0f};
    const CalibrationQuality bad = EvaluateCalibration(pairs, wrong);
    AM_EXPECT(Near(bad.mae, 2.75f));
    AM_EXPECT(Near(bad.rmse, 2.806243f));
}

AM_TEST(empty_quality_is_zero)
{
    const std::vector<std::pair<float, float>> empty;
    const LinearCalibration                    cal{2.0f, 1.5f};
    const CalibrationQuality                   quality = EvaluateCalibration(empty, cal);
    AM_EXPECT_EQ(quality.mae, 0.0f);
    AM_EXPECT_EQ(quality.rmse, 0.0f);
}

AM_TEST(fit_rejects_insufficient_pairs)
{
    LinearCalibration                        cal;
    const std::vector<std::pair<float, float>> empty;
    AM_EXPECT_EQ(FitCalibrationFromPairs(empty, &cal).Code(), StatusCode::kInvalidArgument);

    const std::vector<std::pair<float, float>> single = {{30.0f, 20.0f}};
    AM_EXPECT_EQ(FitCalibrationFromPairs(single, &cal).Code(), StatusCode::kInvalidArgument);
}

AM_TEST(fit_rejects_constant_raw)
{
    // 原始读数全相同 → 增益奇异
    const std::vector<std::pair<float, float>> pairs = {
        {1.0f, 5.0f}, {2.0f, 5.0f}, {3.0f, 5.0f}};
    LinearCalibration cal;
    AM_EXPECT_EQ(FitCalibrationFromPairs(pairs, &cal).Code(), StatusCode::kInvalidArgument);
}

AM_TEST(fit_rejects_nonfinite)
{
    const std::vector<std::pair<float, float>> pairs = {
        {1.0f, 5.0f}, {std::numeric_limits<float>::quiet_NaN(), 6.0f}};
    LinearCalibration cal;
    AM_EXPECT_EQ(FitCalibrationFromPairs(pairs, &cal).Code(), StatusCode::kInvalidArgument);
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
