// ============================================================================
// 文件：src/thermal/mlx90640_client.cpp
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与编译）
// 依据：include/animal_monitor/thermal/mlx90640_client.h；
//       kernel/drivers/mlx90640/uapi/mlx90640.h（目标构建 include 路径需含 kernel/drivers）
// 声明：输出为未标定估算（NOT_CALIBRATED）。
// ============================================================================

#include "animal_monitor/thermal/mlx90640_client.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mlx90640/uapi/mlx90640.h"

namespace animal_monitor {

namespace {

constexpr std::uint32_t kDefaultCameraId = 0;  // 板卡配置项接入前固定为 0（UNVERIFIED）

// 读取失败（短读）时返回 kInternal；信号中断返回 kCancelled（调用方决定重试）。
Status StatusFromErrno(int error, const char* what)
{
    if (error == EINTR)
    {
        return Status(StatusCode::kCancelled, std::string(what) + ": 被信号中断");
    }
    if (error == EAGAIN || error == EWOULDBLOCK)
    {
        return Status(StatusCode::kTimeout, std::string(what) + ": 无可用帧");
    }
    if (error == ENODEV)
    {
        return Status(StatusCode::kHardwareUnavailable, std::string(what) + ": 设备已移除");
    }
    return Status(StatusCode::kUnavailable,
                  std::string(what) + ": " + std::strerror(error));
}

}  // namespace

Mlx90640Client::Mlx90640Client(std::string device_path)
    : device_path_(std::move(device_path))
{
}

Mlx90640Client::~Mlx90640Client()
{
    Close();
}

Status Mlx90640Client::Open()
{
    if (fd_ >= 0)
    {
        return Status::MakeOk();  // 幂等
    }

    fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
    {
        fd_ = -1;
        return Status(StatusCode::kUnavailable,
                      "打开 " + device_path_ + " 失败: " + std::strerror(errno));
    }

    // ABI 版本校验：版本不匹配禁止继续使用（结构布局可能已变）。
    __u32 abi = 0;
    if (::ioctl(fd_, MLX_IOC_GET_ABI, &abi) < 0)
    {
        const Status status = StatusFromErrno(errno, "MLX_IOC_GET_ABI");
        Close();
        return status;
    }
    if (abi != MLX90640_ABI_VERSION)
    {
        Close();
        return Status(StatusCode::kUnavailable,
                      "ABI 版本不匹配: 驱动 " + std::to_string(abi) +
                          " 客户端 " + std::to_string(MLX90640_ABI_VERSION));
    }

    // 标定参数拷贝 → 用户态解析（内核不做温度算法）。
    struct mlx90640_params params;
    std::memset(&params, 0, sizeof(params));
    if (::ioctl(fd_, MLX_IOC_GET_PARAMS, &params) < 0)
    {
        const Status status = StatusFromErrno(errno, "MLX_IOC_GET_PARAMS");
        Close();
        return status;
    }
    if (params.abi_version != MLX90640_ABI_VERSION || params.eeprom_size != MLX90640_EEPROM_SIZE)
    {
        Close();
        return Status(StatusCode::kUnavailable, "标定参数结构与 ABI 不匹配");
    }

    const Status parse = ParseEeprom(params.eeprom, &eeprom_);
    eeprom_ok_ = parse.Ok();
    // 解析失败不关闭设备：原始帧仍可读，像素以 NaN 标记（见 ReadFrame）。

    return Status::MakeOk();
}

void Mlx90640Client::Close()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    eeprom_ok_ = false;
}

Status Mlx90640Client::ReadFrame(ThermalFrame* frame, int timeout_ms)
{
    if (frame == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "ReadFrame: frame 不能为空指针");
    }
    if (fd_ < 0)
    {
        return Status(StatusCode::kUnavailable, "ReadFrame: 设备未打开");
    }

    struct pollfd pfd;
    pfd.fd      = fd_;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    const int poll_ret = ::poll(&pfd, 1, timeout_ms);
    if (poll_ret == 0)
    {
        return Status(StatusCode::kTimeout, "ReadFrame: poll 超时");
    }
    if (poll_ret < 0)
    {
        return StatusFromErrno(errno, "ReadFrame: poll");
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return Status(StatusCode::kHardwareUnavailable, "ReadFrame: 设备异常/已移除");
    }

    struct mlx90640_frame kframe;
    std::memset(&kframe, 0, sizeof(kframe));
    const ssize_t n = ::read(fd_, &kframe, sizeof(kframe));
    if (n < 0)
    {
        return StatusFromErrno(errno, "ReadFrame: read");
    }
    if (n != static_cast<ssize_t>(sizeof(kframe)))
    {
        return Status(StatusCode::kInternal, "ReadFrame: 帧短读");
    }
    if (kframe.abi_version != MLX90640_ABI_VERSION)
    {
        return Status(StatusCode::kUnavailable, "ReadFrame: 帧 ABI 版本不匹配");
    }

    // 组装用户态 ThermalFrame（内核已按交错模式合并 raw）。
    frame->id.sequence  = kframe.frame_id;
    frame->id.camera_id = kDefaultCameraId;
    frame->timestamps.monotonic_ns = static_cast<std::int64_t>(kframe.timestamp_ns);
    struct timespec wall_ts
    {
    };
    ::clock_gettime(CLOCK_REALTIME, &wall_ts);
    frame->timestamps.wall_clock_ns = static_cast<std::int64_t>(wall_ts.tv_sec) * 1000000000LL +
                                      static_cast<std::int64_t>(wall_ts.tv_nsec);

    frame->status_bits = kframe.status_bits | thermal_status_bits::kNotCalibrated;
    if (!eeprom_ok_)
    {
        frame->status_bits &= ~thermal_status_bits::kEepromOk;
        frame->status_bits |= thermal_status_bits::kOutOfRange;
    }

    // 刷新帧内 RAM 值（温度换算所需）。
    eeprom_.ta_raw       = kframe.ta_raw;
    eeprom_.vdd_raw      = kframe.vdd_raw;
    eeprom_.gain_ram     = kframe.gain_raw;
    eeprom_.cp_pixel[0]  = kframe.cp_pixel[0];
    eeprom_.cp_pixel[1]  = kframe.cp_pixel[1];
    eeprom_.pix_os_cp    = kframe.ptat_art_raw;
    eeprom_.resolution_ram = eeprom_.resolution_ee;  // 初稿：帧内分辨率快照与 EEPROM 一致

    const float ta  = CalcTa(eeprom_, kframe.ta_raw);
    const float vdd = CalcVdd(eeprom_, kframe.vdd_raw);
    frame->ta_c  = ta;
    frame->vdd_v = vdd;
    if (std::isnan(ta) || std::isnan(vdd))
    {
        frame->status_bits |= thermal_status_bits::kOutOfRange;
    }

    if (eeprom_ok_)
    {
        for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
        {
            const float v_ir = TgcCompensation(eeprom_, static_cast<std::uint32_t>(i),
                                               kframe.raw[i], kframe.subpage_id);
            const float temp = CalcTo(eeprom_, static_cast<std::uint32_t>(i), v_ir, ta);
            if (std::isnan(temp))
            {
                frame->status_bits |= thermal_status_bits::kOutOfRange;
            }
            frame->pixels[i] = temp;
        }
    }
    else
    {
        for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
        {
            frame->pixels[i] = std::nanf("");  // 标定缺失：不做无意义的温度换算
        }
    }

    return Status::MakeOk();
}

}  // namespace animal_monitor
