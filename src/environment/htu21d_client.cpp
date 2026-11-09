// ============================================================================
// 文件：src/environment/htu21d_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/environment/htu21d_client.h
// 说明：IIO sysfs 同步读取；sysfs 路径以实际内核导出为准（UNVERIFIED）。
// ============================================================================

#include "animal_monitor/environment/htu21d_client.h"

#include <cstdlib>
#include <fstream>
#include <utility>

#include "animal_monitor/environment/htu21d_math.h"

namespace animal_monitor
{

namespace
{

// 读单个 sysfs 属性文件并解析为 float（空/非法内容返回 kUnavailable）。
Status ReadFloatFile(const std::string& path, float* out)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return Status(StatusCode::kNotFound, "无法打开 sysfs 属性: " + path);
    }
    std::string text;
    file >> text;
    if (text.empty())
    {
        return Status(StatusCode::kUnavailable, "sysfs 属性为空: " + path);
    }
    try
    {
        *out = std::stof(text);
    }
    catch (const std::exception&)
    {
        return Status(StatusCode::kUnavailable, "sysfs 属性解析失败: " + path + " = " + text);
    }
    return Status::MakeOk();
}

// 读通道 raw 值（16 位无符号十进制）。
Status ReadRawFile(const std::string& path, std::uint16_t* raw)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return Status(StatusCode::kNotFound, "无法打开 sysfs 属性: " + path);
    }
    std::string text;
    file >> text;
    if (text.empty())
    {
        return Status(StatusCode::kUnavailable, "sysfs 属性为空: " + path);
    }
    char*      end   = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || value < 0 || value > 0xFFFF)
    {
        return Status(StatusCode::kUnavailable, "sysfs 属性解析失败: " + path + " = " + text);
    }
    *raw = static_cast<std::uint16_t>(value);
    return Status::MakeOk();
}

// 回退路径：in_*_input 在 PROCESSED 形态下为毫单位（毫摄氏度/毫 %RH）。
Status ConvertProcessedFallback(const std::string& base, const char* channel, float* value)
{
    float        input  = 0.0f;
    const Status status = ReadFloatFile(base + "/in_" + channel + "_input", &input);
    if (!status.Ok())
    {
        return status;
    }
    *value = input * 0.001f;
    return Status::MakeOk();
}

}  // namespace

Htu21dClient::Htu21dClient(std::string iio_device_name)
    : iio_device_name_(std::move(iio_device_name)),
      base_path_("/sys/bus/iio/devices/" + iio_device_name_)
{}

Status Htu21dClient::ReadSample(float* temp_c, float* rh_pct)
{
    if (temp_c == nullptr || rh_pct == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "ReadSample: 输出不能为空指针");
    }

    // 温度：优先 in_temp_raw → Htu21dTempC 公式（含 -46.85 常数项）。
    std::uint16_t st     = 0;
    Status        status = ReadRawFile(base_path_ + "/in_temp_raw", &st);
    if (status.Ok())
    {
        *temp_c = Htu21dTempC(st);
    }
    else
    {
        // 回退：PROCESSED 形态的 in_temp_input（毫摄氏度）。
        status = ConvertProcessedFallback(base_path_, "temp", temp_c);
    }
    if (!status.Ok())
    {
        return status;
    }

    // 湿度：优先 in_humidityrelative_raw → Htu21dRhPercent 公式（含 -6 常数项）。
    std::uint16_t srh = 0;
    status            = ReadRawFile(base_path_ + "/in_humidityrelative_raw", &srh);
    if (status.Ok())
    {
        *rh_pct = Htu21dRhPercent(srh);
    }
    else
    {
        status = ConvertProcessedFallback(base_path_, "humidityrelative", rh_pct);
    }
    return status;
}

}  // namespace animal_monitor
