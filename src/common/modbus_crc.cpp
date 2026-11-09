#include "animal_monitor/common/modbus_crc.h"

namespace animal_monitor
{

std::uint16_t ModbusCrc16(const std::uint8_t* data, std::size_t size)
{
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            if ((crc & 0x0001) != 0)
            {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001);
            }
            else
            {
                crc = static_cast<std::uint16_t>(crc >> 1);
            }
        }
    }
    return crc;
}

std::uint16_t ModbusCrc16(const std::vector<std::uint8_t>& data)
{
    return ModbusCrc16(data.data(), data.size());
}

void AppendModbusCrc(std::vector<std::uint8_t>& frame)
{
    const std::uint16_t crc = ModbusCrc16(frame.data(), frame.size());
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFF));  // 低字节在前
    frame.push_back(static_cast<std::uint8_t>((crc >> 8) & 0xFF));
}

bool VerifyModbusCrc(const std::vector<std::uint8_t>& frame)
{
    if (frame.size() < 2)
    {
        return false;
    }
    const std::size_t   payload_size = frame.size() - 2;
    const std::uint16_t expected =
        static_cast<std::uint16_t>(frame[payload_size] | (frame[payload_size + 1] << 8));
    return ModbusCrc16(frame.data(), payload_size) == expected;
}

}  // namespace animal_monitor
