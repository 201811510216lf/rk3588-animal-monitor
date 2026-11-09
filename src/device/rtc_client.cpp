// ============================================================================
// 文件：src/device/rtc_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/device/rtc_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（RTC 用 RTC class，如 DS3231 优先内核驱动）。
// 说明：Unix 秒换算采用 days_from_civil（Howard Hinnant 公开算法），
//       按 UTC 解释 RTC，不引入 timegm/时区依赖。
// ============================================================================

#include "animal_monitor/device/rtc_client.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <fcntl.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace animal_monitor
{

namespace
{

constexpr std::int64_t kSecondsPerDay = 86400;

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
        return Status(StatusCode::kHardwareUnavailable, what + ": RTC 设备不存在");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

// 公历日期 → 自 1970-01-01 的天数（Howard Hinnant 的 days_from_civil 算法）。
// 输入需为合法日期（内核 rtc_time 保证），不做深度校验。
std::int64_t DaysFromCivil(std::int64_t year, unsigned int month, unsigned int day)
{
    year -= month <= 2 ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned int yoe = static_cast<unsigned int>(year - era * 400);  // [0, 399]
    // 注意 month 偏移项用有符号运算（12 月对应 -3，1/2 月对应 +9）。
    const int          month_offset = month > 2 ? -3 : 9;
    const unsigned int doy =
        (153u * (static_cast<int>(month) + month_offset) + 2) / 5 + day - 1;  // [0, 365]
    const unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

}  // namespace

RtcClient::RtcClient(std::string device_path) : device_path_(std::move(device_path)) {}

RtcClient::~RtcClient() { Close(); }

Status RtcClient::Open()
{
    if (fd_ >= 0)
    {
        return Status::MakeOk();  // 幂等
    }
    if (device_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "RTC 节点路径为空");
    }

    const int fd = ::open(device_path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开 RTC 节点失败: " + device_path_);
    }

    fd_ = fd;
    return Status::MakeOk();
}

Status RtcClient::ReadDateTime(RtcDateTime* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "out 指针为空");
    }
    if (fd_ < 0)
    {
        return Status(StatusCode::kUnavailable, "RTC 未打开");
    }

    struct rtc_time time{};
    if (::ioctl(fd_, RTC_RD_TIME, &time) < 0)
    {
        return StatusFromErrno(errno, "ioctl RTC_RD_TIME 失败");
    }

    out->year   = time.tm_year + 1900;
    out->month  = time.tm_mon + 1;
    out->day    = time.tm_mday;
    out->hour   = time.tm_hour;
    out->minute = time.tm_min;
    out->second = time.tm_sec;
    return Status::MakeOk();
}

Status RtcClient::ReadUnixSeconds(std::int64_t* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "out 指针为空");
    }

    RtcDateTime date_time;
    Status      status = ReadDateTime(&date_time);
    if (!status.Ok())
    {
        return status;
    }

    // 按 UTC 解释（不处理本地时区/DST）。
    const std::int64_t days =
        DaysFromCivil(date_time.year, static_cast<unsigned int>(date_time.month),
                      static_cast<unsigned int>(date_time.day));
    *out = days * kSecondsPerDay + static_cast<std::int64_t>(date_time.hour) * 3600 +
           static_cast<std::int64_t>(date_time.minute) * 60 + date_time.second;
    return Status::MakeOk();
}

void RtcClient::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace animal_monitor
