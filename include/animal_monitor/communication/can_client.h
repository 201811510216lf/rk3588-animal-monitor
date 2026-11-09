#pragma once

// ============================================================================
// 文件：include/animal_monitor/communication/can_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译，依赖 Linux 头）
// 依据：任务书 CODEX_MASTER_TASK.md §8/§9（CAN 用 SocketCAN + 内核 mcp251x 驱动）。
// 线程模型：客户端不内置线程；Open/Close 与 Send/Receive 的并发使用由上层同步，
//          同一 socket 上的并发 Receive 需外部串行化。
// 说明：仅支持经典 CAN（CAN_RAW），不支持 CAN FD；接口名由板卡配置传入，
//       （如 "can0"，与 mcp251x 设备树绑定对应，UNVERIFIED）。
// ============================================================================

#include <string>

#include "animal_monitor/common/status.h"
#include "animal_monitor/communication/can_frame.h"

namespace animal_monitor {

// SocketCAN 客户端：AF_CAN + SOCK_RAW + CAN_RAW，RAII 生命周期。
class CanClient
{
  public:
    // interface_name：SocketCAN 网络接口名（如 "can0"），由配置传入。
    explicit CanClient(std::string interface_name);

    // RAII：析构时关闭 socket。
    ~CanClient();

    CanClient(const CanClient&) = delete;
    CanClient& operator=(const CanClient&) = delete;

    // socket(AF_CAN, SOCK_RAW, CAN_RAW) + bind 到接口（if_nametoa 语义：按名解析索引）。
    Status Open();

    // 发送一个经典 CAN 帧（can_id 按 extended 标志附加 CAN_EFF_FLAG，
    // rtr 附加 CAN_RTR_FLAG；dlc 非法返回 kInvalidArgument）。
    Status Send(const CanFrame& frame);

    // poll 阻塞接收一帧（timeout_ms 毫秒，负数表示无限等待；
    // 超时返回 kTimeout，被信号中断返回 kCancelled）。
    Status Receive(CanFrame* frame, int timeout_ms);

    // 幂等关闭（可重复调用）。
    void Close();

    bool               IsOpen() const { return fd_ >= 0; }
    const std::string& InterfaceName() const { return interface_name_; }

  private:
    std::string interface_name_;
    int         fd_ = -1;
};

}  // namespace animal_monitor
