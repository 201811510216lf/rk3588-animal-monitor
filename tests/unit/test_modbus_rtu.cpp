// ============================================================================
// 文件：tests/unit/test_modbus_rtu.cpp
// 状态：implemented（host 单测，MSVC /W4 /permissive- 零告警）
// 依据：include/animal_monitor/communication/modbus_rtu.h；
//       与 tests/unit/test_modbus_crc.cpp 的权威向量一致（01 03 00 00 00 0A → C5 CD）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "test_harness.h"

#include "animal_monitor/common/modbus_crc.h"
#include "animal_monitor/common/status.h"
#include "animal_monitor/communication/modbus_rtu.h"

using animal_monitor::AppendModbusCrc;
using animal_monitor::BuildReadHoldingRegisters;
using animal_monitor::BuildWriteSingleRegister;
using animal_monitor::IsException;
using animal_monitor::ModbusFrame;
using animal_monitor::ParseFrame;
using animal_monitor::ParseReadHoldingResponse;
using animal_monitor::Status;
using animal_monitor::StatusCode;

namespace {

// 权威读保持寄存器请求：01 03 00 00 00 0A + CRC（寄存器值 0xCDC5，低字节在前 C5 CD）。
AM_TEST(build_read_holding_registers_exact_wire_bytes)
{
    std::vector<std::uint8_t> frame;
    const Status              status = BuildReadHoldingRegisters(0x01, 0x0000, 0x000A, &frame);
    AM_EXPECT(status.Ok());

    const std::vector<std::uint8_t> expected = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
    AM_EXPECT_EQ(frame, expected);
}

AM_TEST(build_read_holding_registers_rejects_invalid_count)
{
    std::vector<std::uint8_t> frame;
    AM_EXPECT(!BuildReadHoldingRegisters(0x01, 0x0000, 0, &frame).Ok());
    AM_EXPECT(BuildReadHoldingRegisters(0x01, 0x0000, 0, &frame).Code() ==
              StatusCode::kInvalidArgument);
    AM_EXPECT(!BuildReadHoldingRegisters(0x01, 0x0000, 126, &frame).Ok());
    AM_EXPECT(BuildReadHoldingRegisters(0x01, 0x0000, 125, &frame).Ok());
    AM_EXPECT_EQ(frame.size(), 8u);
    AM_EXPECT_EQ(frame[5], 0x7Du);
}

AM_TEST(build_write_single_register_exact_wire_bytes)
{
    std::vector<std::uint8_t> frame;
    const Status              status = BuildWriteSingleRegister(0x01, 0x0001, 0x0003, &frame);
    AM_EXPECT(status.Ok());

    // 01 06 00 01 00 03，CRC 寄存器值 0x0B98 → 低字节在前 98 0B。
    const std::vector<std::uint8_t> expected = {0x01, 0x06, 0x00, 0x01, 0x00, 0x03, 0x98, 0x0B};
    AM_EXPECT_EQ(frame, expected);
}

// 解析读保持寄存器响应 01 03 04 00 0A 01 02 + CRC → 2 个寄存器 [0x000A, 0x0102]。
AM_TEST(parse_read_holding_response_ok)
{
    // 权威响应 CRC：寄存器值 0x605A → 低字节在前 5A 60。
    const std::vector<std::uint8_t> wire = {0x01, 0x03, 0x04, 0x00, 0x0A, 0x01, 0x02, 0x5A, 0x60};

    ModbusFrame            frame;
    std::string            err;
    const Status           parse = ParseFrame(wire, &frame, &err);
    AM_EXPECT(parse.Ok());
    AM_EXPECT_EQ(frame.slave, 0x01u);
    AM_EXPECT_EQ(frame.function, 0x03u);
    AM_EXPECT_EQ(frame.data.size(), 5u);

    std::vector<std::uint16_t> registers;
    AM_EXPECT(ParseReadHoldingResponse(frame, &registers, &err).Ok());
    AM_EXPECT_EQ(registers.size(), 2u);
    AM_EXPECT_EQ(registers[0], 0x000Au);
    AM_EXPECT_EQ(registers[1], 0x0102u);
}

AM_TEST(parse_frame_rejects_bad_crc)
{
    std::vector<std::uint8_t> wire = {0x01, 0x03, 0x04, 0x00, 0x0A, 0x01, 0x02};
    AppendModbusCrc(wire);
    ModbusFrame frame;
    std::string err;

    wire[8] ^= 0xFF;  // 破坏 CRC
    AM_EXPECT(!ParseFrame(wire, &frame, &err).Ok());
    AM_EXPECT(!err.empty());
}

AM_TEST(parse_frame_rejects_short_wire)
{
    const std::vector<std::uint8_t> wire = {0x01, 0x03, 0x00};
    ModbusFrame            frame;
    std::string            err;
    AM_EXPECT(!ParseFrame(wire, &frame, &err).Ok());
    AM_EXPECT(!err.empty());
}

AM_TEST(parse_frame_rejects_broadcast_slave)
{
    std::vector<std::uint8_t> wire = {0x00, 0x03, 0x00, 0x00, 0x00, 0x01};
    AppendModbusCrc(wire);
    ModbusFrame frame;
    std::string err;
    AM_EXPECT(!ParseFrame(wire, &frame, &err).Ok());
    AM_EXPECT(err.find("广播") != std::string::npos);
}

// 异常帧：从站 1、功能码 0x83（读保持寄存器异常）、异常码 0x02（非法数据地址）。
AM_TEST(exception_frame_recognized)
{
    // CRC 寄存器值 0xF1C0 → 低字节在前 C0 F1。
    const std::vector<std::uint8_t> wire = {0x01, 0x83, 0x02, 0xC0, 0xF1};

    ModbusFrame frame;
    std::string err;
    AM_EXPECT(ParseFrame(wire, &frame, &err).Ok());

    std::uint8_t code = 0;
    AM_EXPECT(IsException(frame, &code));
    AM_EXPECT_EQ(code, 0x02u);

    // 异常响应不能按正常响应解析出寄存器。
    std::vector<std::uint16_t> registers;
    AM_EXPECT(!ParseReadHoldingResponse(frame, &registers, &err).Ok());
    AM_EXPECT(err.find("异常") != std::string::npos);
    AM_EXPECT(registers.empty());
}

AM_TEST(is_exception_returns_false_for_normal_frame)
{
    const ModbusFrame frame{0x01, 0x03, {0x04, 0x00, 0x0A, 0x01, 0x02}};
    std::uint8_t      code = 0xFF;
    AM_EXPECT(!IsException(frame, &code));
    AM_EXPECT_EQ(code, 0u);
}

AM_TEST(parse_response_rejects_odd_byte_count)
{
    const ModbusFrame frame{0x01, 0x03, {0x05, 0x00, 0x01, 0x02, 0x03}};
    std::vector<std::uint16_t> registers;
    std::string                err;
    AM_EXPECT(!ParseReadHoldingResponse(frame, &registers, &err).Ok());
    AM_EXPECT(err.find("偶数") != std::string::npos);
}

AM_TEST(parse_response_rejects_byte_count_mismatch)
{
    // 字节数声明 4，实际只有 2 字节数据。
    const ModbusFrame frame{0x01, 0x03, {0x04, 0x00, 0x01}};
    std::vector<std::uint16_t> registers;
    std::string                err;
    AM_EXPECT(!ParseReadHoldingResponse(frame, &registers, &err).Ok());
    AM_EXPECT(err.find("不一致") != std::string::npos);
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
