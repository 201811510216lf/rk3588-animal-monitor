// ============================================================================
// 文件：src/device/gpio_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译，链接 libgpiod v1）
// 依据：include/animal_monitor/device/gpio_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（GPIO 用 libgpiod，IRQ 用于 poll/事件）。
// ============================================================================

#include "animal_monitor/device/gpio_client.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include <gpiod.h>

namespace animal_monitor {

namespace {

constexpr char kConsumerName[] = "animal_monitor";

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
        return Status(StatusCode::kHardwareUnavailable, what + ": 芯片不存在");
    }
    if (error == EBUSY)
    {
        return Status(StatusCode::kUnavailable, what + ": 行已被占用");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

}  // namespace

GpioClient::GpioClient(std::string chip_path, unsigned int line_offset)
    : chip_path_(std::move(chip_path)), line_offset_(line_offset)
{
}

GpioClient::~GpioClient()
{
    Close();
}

Status GpioClient::Open(GpioDirection direction)
{
    if (line_ != nullptr)
    {
        return Status::MakeOk();  // 幂等
    }
    if (chip_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "gpiochip 路径为空");
    }

    chip_ = ::gpiod_chip_open(chip_path_.c_str());
    if (chip_ == nullptr)
    {
        return StatusFromErrno(errno, "打开 gpiochip " + chip_path_ + " 失败");
    }

    line_ = ::gpiod_chip_get_line(chip_, line_offset_);
    if (line_ == nullptr)
    {
        const int saved = errno;
        ::gpiod_chip_close(chip_);
        chip_ = nullptr;
        return StatusFromErrno(saved, "获取行失败");
    }

    int request_result = -1;
    switch (direction)
    {
        case GpioDirection::kInput:
            request_result = ::gpiod_line_request_input(line_, kConsumerName);
            break;
        case GpioDirection::kOutput:
            // 初始电平固定 0；初始值策略由上层配置决定（UNVERIFIED）。
            request_result = ::gpiod_line_request_output(line_, kConsumerName, 0);
            break;
    }
    if (request_result < 0)
    {
        const int saved = errno;
        ::gpiod_line_release(line_);
        ::gpiod_chip_close(chip_);
        line_ = nullptr;
        chip_ = nullptr;
        return StatusFromErrno(saved, "请求 GPIO 行失败");
    }

    return Status::MakeOk();
}

int GpioClient::Read()
{
    if (line_ == nullptr)
    {
        errno = EBADF;
        return -1;
    }
    return ::gpiod_line_get_value(line_);
}

Status GpioClient::Write(int value)
{
    if (line_ == nullptr)
    {
        return Status(StatusCode::kUnavailable, "GPIO 行未打开");
    }
    if (value != 0 && value != 1)
    {
        return Status(StatusCode::kInvalidArgument, "GPIO 电平必须为 0 或 1");
    }
    if (::gpiod_line_set_value(line_, value) < 0)
    {
        return StatusFromErrno(errno, "写 GPIO 电平失败");
    }
    return Status::MakeOk();
}

Status GpioClient::WaitForEvent(int timeout_ms)
{
    if (line_ == nullptr)
    {
        return Status(StatusCode::kUnavailable, "GPIO 行未打开");
    }

    struct timespec timeout
    {
    };
    if (timeout_ms >= 0)
    {
        timeout.tv_sec  = timeout_ms / 1000;
        timeout.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);
    }
    else
    {
        timeout.tv_sec  = 1;  // 无限等待的占位值（见下方循环）
        timeout.tv_nsec = 0;
    }

    // gpiod_line_event_wait：1=有事件，0=超时，-1=错误。
    for (;;)
    {
        const int result = ::gpiod_line_event_wait(line_, &timeout);
        if (result > 0)
        {
            return Status::MakeOk();
        }
        if (result == 0)
        {
            if (timeout_ms < 0)
            {
                continue;  // 无限等待：占位超时后继续
            }
            return Status(StatusCode::kTimeout, "等待 GPIO 事件超时");
        }
        if (errno == EINTR)
        {
            if (timeout_ms < 0)
            {
                continue;  // 无限等待时信号中断继续等待
            }
            return Status(StatusCode::kCancelled, "等待 GPIO 事件被信号中断");
        }
        return StatusFromErrno(errno, "等待 GPIO 事件失败");
    }
}

void GpioClient::Close()
{
    if (line_ != nullptr)
    {
        ::gpiod_line_release(line_);
        line_ = nullptr;
    }
    if (chip_ != nullptr)
    {
        ::gpiod_chip_close(chip_);
        chip_ = nullptr;
    }
}

}  // namespace animal_monitor
