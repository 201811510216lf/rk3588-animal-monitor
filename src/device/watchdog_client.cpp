// ============================================================================
// 文件：src/device/watchdog_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/device/watchdog_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（Watchdog 用 SoC watchdog + /dev/watchdog）。
// ============================================================================

#include "animal_monitor/device/watchdog_client.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
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
        return Status(StatusCode::kHardwareUnavailable, what + ": 看门狗设备不存在");
    }
    if (error == EBUSY)
    {
        return Status(StatusCode::kUnavailable, what + ": 看门狗已被其他进程占用");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

}  // namespace

WatchdogClient::WatchdogClient(std::string device_path, int timeout_seconds)
    : device_path_(std::move(device_path)), timeout_seconds_(timeout_seconds)
{}

WatchdogClient::~WatchdogClient() { Close(); }

Status WatchdogClient::Open()
{
    if (fd_ >= 0)
    {
        return Status::MakeOk();  // 幂等
    }
    if (device_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "看门狗节点路径为空");
    }
    if (timeout_seconds_ <= 0)
    {
        return Status(StatusCode::kInvalidArgument, "看门狗超时必须大于 0 秒");
    }

    const int fd = ::open(device_path_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开看门狗失败: " + device_path_);
    }

    // 身份查询：失败非致命（部分驱动不支持），仅影响诊断信息。
    struct watchdog_info info{};
    if (::ioctl(fd, WDIOC_GETSUPPORT, &info) < 0)
    {
        support_info_ = "(WDIOC_GETSUPPORT 失败)";
    }
    else
    {
        // info.identity 为 __u8[32]，显式转 const char*（无隐式转换）。
        const char* identity = reinterpret_cast<const char*>(info.identity);
        support_info_.assign(identity, ::strnlen(identity, sizeof(info.identity)));
    }

    // 设置超时：失败致命（无法保证喂狗节奏即无法安全运行）。
    int timeout = timeout_seconds_;
    if (::ioctl(fd, WDIOC_SETTIMEOUT, &timeout) < 0)
    {
        const int saved = errno;
        ::close(fd);
        return StatusFromErrno(saved, "WDIOC_SETTIMEOUT 失败");
    }
    actual_timeout_ = timeout;  // 驱动可能裁剪，以返回值为准

    fd_ = fd;
    return Status::MakeOk();
}

Status WatchdogClient::KeepAlive()
{
    if (fd_ < 0)
    {
        return Status(StatusCode::kUnavailable, "看门狗未打开");
    }

    // watchdog core 标准喂狗方式：写一个 '\0' 字节。
    const char magic = '\0';
    if (::write(fd_, &magic, 1) < 0)
    {
        return StatusFromErrno(errno, "喂狗失败");
    }
    return Status::MakeOk();
}

void WatchdogClient::Close()
{
    if (fd_ < 0)
    {
        return;
    }

    // 魔术字关闭（等价 WDIOC_SETOPTIONS + WDIOS_DISABLECARD）。
    // 警告：关闭后看门狗不再被喂，若系统 watchdog 服务未接管，超时后复位。
    const char magic = 'V';
    (void)::write(fd_, &magic, 1);
    ::close(fd_);
    fd_ = -1;
}

}  // namespace animal_monitor
