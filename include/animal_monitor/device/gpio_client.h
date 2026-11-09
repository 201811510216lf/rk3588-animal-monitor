#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/gpio_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译，
//       依赖 libgpiod v1（<gpiod.h>，链接 -lgpiod））
// 依据：任务书 CODEX_MASTER_TASK.md §8（GPIO 用 gpio character device + libgpiod）。
// 线程模型：客户端不内置线程；同一实例的并发使用由上层同步。
// 说明：chip_path（如 "/dev/gpiochip0"）与 line_offset（如 12）全部由配置传入，
//       不伪造板卡 GPIO 编号（UNVERIFIED）。本客户端不配置去抖/上下拉：
//       去抖与电气配置由内核（设备树）或上层策略决定。
// ============================================================================

#include <string>

#include "animal_monitor/common/status.h"

// libgpiod v1 的前置声明，避免头文件依赖 <gpiod.h>（实现文件包含）。
struct gpiod_chip;
struct gpiod_line;

namespace animal_monitor
{

enum class GpioDirection
{
    kInput,
    kOutput,
};

// libgpiod v1 用户态 GPIO 客户端（gpio character device）。
class GpioClient
{
   public:
    // chip_path：gpiochip 设备节点（如 "/dev/gpiochip0"）；line_offset：行号。
    GpioClient(std::string chip_path, unsigned int line_offset);

    // RAII：析构时释放行与芯片。
    ~GpioClient();

    GpioClient(const GpioClient&)            = delete;
    GpioClient& operator=(const GpioClient&) = delete;

    // 打开芯片并请求行。输入行用 gpiod_line_request_input；
    // 输出行用 gpiod_line_request_output（初始电平 0）。consumer 名固定
    // "animal_monitor"（供内核 diagnostics 显示）。
    Status Open(GpioDirection direction);

    // 读取电平：1/0；失败返回 -1 并保留 errno 语义。
    int Read();

    // 写电平：value 必须为 0 或 1，否则 kInvalidArgument。
    Status Write(int value);

    // 等待行事件（IRQ 用途）：gpiod_line_event_wait，等待至多 timeout_ms
    // 毫秒（负数无限）。返回 Ok 表示有事件（电平读取由上层按策略处理），
    // 超时返回 kTimeout。注：本实现不读事件内容、不配置去抖——
    // 去抖与边沿策略由配置/上层处理（事件内容读取可扩展 gpiod_line_event_read）。
    Status WaitForEvent(int timeout_ms);

    // 幂等关闭（释放行 + 关闭芯片，可重复调用）。
    void Close();

    bool IsOpen() const { return line_ != nullptr; }

   private:
    std::string  chip_path_;
    unsigned int line_offset_;
    gpiod_chip*  chip_ = nullptr;
    gpiod_line*  line_ = nullptr;
};

}  // namespace animal_monitor
