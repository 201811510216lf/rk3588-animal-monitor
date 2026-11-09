#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace animal_monitor
{

// Modbus RTU CRC-16（多项式 0xA001，初始值 0xFFFF）。
// 帧内字节序：CRC 低字节在前（little-endian），与 Modbus 规范一致。
std::uint16_t ModbusCrc16(const std::uint8_t* data, std::size_t size);

std::uint16_t ModbusCrc16(const std::vector<std::uint8_t>& data);

// 在帧尾追加 CRC（低字节在前）。
void AppendModbusCrc(std::vector<std::uint8_t>& frame);

// 校验帧尾两字节 CRC 是否与前面内容匹配；帧长不足 2 返回 false。
bool VerifyModbusCrc(const std::vector<std::uint8_t>& frame);

}  // namespace animal_monitor
