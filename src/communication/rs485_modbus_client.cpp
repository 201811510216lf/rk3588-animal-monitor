// ============================================================================
// 文件：src/communication/rs485_modbus_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/communication/rs485_modbus_client.h；
//       任务书 CODEX_MASTER_TASK.md §8（RS485 用 UART RS485 mode + TIOCSRS485）。
// 说明：Modbus RTU 帧间静默间隔（3.5 字符时间）由内核串口驱动与主站调度
//       （总线上单事务串行）保证；本实现不在用户态自行计算字符时间。
// ============================================================================

#include "animal_monitor/communication/rs485_modbus_client.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "animal_monitor/common/modbus_crc.h"

namespace animal_monitor {

namespace {

constexpr std::size_t kMaxModbusWireSize = 256;  // PDU 上限 253 + 从站 + 功能码 + CRC

Status StatusFromErrno(int error, const std::string& what)
{
    if (error == EINTR)
    {
        return Status(StatusCode::kCancelled, what + ": 被信号中断");
    }
    if (error == EAGAIN || error == EWOULDBLOCK)
    {
        return Status(StatusCode::kTimeout, what + ": 无数据");
    }
    if (error == EACCES || error == EPERM)
    {
        return Status(StatusCode::kPermissionDenied, what + ": 权限不足");
    }
    if (error == ENODEV || error == ENOENT)
    {
        return Status(StatusCode::kHardwareUnavailable, what + ": 设备不存在");
    }
    if (error == EBUSY)
    {
        return Status(StatusCode::kUnavailable, what + ": 设备忙");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

// 标准波特率 → termios 常量；不支持的波特率返回 -1。
int BaudConstant(int baud)
{
    switch (baud)
    {
        case 1200:
            return B1200;
        case 2400:
            return B2400;
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
#ifdef B230400
        case 230400:
            return B230400;
#endif
        default:
            return -1;
    }
}

}  // namespace

Rs485ModbusClient::Rs485ModbusClient(std::string tty_path, int baud, Rs485Parity parity,
                                     int timeout_ms)
    : tty_path_(std::move(tty_path)), baud_(baud), parity_(parity), timeout_ms_(timeout_ms)
{
}

Rs485ModbusClient::~Rs485ModbusClient()
{
    Close();
}

Status Rs485ModbusClient::Open()
{
    if (fd_ >= 0)
    {
        return Status::MakeOk();  // 幂等
    }
    if (tty_path_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "串口路径为空");
    }
    if (timeout_ms_ <= 0)
    {
        return Status(StatusCode::kInvalidArgument, "timeout_ms 必须大于 0");
    }
    const int speed = BaudConstant(baud_);
    if (speed < 0)
    {
        return Status(StatusCode::kInvalidArgument,
                      "不支持的波特率: " + std::to_string(baud_));
    }

    const int fd = ::open(tty_path_.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "打开串口 " + tty_path_ + " 失败");
    }

    struct termios tio
    {
    };
    if (::tcgetattr(fd, &tio) < 0)
    {
        const int saved = errno;
        ::close(fd);
        return StatusFromErrno(saved, "tcgetattr 失败");
    }

    ::cfmakeraw(&tio);  // raw 模式：VMIN=1、VTIME=0，读取配合 poll 使用
    tio.c_cflag |= CREAD | CLOCAL;  // 本地串口，无调制解调器控制
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
    tio.c_cflag |= CS8;  // 8 数据位
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);  // 1 停止位
    switch (parity_)
    {
        case Rs485Parity::kNone:
            tio.c_cflag &= ~static_cast<tcflag_t>(PARENB);
            break;
        case Rs485Parity::kEven:
            tio.c_cflag |= PARENB;
            tio.c_cflag &= ~static_cast<tcflag_t>(PARODD);
            break;
        case Rs485Parity::kOdd:
            tio.c_cflag |= PARENB | PARODD;
            break;
    }
    if (::cfsetspeed(&tio, static_cast<speed_t>(speed)) < 0)
    {
        const int saved = errno;
        ::close(fd);
        return StatusFromErrno(saved, "设置波特率失败");
    }
    if (::tcsetattr(fd, TCSANOW, &tio) < 0)
    {
        const int saved = errno;
        ::close(fd);
        return StatusFromErrno(saved, "tcsetattr 失败");
    }

    // RS485 半双工模式：启用内核方向自动控制（RTS 在发送期间有效）。
    // 失败非致命：部分板卡收发器自带自动方向控制，无需内核切换。
    struct serial_rs485 rs485
    {
    };
    rs485.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    if (::ioctl(fd, TIOCSRS485, &rs485) < 0)
    {
        rs485_warning_ = "TIOCSRS485 失败（" + std::string(std::strerror(errno)) +
                         "）：按普通 UART 模式继续，请确认收发器方向控制";
    }
    else
    {
        rs485_warning_.clear();
    }

    fd_ = fd;
    return Status::MakeOk();
}

