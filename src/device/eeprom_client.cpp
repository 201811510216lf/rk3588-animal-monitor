// ============================================================================
// 文件：src/device/eeprom_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/device/eeprom_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（EEPROM 用 at24 + 24Cxx，优先内核驱动）。
// ============================================================================

#include "animal_monitor/device/eeprom_client.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
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
        return Status(StatusCode::kPermissionDenied, what + ": 权限不足（写入可能受只读标志限制）");
    }
    if (error == ENODEV || error == ENOENT)
    {
        return Status(StatusCode::kHardwareUnavailable, what + ": 设备不存在（检查 I2C/at24）");
    }
    if (error == EINVAL)
    {
        return Status(StatusCode::kInvalidArgument, what + ": 偏移/长度越界");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

// offset 转 off_t；超出有符号范围返回失败（真 EEPROM 容量远小于此）。
bool ToOffT(std::uint64_t offset, off_t* out)
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    {
        return false;
    }
    *out = static_cast<off_t>(offset);
    return true;
}

}  // namespace

EepromClient::EepromClient(std::string sysfs_path) : sysfs_path_(std::move(sysfs_path)) {}

Status EepromClient::Read(std::uint64_t offset, std::uint8_t* buf, std::size_t len)
{
    if (buf == nullptr && len != 0)
    {
        return Status(StatusCode::kInvalidArgument, "buf 指针为空");
    }
    if (sysfs_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "EEPROM sysfs 路径为空");
    }
    if (len == 0)
    {
        return Status::MakeOk();
    }

    off_t off = 0;
    if (!ToOffT(offset, &off))
    {
        return Status(StatusCode::kInvalidArgument, "offset 超出 off_t 范围");
    }

    const int fd = ::open(sysfs_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开 EEPROM 失败: " + sysfs_path_);
    }

    std::size_t done  = 0;
    int         error = 0;
    while (done < len)
    {
        const ssize_t received =
            ::pread(fd, buf + done, len - done, off + static_cast<off_t>(done));
        if (received < 0)
        {
            error = errno;
            break;
        }
        if (received == 0)
        {
            error = EINVAL;  // 短读：偏移/长度超出器件容量
            break;
        }
        done += static_cast<std::size_t>(received);
    }
    ::close(fd);

    if (error != 0)
    {
        return StatusFromErrno(error, "读取 EEPROM 失败");
    }
    return Status::MakeOk();
}

Status EepromClient::Write(std::uint64_t offset, const std::uint8_t* buf, std::size_t len)
{
    if (buf == nullptr && len != 0)
    {
        return Status(StatusCode::kInvalidArgument, "buf 指针为空");
    }
    if (sysfs_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "EEPROM sysfs 路径为空");
    }
    if (len == 0)
    {
        return Status::MakeOk();
    }

    off_t off = 0;
    if (!ToOffT(offset, &off))
    {
        return Status(StatusCode::kInvalidArgument, "offset 超出 off_t 范围");
    }

    const int fd = ::open(sysfs_path_.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开 EEPROM 失败: " + sysfs_path_);
    }

    std::size_t done  = 0;
    int         error = 0;
    while (done < len)
    {
        const ssize_t written =
            ::pwrite(fd, buf + done, len - done, off + static_cast<off_t>(done));
        if (written < 0)
        {
            error = errno;
            break;
        }
        if (written == 0)
        {
            error = EINVAL;  // 短写：偏移/长度超出器件容量
            break;
        }
        done += static_cast<std::size_t>(written);
    }
    ::close(fd);

    if (error != 0)
    {
        return StatusFromErrno(error, "写入 EEPROM 失败");
    }
    return Status::MakeOk();
}

}  // namespace animal_monitor
