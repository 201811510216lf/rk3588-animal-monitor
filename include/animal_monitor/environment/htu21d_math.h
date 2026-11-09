#pragma once

// ============================================================================
// 文件：include/animal_monitor/environment/htu21d_math.h
// 状态：implemented（纯数学，host 可编译；MSVC /W4 已验证）
// 依据：任务书 CODEX_MASTER_TASK.md §7.2；HTU21D（Sensirion/兼容 Si7021）数据手册公开公式
// 说明：
//   - CRC-8：多项式 0x131（x^8 + x^5 + x^4 + 1），初值 0x00，MSB 先行，
//     覆盖两个数据字节（MSB、LSB），与器件 3 字节读序列的校验字节比对。
//   - 换算公式（数据手册 §公式）：
//       RH(%) = -6 + 125 × Srh / 2^16
//       T(°C) = -46.85 + 175.72 × St / 2^16
//   - 输出为器件数据手册公式的直接换算，不代表已校准的“准确温湿度”
//     （未经标准湿度源/参考温度计比对，UNVERIFIED）。
// ============================================================================

#include <cstdint>

namespace animal_monitor
{

// 计算两字节数据的 CRC-8（多项式 0x131、初值 0、MSB 先）。
std::uint8_t Htu21dCrc8(std::uint8_t value_msb, std::uint8_t value_lsb);

// 校验 CRC 字节是否与数据匹配。value 为 (msb << 8) | lsb 合并值。
bool Htu21dCrcOk(std::uint16_t value, std::uint8_t crc_byte);

// 湿度原始值 → 相对湿度（%RH），范围约 [-6, +125)。
float Htu21dRhPercent(std::uint16_t srh);

// 温度原始值 → 温度（°C），范围约 [-46.85, +128.87)。
float Htu21dTempC(std::uint16_t st);

}  // namespace animal_monitor
