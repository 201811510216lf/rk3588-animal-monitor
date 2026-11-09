// ============================================================================
// 文件：src/device/adc_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/device/adc_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（ADC 用 IIO 框架，优先内核驱动）。
// ============================================================================

#include "animal_monitor/device/adc_client.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace animal_monitor
{

namespace
{

constexpr char        kSysfsRoot[] = "/sys/bus/iio/devices/";
constexpr std::size_t kAttrCap     = 64;  // raw/scale 属性文本远小于此

Status StatusFromErrno(int error, const std::string& what)
{
    if (error == EINTR)
    {
        return Status(StatusCode::kCancelled, what + ": 被信号中断");
    }
    if (error == EACCES || error == EPERM)
    {
        return Status(StatusCode::kPermissionDenied, what + ": 权限不足");
    }
    if (error == ENODEV || error == ENOENT)
    {
        return Status(StatusCode::kHardwareUnavailable, what + ": 属性/设备不存在");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

}  // namespace

AdcClient::AdcClient(std::string iio_device_name) : device_name_(std::move(iio_device_name)) {}

Status AdcClient::Open()
{
    if (open_)
    {
        return Status::MakeOk();  // 幂等
    }
    if (device_name_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "IIO 设备名为空");
    }

    base_path_ = kSysfsRoot + device_name_;

    struct stat info{};
    if (::stat(base_path_.c_str(), &info) < 0)
    {
        return StatusFromErrno(errno, "IIO 设备目录不存在: " + base_path_);
    }
    if (!S_ISDIR(info.st_mode))
    {
        return Status(StatusCode::kHardwareUnavailable, "IIO 路径不是目录: " + base_path_);
    }

    open_ = true;
    return Status::MakeOk();
}

Status AdcClient::ReadAttr(const std::string& attr_name, std::string* out)
{
    if (!open_)
    {
        return Status(StatusCode::kUnavailable, "ADC 未打开");
    }
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "out 指针为空");
    }

    const std::string path = base_path_ + "/" + attr_name;
    const int         fd   = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开 IIO 属性失败: " + path);
    }

    out->clear();
    char buffer[64];
    for (;;)
    {
        const ssize_t received = ::read(fd, buffer, sizeof(buffer));
        if (received < 0)
        {
            const int saved = errno;
            ::close(fd);
            return StatusFromErrno(saved, "读取 IIO 属性失败: " + path);
        }
        if (received == 0)
        {
            break;
        }
        out->append(buffer, static_cast<std::size_t>(received));
        if (out->size() > kAttrCap)
        {
            ::close(fd);
            return Status(StatusCode::kUnavailable, "IIO 属性内容异常超长: " + path);
        }
    }
    ::close(fd);

    // 去除尾部空白（sysfs 属性以 '\n' 结尾）。
    while (!out->empty() && (out->back() == '\n' || out->back() == '\r' || out->back() == ' ' ||
                             out->back() == '\t'))
    {
        out->pop_back();
    }
    if (out->empty())
    {
        return Status(StatusCode::kUnavailable, "IIO 属性内容为空: " + path);
    }
    return Status::MakeOk();
}

Status AdcClient::ReadRaw(int channel, std::int32_t* raw)
{
    if (raw == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "raw 指针为空");
    }
    if (channel < 0)
    {
        return Status(StatusCode::kInvalidArgument, "通道号必须非负");
    }

    std::string text;
    Status      status = ReadAttr("in_voltage" + std::to_string(channel) + "_raw", &text);
    if (!status.Ok())
    {
        return status;
    }

    errno            = 0;
    char*      end   = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE)
    {
        return Status(StatusCode::kUnavailable, "解析 raw 属性失败: " + text);
    }
    *raw = static_cast<std::int32_t>(value);
    return Status::MakeOk();
}

Status AdcClient::ReadScaled(int channel, float* value)
{
    if (value == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "value 指针为空");
    }

    std::int32_t raw    = 0;
    Status       status = ReadRaw(channel, &raw);
    if (!status.Ok())
    {
        return status;
    }

    // 先尝试通道级 scale，回退共享 scale（内核按驱动决定二者之一）。
    std::string scale_text;
    status = ReadAttr("in_voltage" + std::to_string(channel) + "_scale", &scale_text);
    if (!status.Ok() && status.Code() == StatusCode::kHardwareUnavailable)
    {
        status = ReadAttr("in_voltage_scale", &scale_text);
    }
    if (!status.Ok())
    {
        return status;
    }

    errno             = 0;
    char*       end   = nullptr;
    const float scale = std::strtof(scale_text.c_str(), &end);
    if (end == scale_text.c_str() || *end != '\0' || errno == ERANGE)
    {
        return Status(StatusCode::kUnavailable, "解析 scale 属性失败: " + scale_text);
    }

    *value = static_cast<float>(raw) * scale;
    return Status::MakeOk();
}

void AdcClient::Close() { open_ = false; }

}  // namespace animal_monitor
