#pragma once

// ============================================================================
// 文件：include/animal_monitor/communication/modbus_rtu.h
// 状态：implemented（host 可编译纯逻辑；已由 tests/unit/test_modbus_rtu.cpp 覆盖，
//       MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md §8/§9（Modbus RTU over RS485）；
//       复用 common/modbus_crc.h（CRC 已实现并已测）。
// 说明：本文件只做帧编解码（纯逻辑、无 IO）；串口收发由目标构建的
//       Rs485ModbusClient 完成。CRC 字节序为低字节在前（Modbus 规范）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "animal_monitor/common/status.h"

namespace animal_monitor {

// Modbus RTU 应用层帧视图：data 为 PDU 数据区（不含 CRC）。
// slave == 0 表示广播地址，仅允许出现在请求方向；响应帧的从站地址不得为 0。
struct ModbusFrame
{
    std::uint8_t           slave    = 0;
    std::uint8_t           function = 0;
    std::vector<std::uint8_t> data;
};

// 构建"读保持寄存器"请求（功能码 0x03），自动追加 CRC（低字节在前）。
// count 合法范围 [1, 125]（Modbus 规范单次读取上限），越界返回 kInvalidArgument。
Status BuildReadHoldingRegisters(std::uint8_t slave, std::uint16_t addr, std::uint16_t count,
                                 std::vector<std::uint8_t>* frame);

// 构建"写单寄存器"请求（功能码 0x06），自动追加 CRC（低字节在前）。
Status BuildWriteSingleRegister(std::uint8_t slave, std::uint16_t addr, std::uint16_t value,
                                std::vector<std::uint8_t>* frame);

// 解析线上帧（含 CRC），成功时 out->data 不含 CRC。
// 校验项：最小长度 4（从站+功能码+CRC）、CRC 正确、从站与功能码一致性：
//   - 从站地址 0（广播）不合法（响应帧不可能来自广播地址）；
//   - 功能码低 7 位不得为 0；
//   - 异常帧（function & 0x80）的 data 必须恰好 1 字节（异常码）。
// 失败时若 err 非空则写入可读原因；err 允许传 nullptr。
Status ParseFrame(const std::vector<std::uint8_t>& wire, ModbusFrame* out, std::string* err);

// 解析"读保持寄存器"响应（function == 0x03）：data[0] 为字节数，
// 必须等于 2×寄存器数（即必须为偶数且与 data 剩余长度一致）。
// 寄存器为 Modbus 规范的大端序（高字节在前）。
Status ParseReadHoldingResponse(const ModbusFrame& frame, std::vector<std::uint16_t>* registers,
                                std::string* err);

// 是否为异常响应（function 最高位为 1）；是则把异常码（data[0]，缺省 0）
// 写入 *exception_code（可为 nullptr）。
bool IsException(const ModbusFrame& frame, std::uint8_t* exception_code);

}  // namespace animal_monitor
