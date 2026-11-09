#pragma once

// ============================================================================
// 文件：include/animal_monitor/communication/can_frame.h
// 状态：implemented（host 可编译 header-only；已由 tests/unit/test_can_frame.cpp 覆盖）
// 依据：任务书 CODEX_MASTER_TASK.md §8/§9（SocketCAN / CAN RAW）。
// 说明：can_id 只存裸 ID（标准帧 11 位 / 扩展帧 29 位），不含 EFF/RTR/ERR 标志位；
//       与 Linux struct can_frame 的标志位转换由目标构建的 CanClient 完成。
//       rtr 帧的 data 字节在线上不传输，ToString 对 rtr 帧不输出数据区。
// ============================================================================

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace animal_monitor {

// CAN 经典帧最大 DLC（数据长度码）：0..8。
inline constexpr std::uint8_t kMaxCanDlc = 8;

// CAN 经典帧（经典 CAN，非 CAN FD）。
struct CanFrame
{
    std::uint32_t can_id    = 0;      // 裸 ID：标准帧 11 位，扩展帧 29 位
    bool          extended  = false;  // true 表示扩展帧（29 位 ID）
    bool          rtr       = false;  // 远程传输请求帧
    std::uint8_t  dlc       = 0;      // 数据长度 0..8
    std::uint8_t  data[8]   = {};     // 数据区（rtr 帧不传输 data）
};

// 经典 CAN 的 DLC 合法值：0..8。越界（如 CAN FD 长度或损坏帧）返回 false。
inline bool ValidateDlc(std::uint8_t dlc)
{
    return dlc <= kMaxCanDlc;
}

// 调试用字符串："<小写十六进制 id>#<每字节两位小写十六进制>"；
// rtr 帧输出 "<id>#R"（不输出数据区）。
// 例：{0x123, false, false, 2, {0x0A, 0x01}} → "123#0a01"。
inline std::string ToString(const CanFrame& frame)
{
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << frame.can_id << '#';
    if (frame.rtr)
    {
        oss << 'R';
    }
    else
    {
        // 防御性截断：未校验的非法 dlc 不得越界读 data。
        const std::uint8_t bytes = frame.dlc <= kMaxCanDlc ? frame.dlc : kMaxCanDlc;
        for (std::uint8_t i = 0; i < bytes; ++i)
        {
            oss << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(frame.data[i]);
        }
    }
    return oss.str();
}

}  // namespace animal_monitor
