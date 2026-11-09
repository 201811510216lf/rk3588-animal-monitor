// ============================================================================
// 文件：src/environment/htu21d_math.cpp
// 状态：implemented（纯数学，host 可编译；MSVC /W4 已验证）
// 依据：include/animal_monitor/environment/htu21d_math.h；HTU21D 数据手册公开公式
// ============================================================================

#include "animal_monitor/environment/htu21d_math.h"

namespace animal_monitor
{

namespace
{

// CRC-8 多项式 0x131（MSB 先行时每次移位异或低 8 位 0x31）。
constexpr std::uint8_t kCrc8Poly    = 0x31;
constexpr int          kBitsPerByte = 8;

// 湿度换算：RH(%) = -6 + 125 × Srh / 2^16。
constexpr float kRhOffsetPercent = -6.0f;
constexpr float kRhSlope         = 125.0f;
constexpr float kRhDivisor       = 65536.0f;

// 温度换算：T(°C) = -46.85 + 175.72 × St / 2^16。
constexpr float kTempOffsetC = -46.85f;
constexpr float kTempSlope   = 175.72f;
constexpr float kTempDivisor = 65536.0f;

}  // namespace

std::uint8_t Htu21dCrc8(std::uint8_t value_msb, std::uint8_t value_lsb)
{
    std::uint8_t       crc      = 0x00;
    const std::uint8_t bytes[2] = {value_msb, value_lsb};
    for (int byte = 0; byte < 2; ++byte)
    {
        crc ^= bytes[byte];
        for (int bit = 0; bit < kBitsPerByte; ++bit)
        {
            if ((crc & 0x80) != 0)
            {
                crc = static_cast<std::uint8_t>((crc << 1) ^ kCrc8Poly);
            }
            else
            {
                crc = static_cast<std::uint8_t>(crc << 1);
            }
        }
    }
    return crc;
}

bool Htu21dCrcOk(std::uint16_t value, std::uint8_t crc_byte)
{
    const std::uint8_t msb = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    const std::uint8_t lsb = static_cast<std::uint8_t>(value & 0xFF);
    return Htu21dCrc8(msb, lsb) == crc_byte;
}

float Htu21dRhPercent(std::uint16_t srh)
{
    return kRhOffsetPercent + kRhSlope * static_cast<float>(srh) / kRhDivisor;
}

float Htu21dTempC(std::uint16_t st)
{
    return kTempOffsetC + kTempSlope * static_cast<float>(st) / kTempDivisor;
}

}  // namespace animal_monitor
