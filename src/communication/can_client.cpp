// ============================================================================
// 文件：src/communication/can_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/communication/can_client.h；
//       任务书 CODEX_MASTER_TASK.md §8/§9（SocketCAN + 内核 mcp251x 驱动）。
// ============================================================================

#include "animal_monitor/communication/can_client.h"

#include <cerrno>
#include <cstring>
#include <string>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace animal_monitor {

namespace {

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
    if (error == ENODEV || error == ENETDOWN)
    {
        return Status(StatusCode::kHardwareUnavailable, what + ": 接口未就绪");
    }
    return Status(StatusCode::kUnavailable, what + ": " + std::strerror(error));
}

}  // namespace

CanClient::CanClient(std::string interface_name) : interface_name_(std::move(interface_name))
{
}

CanClient::~CanClient()
{
    Close();
}

Status CanClient::Open()
{
    if (fd_ >= 0)
    {
        return Status::MakeOk();  // 幂等
    }
    if (interface_name_.empty())
    {
        return Status(StatusCode::kInvalidArgument, "CAN 接口名为空");
    }

    const int fd = ::socket(AF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0)
    {
        return StatusFromErrno(errno, "创建 AF_CAN socket 失败");
    }

    const unsigned int ifindex = ::if_nametoindex(interface_name_.c_str());
    if (ifindex == 0)
    {
        const int saved = errno;
        ::close(fd);
        return Status(StatusCode::kHardwareUnavailable,
                      "接口 " + interface_name_ + " 不存在: " + std::strerror(saved));
    }

    struct sockaddr_can addr
    {
    };
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifindex;
    if (::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        const int saved = errno;
        ::close(fd);
        return StatusFromErrno(saved, "bind 接口 " + interface_name_ + " 失败");
    }

    fd_ = fd;
    return Status::MakeOk();
}

Status CanClient::Send(const CanFrame& frame)
{
    if (fd_ < 0)
    {
        return Status(StatusCode::kUnavailable, "CAN socket 未打开");
    }
    if (!ValidateDlc(frame.dlc))
    {
        return Status(StatusCode::kInvalidArgument, "非法 DLC（经典 CAN 合法范围 0..8）");
    }

    struct can_frame cf
    {
    };
    cf.can_id = frame.can_id & (frame.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    if (frame.extended)
    {
        cf.can_id |= CAN_EFF_FLAG;
    }
    if (frame.rtr)
    {
        cf.can_id |= CAN_RTR_FLAG;
    }
    cf.len      = frame.dlc;
    cf.len8_dlc = frame.dlc;  // 经典帧两字段保持一致，兼容新旧内核结构
    std::memcpy(cf.data, frame.data, frame.dlc);

    const ssize_t written = ::write(fd_, &cf, sizeof(cf));
    if (written < 0)
    {
        return StatusFromErrno(errno, "发送 CAN 帧失败");
    }
    if (static_cast<std::size_t>(written) != sizeof(cf))
    {
        return Status(StatusCode::kUnavailable, "发送 CAN 帧短写");
    }
    return Status::MakeOk();
}

Status CanClient::Receive(CanFrame* frame, int timeout_ms)
{
    if (frame == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "frame 指针为空");
    }
    if (fd_ < 0)
    {
        return Status(StatusCode::kUnavailable, "CAN socket 未打开");
    }

    struct pollfd pfd
    {
    };
    pfd.fd     = fd_;
    pfd.events = POLLIN;
    const int poll_result = ::poll(&pfd, 1, timeout_ms);
    if (poll_result < 0)
    {
        return StatusFromErrno(errno, "poll CAN socket 失败");
    }
    if (poll_result == 0)
    {
        return Status(StatusCode::kTimeout, "等待 CAN 帧超时");
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return Status(StatusCode::kUnavailable, "CAN socket 异常事件");
    }

    struct can_frame cf
    {
    };
    const ssize_t received = ::read(fd_, &cf, sizeof(cf));
    if (received < 0)
    {
        return StatusFromErrno(errno, "接收 CAN 帧失败");
    }
    if (static_cast<std::size_t>(received) != sizeof(cf))
    {
        return Status(StatusCode::kUnavailable, "接收 CAN 帧短读");
    }
    if (cf.len > kMaxCanDlc)
    {
        return Status(StatusCode::kInvalidArgument, "收到非法 DLC（可能为 CAN FD 帧）");
    }

    CanFrame parsed;
    parsed.can_id   = cf.can_id & (cf.can_id & CAN_EFF_FLAG ? CAN_EFF_MASK : CAN_SFF_MASK);
    parsed.extended = (cf.can_id & CAN_EFF_FLAG) != 0;
    parsed.rtr      = (cf.can_id & CAN_RTR_FLAG) != 0;
    parsed.dlc      = cf.len;
    std::memcpy(parsed.data, cf.data, cf.len);
    std::memset(parsed.data + cf.len, 0, kMaxCanDlc - cf.len);
    *frame = parsed;
    return Status::MakeOk();
}

void CanClient::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace animal_monitor
