#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/watchdog_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译）
// 依据：任务书 CODEX_MASTER_TASK.md §8（Watchdog 用 SoC watchdog + /dev/watchdog）。
// 说明：device_path 默认 "/dev/watchdog" 为 Linux watchdog core 标准节点（可配置）；
//       timeout_seconds 为软件策略默认值（10 秒），实际生效值以 WDIOC_SETTIMEOUT
//       返回为准（驱动可能裁剪）。板卡 watchdog 硬件与复位行为 UNVERIFIED。
// 警告：打开硬件看门狗后必须周期性 KeepAlive；Close 前必须先有守护进程
//       接管（写 'V' 魔术字仅为正常关闭流程），否则系统将被复位。
// 线程模型：KeepAlive 可能被定时线程调用；实例生命周期（Open/Close）由
//           守护进程主线程管理，与 KeepAlive 并发时需上层保证顺序。
// ============================================================================

#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor {

// /dev/watchdog 用户态客户端（写 '\0' 喂狗，写 'V' 魔术字关闭）。
class WatchdogClient
{
  public:
    // device_path：看门狗节点（默认 /dev/watchdog）；timeout_seconds：请求超时。
    explicit WatchdogClient(std::string device_path = "/dev/watchdog", int timeout_seconds = 10);

    // RAII：析构时执行魔术字关闭（前提是有守护接管，见头文件警告）。
    ~WatchdogClient();

    WatchdogClient(const WatchdogClient&) = delete;
    WatchdogClient& operator=(const WatchdogClient&) = delete;

    // 打开节点（O_WRONLY | O_CLOEXEC），ioctl WDIOC_GETSUPPORT 记录身份
    //（失败非致命），WDIOC_SETTIMEOUT 设置超时（失败致命，返回错误并关闭）。
    Status Open();

    // 喂狗：写一个 '\0' 字节（watchdog core 标准语义）。
    Status KeepAlive();

    // 关闭：先写 'V' 魔术字（WDIOC_SETOPTIONS WDIOS_DISABLECARD 等价操作）
    // 再关闭节点。幂等；重复调用安全。
    void Close();

    bool IsOpen() const { return fd_ >= 0; }

    // WDIOC_SETTIMEOUT 之后驱动实际生效的超时（秒，Open 后有效）。
    int                ActualTimeoutSeconds() const { return actual_timeout_; }
    const std::string& SupportInfo() const { return support_info_; }

  private:
    std::string device_path_;
    int         timeout_seconds_;
    int         fd_              = -1;
    int         actual_timeout_  = 0;
    std::string support_info_;  // WDIOC_GETSUPPORT 记录的 identity（诊断用）
};

}  // namespace animal_monitor
