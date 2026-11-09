// ============================================================================
// 文件：src/thermal/thermal_frame.cpp
// 状态：implemented（纯数学，host 可编译；MSVC /W4 已验证见测试记录）
// 依据：include/animal_monitor/thermal/thermal_frame.h
// ============================================================================

#include "animal_monitor/thermal/thermal_frame.h"

#include <cmath>
#include <cstddef>

namespace animal_monitor {

namespace {

// 像素有效温度范围（°C）：器件目标物量程 -40..300，留 100 余量。
// 仅用于统计有效性判断，不是标定值（UNVERIFIED：以数据手册量程为准）。
constexpr float kMinPlausibleTempC = -40.0f;
constexpr float kMaxPlausibleTempC = 400.0f;

}  // namespace

bool IsValidPixelValue(float temp_c)
{
    return std::isfinite(temp_c) && temp_c >= kMinPlausibleTempC && temp_c <= kMaxPlausibleTempC;
}

float ValidPixelRatio(const ThermalFrame& frame)
{
    int valid = 0;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        if (IsValidPixelValue(frame.pixels[i]))
        {
            ++valid;
        }
    }
    return static_cast<float>(valid) / static_cast<float>(ThermalFrame::kPixelCount);
}

float MinPixel(const ThermalFrame& frame)
{
    float result = 0.0f;
    bool  found  = false;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        if (!IsValidPixelValue(frame.pixels[i]))
        {
            continue;
        }
        if (!found || frame.pixels[i] < result)
        {
            result = frame.pixels[i];
            found  = true;
        }
    }
    return result;
}

float MaxPixel(const ThermalFrame& frame)
{
    float result = 0.0f;
    bool  found  = false;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        if (!IsValidPixelValue(frame.pixels[i]))
        {
            continue;
        }
        if (!found || frame.pixels[i] > result)
        {
            result = frame.pixels[i];
            found  = true;
        }
    }
    return result;
}

float MeanPixel(const ThermalFrame& frame)
{
    double sum = 0.0;
    int    cnt = 0;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        if (IsValidPixelValue(frame.pixels[i]))
        {
            sum += static_cast<double>(frame.pixels[i]);
            ++cnt;
        }
    }
    return cnt == 0 ? 0.0f : static_cast<float>(sum / static_cast<double>(cnt));
}

int HotspotPixelIndex(const ThermalFrame& frame)
{
    int   index = -1;
    float best  = 0.0f;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        if (!IsValidPixelValue(frame.pixels[i]))
        {
            continue;
        }
        // 严格大于：并列最高温取最小索引，保证结果确定性。
        if (index < 0 || frame.pixels[i] > best)
        {
            index = i;
            best  = frame.pixels[i];
        }
    }
    return index;
}

}  // namespace animal_monitor
