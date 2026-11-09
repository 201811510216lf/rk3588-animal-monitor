#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/rtc_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译）
// 依据：任务书 CODEX_MASTER_TASK.md §8（RTC 用 RTC class + 内核驱动，如 DS3231）。
// 说明：device_path 默认 "/dev/rtc0" 为 Linux RTC class 标准节点；若板卡有多个
//       RTC 或节点名不同，由配置覆盖（板卡接线 UNVERIFIED）。
//       Unix 秒按 UTC 解释（不依赖本地时区/DST）；若 RTC 存本地时间需上层转换。
// 线程模型：客户端不内置线程；并发读取安全（ioctl 为单次系统调用）。
// ============================================================================

#include <cstdint>
#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor {

// RTC 时间分解（公元纪年，1 月起）。
struct RtcDateTime
{
    std::int32_t year   = 1970;
    std::int32_t month  = 1;  // 1..12
    std::int32_t day    = 1;  // 1..31
    std::int32_t hour   = 0;  // 0..23
    std::int32_t minute = 0;
    std::int32_t second = 0;
};

// RTC class 用户态客户端（/dev/rtcN + ioctl RTC_RD_TIME）。
class RtcClient
{
  public:
    // device_path：RTC cdev 节点，默认 Linux 标准节点 /dev/rtc0（可配置覆盖）。
    explicit RtcClient(std::string device_path = "/dev/rtc0");

    // RAII：析构时关闭节点。
    ~RtcClient();

    RtcClient(const RtcClient&) = delete;
    RtcClient& operator=(const RtcClient&) = delete;

    // 打开 RTC 节点（O_RDONLY | O_CLOEXEC）。
    Status Open();

    // ioctl RTC_RD_TIME 读取时间分解（rtc_time 的 tm_year+1900 → 公元年）。
    Status ReadDateTime(RtcDateTime* out);

    // 读取时间并按 UTC 计算 Unix 秒（days_from_civil 算法，不依赖时区）。
    Status ReadUnixSeconds(std::int64_t* out);

    // 幂等关闭（可重复调用）。
    void Close();

    bool IsOpen() const { return fd_ >= 0; }

  private:
    std::string device_path_;
    int         fd_ = -1;
};

}  // namespace animal_monitor