Status Rs485ModbusClient::ReadExactly(std::vector<std::uint8_t>* out, std::size_t count)
{
    while (out->size() < count)
    {
        struct pollfd pfd
        {
        };
        pfd.fd     = fd_;
        pfd.events = POLLIN;
        const int poll_result = ::poll(&pfd, 1, timeout_ms_);
        if (poll_result < 0)
        {
            return StatusFromErrno(errno, "poll 串口失败");
        }
        if (poll_result == 0)
        {
            return Status(StatusCode::kTimeout, "读取响应超时");
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return Status(StatusCode::kUnavailable, "串口异常事件");
        }

        std::uint8_t buffer[64];
        const std::size_t want = count - out->size();
        const std::size_t chunk = want < sizeof(buffer) ? want : sizeof(buffer);
        const ssize_t received = ::read(fd_, buffer, chunk);
        if (received < 0)
        {
            return StatusFromErrno(errno, "读取串口失败");
        }
        if (received == 0)
        {
            return Status(StatusCode::kUnavailable, "串口 EOF（对端关闭）");
        }
        out->insert(out->end(), buffer, buffer + received);
    }
    return Status::MakeOk();
}

Status Rs485ModbusClient::Transact(const ModbusFrame& request, ModbusFrame* response)
{
    if (response == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "response 指针为空");
    }
    if (fd_ < 0)
    {
        return Status(StatusCode::kUnavailable, "串口未打开");
    }
    if (IsException(request, nullptr))
    {
        return Status(StatusCode::kInvalidArgument, "主站不能发送异常帧");
    }

    std::vector<std::uint8_t> wire;
    wire.reserve(request.data.size() + 4);
    wire.push_back(request.slave);
    wire.push_back(request.function);
    wire.insert(wire.end(), request.data.begin(), request.data.end());
    AppendModbusCrc(wire);
    if (wire.size() > kMaxModbusWireSize)
    {
        return Status(StatusCode::kInvalidArgument, "请求帧超出 Modbus RTU 长度上限 256 字节");
    }

    // 两次尝试：超时、EOF、CRC/长度错误时重试一次。
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (::tcflush(fd_, TCIOFLUSH) < 0)
        {
            return StatusFromErrno(errno, "tcflush 失败");
        }

        std::size_t written_total = 0;
        while (written_total < wire.size())
        {
            const ssize_t written =
                ::write(fd_, wire.data() + written_total, wire.size() - written_total);
            if (written < 0)
            {
                return StatusFromErrno(errno, "写请求帧失败");
            }
            written_total += static_cast<std::size_t>(written);
        }

        // 先读从站+功能码两字节，确定响应剩余长度（Modbus 响应最小 4 字节）。
        std::vector<std::uint8_t> received;
        received.reserve(kMaxModbusWireSize);
        Status status = ReadExactly(&received, 2);
        if (!status.Ok())
        {
            if (attempt == 0)
            {
                continue;  // 重试一次
            }
            return Status(status.Code(), status.Message() + "（已重试一次）");
        }

        const std::uint8_t function = received[1];
        std::size_t remaining = 0;
        if ((function & 0x80) != 0)
        {
            remaining = 3;  // 异常码 + CRC
        }
        else if (function == 0x03)
        {
            status = ReadExactly(&received, 3);
            if (!status.Ok())
            {
                if (attempt == 0)
                {
                    continue;
                }
                return Status(status.Code(), status.Message() + "（已重试一次）");
            }
            remaining = static_cast<std::size_t>(received[2]) + 2;  // 数据 + CRC
        }
        else if (function == 0x06)
        {
            remaining = 6;  // 地址 2 + 值 2 + CRC 2
        }
        else
        {
            // 未知功能码：按"字节计数型"响应假设（1 字节字节数 + 数据 + CRC）。
            // 仅在主站使用其他功能码时适用，当前业务仅 0x03/0x06。
            status = ReadExactly(&received, 3);
            if (!status.Ok())
            {
                if (attempt == 0)
                {
                    continue;
                }
                return Status(status.Code(), status.Message() + "（已重试一次）");
            }
            remaining = static_cast<std::size_t>(received[2]) + 2;
        }

        status = ReadExactly(&received, received.size() + remaining);
        if (!status.Ok())
        {
            if (attempt == 0)
            {
                continue;
            }
            return Status(status.Code(), status.Message() + "（已重试一次）");
        }
        if (received.size() > kMaxModbusWireSize)
        {
            return Status(StatusCode::kInvalidArgument, "响应帧超出 Modbus RTU 长度上限");
        }

        ModbusFrame parsed;
        std::string  err;
        status = ParseFrame(received, &parsed, &err);
        if (!status.Ok())
        {
            if (attempt == 0)
            {
                continue;  // CRC/长度错误，重试一次
            }
            return Status(status.Code(), "响应解析失败: " + err + "（已重试一次）");
        }

        // 从站/功能码一致性：不匹配说明对端应答异常或总线残留，重试无意义。
        const bool function_ok =
            parsed.function == request.function ||
            parsed.function == static_cast<std::uint8_t>(request.function | 0x80);
        if (parsed.slave != request.slave || !function_ok)
        {
            return Status(StatusCode::kUnavailable, "响应与请求不匹配（从站/功能码不一致）");
        }

        *response = std::move(parsed);
        return Status::MakeOk();
    }

    return Status(StatusCode::kUnavailable, "事务失败（逻辑上不可达）");
}

void Rs485ModbusClient::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace animal_monitor
