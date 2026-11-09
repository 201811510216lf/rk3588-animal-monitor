// ============================================================================
// 文件：src/device/pwm_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/device/pwm_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（PWM 用 pwm framework，sysfs 为过渡方案）。
// ============================================================================

#include "animal_monitor/device/pwm_client.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace animal_monitor
{

namespace
{

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
        return Status(StatusCode::kHardwareUnavailable, what + ": 设备不存在（检查芯片/内核配置）");
    }
    if (error == EBUSY)
    {
        return Status(StatusCode::kUnavailable, what + ": 通道已导出");
    }
    if (error == EINVAL)
    {
        return Status(StatusCode::kInvalidArgument, what + ": 参数非法（如 duty>period）");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

}  // namespace

PwmClient::PwmClient(std::string chip_path, unsigned int channel)
    : chip_path_(std::move(chip_path)), channel_(channel)
{}

PwmClient::~PwmClient() { Close(); }

Status PwmClient::Open()
{
    if (exported_)
    {
        return Status::MakeOk();  // 幂等
    }
    if (chip_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "PWM chip 路径为空");
    }

    pwm_path_                     = chip_path_ + "/pwm" + std::to_string(channel_);
    const std::string export_path = chip_path_ + "/export";

    const int fd = ::open(export_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开 PWM export 文件失败: " + export_path);
    }

    const std::string channel_text = std::to_string(channel_);
    const ssize_t     written      = ::write(fd, channel_text.data(), channel_text.size());
    const int         saved        = errno;
    ::close(fd);
    if (written < 0)
    {
        if (saved == EBUSY)
        {
            // 通道已被其他进程导出：借用之，但不拥有 unexport 权限。
            exported_ = true;
            owned_    = false;
            return Status::MakeOk();
        }
        return StatusFromErrno(saved, "写 PWM export 失败");
    }
    if (static_cast<std::size_t>(written) != channel_text.size())
    {
        return Status(StatusCode::kUnavailable, "写 PWM export 短写");
    }

    exported_ = true;
    owned_    = true;
    return Status::MakeOk();
}

Status PwmClient::WriteSysfsFile(const std::string& path, const std::string& text)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开 sysfs 文件失败: " + path);
    }

    std::size_t written_total = 0;
    int         error         = 0;
    while (written_total < text.size())
    {
        const ssize_t written =
            ::write(fd, text.data() + written_total, text.size() - written_total);
        if (written < 0)
        {
            error = errno;
            break;
        }
        written_total += static_cast<std::size_t>(written);
    }
    ::close(fd);
    if (error != 0)
    {
        return StatusFromErrno(error, "写 sysfs 文件失败: " + path);
    }
    if (written_total != text.size())
    {
        return Status(StatusCode::kUnavailable, "写 sysfs 文件短写: " + path);
    }
    return Status::MakeOk();
}

Status PwmClient::SetPeriodNs(std::uint64_t period_ns)
{
    if (!exported_)
    {
        return Status(StatusCode::kUnavailable, "PWM 通道未导出");
    }
    if (period_ns == 0)
    {
        return Status(StatusCode::kInvalidArgument, "周期必须大于 0");
    }
    return WriteSysfsFile(pwm_path_ + "/period", std::to_string(period_ns));
}

Status PwmClient::SetDutyNs(std::uint64_t duty_ns)
{
    if (!exported_)
    {
        return Status(StatusCode::kUnavailable, "PWM 通道未导出");
    }
    return WriteSysfsFile(pwm_path_ + "/duty_cycle", std::to_string(duty_ns));
}

Status PwmClient::Enable(bool enable)
{
    if (!exported_)
    {
        return Status(StatusCode::kUnavailable, "PWM 通道未导出");
    }
    return WriteSysfsFile(pwm_path_ + "/enable", enable ? "1" : "0");
}

void PwmClient::Close()
{
    if (!exported_)
    {
        return;
    }

    if (owned_)
    {
        // 尽力而为：先停输出再 unexport（失败不阻塞关闭流程）。
        (void)WriteSysfsFile(pwm_path_ + "/enable", "0");
        const std::string unexport_path = chip_path_ + "/unexport";
        const std::string channel_text  = std::to_string(channel_);
        const int         fd            = ::open(unexport_path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            (void)::write(fd, channel_text.data(), channel_text.size());
            ::close(fd);
        }
    }

    exported_ = false;
    owned_    = false;
}

}  // namespace animal_monitor
