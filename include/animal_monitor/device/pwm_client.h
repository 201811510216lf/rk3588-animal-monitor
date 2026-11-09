#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/pwm_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译）
// 依据：任务书 CODEX_MASTER_TASK.md §8（PWM 用 pwm framework；扇区/蜂鸣器用途）。
// 说明：sysfs PWM（/sys/class/pwm/pwmchipN）为过渡方案——新内核推荐 PWM
//       chardev（CONFIG_PWM_SYSFS 在新内核可能默认关闭），板卡到位后视内核
//       版本评估迁移。chip_path/channel 全部由配置传入（UNVERIFIED）。
// 线程模型：客户端不内置线程；同一实例的并发使用由上层同步。
// 约束：必须先 SetPeriodNs 再 SetDutyNs（内核要求 duty_cycle ≤ period）；
//       若当前 duty 大于新 period，写 period 会被内核拒绝（EINVAL），
//       调用方需先降 duty。
// ============================================================================

#include <cstdint>
#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor
{

// sysfs PWM 客户端：写 export 导出通道，操作 period/duty_cycle/enable 文件。
class PwmClient
{
   public:
    // chip_path：PWM 芯片 sysfs 目录（如 "/sys/class/pwm/pwmchip0"）；channel：通道号。
    PwmClient(std::string chip_path, unsigned int channel);

    // RAII：析构时关闭（disable + unexport）。
    ~PwmClient();

    PwmClient(const PwmClient&)            = delete;
    PwmClient& operator=(const PwmClient&) = delete;

    // 写 chip_path/export 导出通道。通道已被其他进程导出（EBUSY）视为成功，
    // 但此时本实例不拥有导出权，Close 不会 unexport。
    Status Open();

    // 写周期（纳秒）。period_ns == 0 返回 kInvalidArgument。
    Status SetPeriodNs(std::uint64_t period_ns);

    // 写占空比（纳秒）。内核约束 duty_cycle ≤ period（见头文件说明）。
    Status SetDutyNs(std::uint64_t duty_ns);

    // 写 enable："1" 使能输出，"0" 停止（输出保持配置，电平由极性决定）。
    Status Enable(bool enable);

    // 幂等关闭：本实例导出过通道时先 disable 再写 unexport（尽力而为）。
    void Close();

    bool IsOpen() const { return exported_; }

   private:
    // 向 sysfs 文件写文本内容；EINVAL 映射为 kInvalidArgument（如 duty>period）。
    Status WriteSysfsFile(const std::string& path, const std::string& text);

    std::string  chip_path_;
    unsigned int channel_;
    std::string  pwm_path_;  // Open 成功后确定：chip_path + "/pwm<channel>"
    bool         exported_ = false;
    bool         owned_    = false;  // 本实例是否执行了 export
};

}  // namespace animal_monitor
