// ============================================================================
// 文件：src/communication/modbus_rtu.cpp
// 状态：implemented（host 可编译纯逻辑；已由 tests/unit/test_modbus_rtu.cpp 覆盖）
// 依据：include/animal_monitor/communication/modbus_rtu.h；
//       任务书 CODEX_MASTER_TASK.md §8/§9。CRC 复用 common/modbus_crc.h。
// ============================================================================

#include "animal_monitor/communication/modbus_rtu.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "animal_monitor/common/modbus_crc.h"

namespace animal_monitor {

namespace {

constexpr std::uint8_t kReadHoldingRegisters = 0x03;
constexpr std::uint8_t kWriteSingleRegister  = 0x06;
constexpr std::uint8_t kExceptionFlag        = 0x80;

// Modbus RTU 帧最小长度：从站 + 功能码 + CRC。
constexpr std::size_t kMinWireSize = 4;

// 单次读保持寄存器上限（Modbus 规范）。
constexpr std::uint16_t kMaxReadCount = 125;

Status InvalidArgument(const std::string& message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

}  // namespace

Status BuildReadHoldingRegisters(std::uint8_t slave, std::uint16_t addr, std::uint16_t count,
                                 std::vector<std::uint8_t>* frame)
{
    if (frame == nullptr)
    {
        return InvalidArgument("frame 指针为空");
    }
    if (count == 0 || count > kMaxReadCount)
    {
        return InvalidArgument("读寄存器数量越界：count 必须在 [1, 125] 内");
    }

    frame->clear();
    frame->push_back(slave);
    frame->push_back(kReadHoldingRegisters);
    frame->push_back(static_cast<std::uint8_t>(addr >> 8));
    frame->push_back(static_cast<std::uint8_t>(addr & 0xFF));
    frame->push_back(static_cast<std::uint8_t>(count >> 8));
    frame->push_back(static_cast<std::uint8_t>(count & 0xFF));
    AppendModbusCrc(*frame);
    return Status::MakeOk();
}

Status BuildWriteSingleRegister(std::uint8_t slave, std::uint16_t addr, std::uint16_t value,
                                std::vector<std::uint8_t>* frame)
{
    if (frame == nullptr)
    {
        return InvalidArgument("frame 指针为空");
    }

    frame->clear();
    frame->push_back(slave);
    frame->push_back(kWriteSingleRegister);
    frame->push_back(static_cast<std::uint8_t>(addr >> 8));
    frame->push_back(static_cast<std::uint8_t>(addr & 0xFF));
    frame->push_back(static_cast<std::uint8_t>(value >> 8));
    frame->push_back(static_cast<std::uint8_t>(value & 0xFF));
    AppendModbusCrc(*frame);
    return Status::MakeOk();
}

Status ParseFrame(const std::vector<std::uint8_t>& wire, ModbusFrame* out, std::string* err)
{
    const auto fail = [err](const std::string& message)
    {
        if (err != nullptr)
        {
            *err = message;
        }
        return Status(StatusCode::kInvalidArgument, message);
    };

    if (out == nullptr)
    {
        return fail("out 指针为空");
    }
    if (wire.size() < kMinWireSize)
    {
        return fail("帧长不足：Modbus RTU 最小帧为 4 字节（从站+功能码+CRC）");
    }
    if (!VerifyModbusCrc(wire))
    {
        return fail("CRC 校验失败");
    }

    const std::uint8_t slave    = wire[0];
    const std::uint8_t function = wire[1];

    // 从站与功能码一致性校验。
    if (slave == 0)
    {
        return fail("从站地址 0 为广播地址，仅允许出现在请求方向");
    }
    if ((function & 0x7F) == 0)
    {
        return fail("非法功能码：功能码低 7 位为 0");
    }

    ModbusFrame parsed;
    parsed.slave    = slave;
    parsed.function = function;
    parsed.data.assign(wire.begin() + 2, wire.end() - 2);  // 去掉首 2 字节与尾 CRC
    if ((function & kExceptionFlag) != 0 && parsed.data.size() != 1)
    {
        return fail("异常帧数据区必须恰好 1 字节（异常码）");
    }

    *out = std::move(parsed);
    return Status::MakeOk();
}

Status ParseReadHoldingResponse(const ModbusFrame& frame, std::vector<std::uint16_t>* registers,
                                std::string* err)
{
    const auto fail = [err](const std::string& message)
    {
        if (err != nullptr)
        {
            *err = message;
        }
        return Status(StatusCode::kInvalidArgument, message);
    };

    if (registers == nullptr)
    {
        return fail("registers 指针为空");
    }

    std::uint8_t exception_code = 0;
    if (IsException(frame, &exception_code))
    {
        return fail("收到异常响应：异常码 " + std::to_string(exception_code));
    }
    if (frame.function != kReadHoldingRegisters)
    {
        return fail("功能码不符：期望 0x03（读保持寄存器响应）");
    }
    if (frame.data.empty())
    {
        return fail("缺少字节数字段");
    }

    const std::uint8_t byte_count = frame.data[0];
    if ((byte_count % 2) != 0)
    {
        return fail("字节数必须为偶数（等于 2×寄存器数）");
    }
    if (frame.data.size() != static_cast<std::size_t>(byte_count) + 1)
    {
        return fail("字节数字段与实际数据长度不一致");
    }

    registers->clear();
    const std::size_t register_count = byte_count / 2;
    registers->reserve(register_count);
    for (std::size_t i = 0; i < register_count; ++i)
    {
        const std::uint16_t value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(frame.data[1 + 2 * i]) << 8) | frame.data[2 + 2 * i]);
        registers->push_back(value);
    }
    return Status::MakeOk();
}

bool IsException(const ModbusFrame& frame, std::uint8_t* exception_code)
{
    const bool is_exception = (frame.function & kExceptionFlag) != 0;
    if (exception_code != nullptr)
    {
        *exception_code = is_exception && !frame.data.empty() ? frame.data[0] : 0;
    }
    return is_exception;
}

}  // namespace animal_monitor
