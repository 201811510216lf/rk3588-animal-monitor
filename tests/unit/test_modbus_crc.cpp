#include <cstdint>
#include <vector>

#include "test_harness.h"

#include "animal_monitor/common/modbus_crc.h"

using animal_monitor::AppendModbusCrc;
using animal_monitor::ModbusCrc16;
using animal_monitor::VerifyModbusCrc;

namespace
{

// 权威测试向量（两处独立锚点）：
// 1) CRC-16/MODBUS 目录校验值："123456789" 的 CRC = 0x4B37；
// 2) 读保持寄存器请求 01 03 00 00 00 0A 的 CRC 寄存器值 = 0xCDC5，
//    线上追加顺序为低字节在前（C5 CD）。
// 注：部分公开资料把线上字节序 C5 CD 记为 0xC5CD，与本实现的寄存器值
// 0xCDC5 是同一结果的不同表示（本实现返回寄存器值，AppendModbusCrc 负责低字节在前）。
AM_TEST(known_vector_read_holding_registers)
{
    const std::vector<std::uint8_t> frame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    AM_EXPECT_EQ(ModbusCrc16(frame), 0xCDC5u);
}

AM_TEST(canonical_crc_catalogue_check_value)
{
    const std::string               text("123456789");
    const std::vector<std::uint8_t> frame(text.begin(), text.end());
    AM_EXPECT_EQ(ModbusCrc16(frame), 0x4B37u);
}

AM_TEST(append_produces_low_byte_first_wire_order)
{
    std::vector<std::uint8_t> frame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    AppendModbusCrc(frame);
    AM_EXPECT_EQ(frame.size(), 8u);
    AM_EXPECT_EQ(frame[6], 0xC5u);  // 低字节在前
    AM_EXPECT_EQ(frame[7], 0xCDu);
}

AM_TEST(verify_round_trip)
{
    std::vector<std::uint8_t> frame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    AppendModbusCrc(frame);
    AM_EXPECT(VerifyModbusCrc(frame));

    frame[6] ^= 0xFF;  // 破坏 CRC
    AM_EXPECT(!VerifyModbusCrc(frame));
}

AM_TEST(empty_frame_crc_is_initial_value) { AM_EXPECT_EQ(ModbusCrc16(nullptr, 0), 0xFFFFu); }

AM_TEST(short_frame_verify_returns_false)
{
    const std::vector<std::uint8_t> one_byte = {0x01};
    AM_EXPECT(!VerifyModbusCrc(one_byte));

    const std::vector<std::uint8_t> empty;
    AM_EXPECT(!VerifyModbusCrc(empty));
}

}  // namespace

int main() { return am_test::RunAll(); }
