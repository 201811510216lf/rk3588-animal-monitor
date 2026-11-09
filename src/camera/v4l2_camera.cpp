// 文件名：src/camera/v4l2_camera.cpp
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_ref_RKMediaProject.md 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject/src/v4l2Camera.cpp（只读审计），未逐行拷贝。
// 修复的参考实现缺陷：
//  - 错误路径统一回滚（无 goto 标签、无未检查返回值）；
//  - 全局状态/设备路径/分辨率硬编码全部参数化；
//  - 队列有界（容量配置化、drop_oldest）；
//  - QBUF 归还与帧生命周期绑定（BufferOwner 析构归还），避免“用完即归还”竞态；
//  - 相机析构后残留帧不再向已关闭 fd 发起 QBUF（弱引用归还通道）。
#include "animal_monitor/camera/v4l2_camera.h"

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../media/dma_heap_allocator.h"
#include "animal_monitor/common/bounded_queue.h"
#include "animal_monitor/logging/logger.h"

namespace animal_monitor {

namespace {

constexpr int kIoctlRetryLimit    = 4;    // EINTR 重试上限
constexpr int kPollTimeoutMs      = 250;  // 采集线程 poll 周期（兼顾停止响应速度）
constexpr auto kQueuePushTimeout  = std::chrono::milliseconds(50);

// V4L2 ioctl 封装：EINTR 自动重试（任务书要求），其余错误原样返回。
int Xioctl(int fd, unsigned long request, void* arg)
{
    int ret = -1;
    for (int attempt = 0; attempt < kIoctlRetryLimit; ++attempt)
    {
        ret = ::ioctl(fd, request, arg);
        if (ret == 0 || errno != EINTR)
        {
            return ret;
        }
    }
    errno = EINTR;
    return ret;
}

std::uint32_t ToV4l2PixelFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::kMjpeg: return V4L2_PIX_FMT_MJPEG;
        case PixelFormat::kNv12:  return V4L2_PIX_FMT_NV12;
        default:                  return 0;
    }
}

std::int64_t NowNanoseconds(clockid_t clock_id)
{
    timespec ts{};
    if (::clock_gettime(clock_id, &ts) != 0)
    {
        return 0;
    }
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL +
           static_cast<std::int64_t>(ts.tv_nsec);
}

std::int64_t TimevalToNanoseconds(const timeval& tv)
{
    return (static_cast<std::int64_t>(tv.tv_sec) * 1'000'000LL +
            static_cast<std::int64_t>(tv.tv_usec)) *
           1000LL;
}

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1u) / alignment * alignment;
}

}  // namespace

// 采集实现全部隐藏在 Impl 中；V4L2 类型不出现在公开头。
class V4l2Camera::Impl
{
  public:
    // 缓冲归还通道：FrameOwner 通过 shared_ptr 持有；
    // 相机 Stop/析构后置 valid=false，残留帧析构时跳过 QBUF（fd 已关闭）。
    struct RequeueChannel
    {
        int            fd    = -1;
        bool           valid = false;
        mutable std::mutex mutex;
    };

    // 采集池槽位：DmaBuffer 由槽位与在途帧共同持有（shared_ptr）。
    struct PoolSlot
    {
        std::shared_ptr<DmaBuffer> buffer;
        std::uint32_t               index = 0;
    };

    // 帧所有权：析构 = SYNC_END(READ) + QBUF 归还。
    // 只要 VideoFramePtr 仍被下游持有，缓冲就不会回池（buffer_lifecycle.md 契约）。
    class FrameOwner : public FdBufferOwner
    {
      public:
        FrameOwner(std::shared_ptr<PoolSlot> slot, std::shared_ptr<RequeueChannel> channel)
            : slot_(std::move(slot)), channel_(std::move(channel))
        {
        }

        ~FrameOwner() override { Release(); }

        int         Fd() const override { return slot_ && slot_->buffer ? slot_->buffer->Fd() : -1; }
        std::size_t Size() const override
        {
            return slot_ && slot_->buffer ? slot_->buffer->Size() : 0;
        }

      private:
        void Release()
        {
            if (!slot_ || !slot_->buffer || !channel_)
            {
                return;
            }

            // 结束 CPU 读窗口（system heap 上近似空操作，保留以兼容非一致性 heap）。
            (void)DmaSyncEndRead(slot_->buffer->Fd());

            std::lock_guard<std::mutex> lock(channel_->mutex);
            if (!channel_->valid || channel_->fd < 0)
            {
                return;  // 相机已关闭：跳过 QBUF，缓冲随本对象析构释放
            }
            v4l2_buffer buffer{};
            buffer.index  = slot_->index;
            buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_DMABUF;
            buffer.m.fd   = slot_->buffer->Fd();
            buffer.length = static_cast<__u32>(slot_->buffer->Size());
            if (Xioctl(channel_->fd, VIDIOC_QBUF, &buffer) < 0)
            {
                AM_LOG_WARNING() << "[V4L2] QBUF index=" << slot_->index
                                 << " 失败: " << std::strerror(errno);
            }
        }

        std::shared_ptr<PoolSlot>         slot_;
        std::shared_ptr<RequeueChannel>   channel_;
    };

    explicit Impl(V4l2CameraConfig config)
        : config_(std::move(config)),
          allocator_(config_.dma_heap_path),
          queue_(config_.queue_capacity == 0 ? 1 : config_.queue_capacity,
                 BoundedQueue<VideoFramePtr>::PushPolicy::kDropOldest)
    {
        name_ = "v4l2:" + config_.device;
        // Start 前的名义格式：按配置估算（NV12 stride 16 对齐，MJPEG 无行概念）。
        nominal_meta_.width  = config_.width;
        nominal_meta_.height = config_.height;
        nominal_meta_.format = config_.pixel_format;
        nominal_meta_.stride = (config_.pixel_format == PixelFormat::kNv12)
                                   ? AlignUp(config_.width, 16u)
                                   : 0u;
    }

    ~Impl() { Stop(); }

    const char*      Name() const { return name_.c_str(); }
    const FrameMeta& NominalMeta() const { return nominal_meta_; }

    Status Start()
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (capture_thread_.joinable())
        {
            return Status(StatusCode::kAlreadyExists, "[V4L2] 采集已启动");
        }

        const Status validate = ValidateConfig();
        if (!validate)
        {
            return validate;
        }

        // 以下任一步失败：TeardownDevice() 完整回滚（REQBUFS(0) → close）。
        fd_ = ::open(config_.device.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0)
        {
            AM_LOG_ERROR() << "[V4L2] 打开 " << config_.device << " 失败: "
                           << std::strerror(errno);
            return Status(StatusCode::kHardwareUnavailable,
                          "[V4L2] 打开设备失败: " + config_.device);
        }

        Status status = NegotiateFormat();
        if (!status)
        {
            TeardownDevice();
            return status;
        }
        status = SetFrameRate();
        if (!status)
        {
            TeardownDevice();
            return status;
        }
        status = AllocateAndQueueBuffers();
        if (!status)
        {
            TeardownDevice();
            return status;
        }
        status = ApplyExposure();
        if (!status)
        {
            TeardownDevice();
            return status;
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Xioctl(fd_, VIDIOC_STREAMON, &type) < 0)
        {
            AM_LOG_ERROR() << "[V4L2] STREAMON 失败: " << std::strerror(errno);
            TeardownDevice();
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] STREAMON 失败: ") + std::strerror(errno));
        }
        stream_on_ = true;

        stop_requested_.store(false, std::memory_order_relaxed);
        try
        {
            capture_thread_ = std::thread(&Impl::CaptureLoop, this);
        }
        catch (const std::system_error& error)
        {
            AM_LOG_ERROR() << "[V4L2] 创建采集线程失败: " << error.what();
            TeardownDevice();
            return Status(StatusCode::kInternal, "[V4L2] 创建采集线程失败");
        }
        return Status::MakeOk();
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stop_requested_.store(true, std::memory_order_relaxed);
        }
        queue_.Close();  // 唤醒 WaitForFrame 阻塞者（BoundedQueue::Close）

        if (capture_thread_.joinable())
        {
            capture_thread_.join();  // 采集线程最迟 kPollTimeoutMs 内退出
        }
        TeardownDevice();
    }

    Status WaitForFrame(VideoFramePtr* out, std::chrono::milliseconds timeout)
    {
        if (out == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[V4L2] out 为空");
        }
        if (queue_.Pop(*out, timeout))
        {
            return Status::MakeOk();
        }
        // 出队失败：队列关闭（排空后）→ kCancelled；纯超时 → kTimeout。
        if (stop_requested_.load(std::memory_order_relaxed) && queue_.Size() == 0)
        {
            return Status(StatusCode::kCancelled, "[V4L2] 采集已关停");
        }
        return Status(StatusCode::kTimeout, "[V4L2] 取帧超时");
    }

  private:
    Status ValidateConfig() const
    {
        if (config_.device.empty())
        {
            return Status(StatusCode::kInvalidArgument, "[V4L2] device 为空");
        }
        if (config_.width == 0 || config_.height == 0 || config_.fps == 0)
        {
            return Status(StatusCode::kInvalidArgument, "[V4L2] width/height/fps 必须非零");
        }
        if (config_.buffer_count < 2)
        {
            return Status(StatusCode::kInvalidArgument, "[V4L2] buffer_count 必须 >= 2");
        }
        if (config_.pixel_format != PixelFormat::kNv12 &&
            config_.pixel_format != PixelFormat::kMjpeg)
        {
            return Status(StatusCode::kInvalidArgument,
                          "[V4L2] pixel_format 仅支持 kNv12 / kMjpeg");
        }
        if (config_.exposure_ms < 0)
        {
            return Status(StatusCode::kInvalidArgument, "[V4L2] exposure_ms 必须 >= 0");
        }
        return Status::MakeOk();
    }

    Status NegotiateFormat()
    {
        v4l2_capability capability{};
        if (Xioctl(fd_, VIDIOC_QUERYCAP, &capability) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] QUERYCAP 失败: ") + std::strerror(errno));
        }
        if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
            (capability.capabilities & V4L2_CAP_STREAMING) == 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          "[V4L2] 设备不支持 VIDEO_CAPTURE/STREAMING");
        }

        const std::uint32_t requested_fourcc = ToV4l2PixelFormat(config_.pixel_format);
        v4l2_format format{};
        format.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width       = config_.width;
        format.fmt.pix.height      = config_.height;
        format.fmt.pix.pixelformat = requested_fourcc;
        format.fmt.pix.field       = V4L2_FIELD_NONE;
        if (Xioctl(fd_, VIDIOC_S_FMT, &format) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] S_FMT 失败: ") + std::strerror(errno));
        }
        // S_FMT 后 G_FMT 读回驱动最终生效配置（参考实现修复项：以读回值为准）。
        v4l2_format active{};
        active.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Xioctl(fd_, VIDIOC_G_FMT, &active) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] G_FMT 失败: ") + std::strerror(errno));
        }
        if (active.fmt.pix.pixelformat != requested_fourcc)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          "[V4L2] 驱动未保持请求的像素格式（驱动协商不匹配）");
        }
        if (active.fmt.pix.width == 0 || active.fmt.pix.height == 0)
        {
            return Status(StatusCode::kHardwareUnavailable, "[V4L2] 驱动返回非法分辨率");
        }

        active_width_       = active.fmt.pix.width;
        active_height_      = active.fmt.pix.height;
        active_stride_      = active.fmt.pix.bytesperline;
        active_sizeimage_   = active.fmt.pix.sizeimage;
        if (active_stride_ == 0)
        {
            active_stride_ = (config_.pixel_format == PixelFormat::kNv12)
                                 ? AlignUp(active_width_, 16u)
                                 : active_width_;
        }

        // 更新名义格式（能力协商后的真实值）。
        nominal_meta_.width  = active_width_;
        nominal_meta_.height = active_height_;
        nominal_meta_.stride = (config_.pixel_format == PixelFormat::kNv12) ? active_stride_ : 0;
        nominal_meta_.format = config_.pixel_format;

        if (active_width_ != config_.width || active_height_ != config_.height)
        {
            AM_LOG_WARNING() << "[V4L2] 分辨率协商为 " << active_width_ << "x" << active_height_
                             << "（请求 " << config_.width << "x" << config_.height << "）";
        }
        AM_LOG_INFO() << "[V4L2] 生效格式: " << active_width_ << "x" << active_height_
                      << " stride=" << active_stride_ << " sizeimage=" << active_sizeimage_;
        return Status::MakeOk();
    }

    Status SetFrameRate()
    {
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Xioctl(fd_, VIDIOC_G_PARM, &parm) < 0)
        {
            // 部分驱动不支持 G_PARM：不视为致命（保持驱动默认帧率，仅告警）。
            AM_LOG_WARNING() << "[V4L2] G_PARM 失败（保持默认帧率）: " << std::strerror(errno);
            return Status::MakeOk();
        }
        if ((parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0)
        {
            AM_LOG_WARNING() << "[V4L2] 驱动不支持设置帧率（保持默认）";
            return Status::MakeOk();
        }

        parm.parm.capture.timeperframe.numerator   = 1;
        parm.parm.capture.timeperframe.denominator = config_.fps;
        if (Xioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] S_PARM 失败: ") + std::strerror(errno));
        }

        // 读回最终生效帧率（告警用，不硬失败）。
        v4l2_streamparm readback{};
        readback.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Xioctl(fd_, VIDIOC_G_PARM, &readback) == 0 &&
            readback.parm.capture.timeperframe.numerator != 0)
        {
            const double fps = static_cast<double>(readback.parm.capture.timeperframe.denominator) /
                               static_cast<double>(readback.parm.capture.timeperframe.numerator);
            AM_LOG_INFO() << "[V4L2] 生效帧率: " << fps << " fps";
        }
        return Status::MakeOk();
    }

    Status AllocateAndQueueBuffers()
    {
        v4l2_requestbuffers request{};
        request.count  = config_.buffer_count;
        request.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_DMABUF;
        if (Xioctl(fd_, VIDIOC_REQBUFS, &request) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] REQBUFS(DMABUF) 失败: ") + std::strerror(errno));
        }
        if (request.count == 0)
        {
            return Status(StatusCode::kHardwareUnavailable, "[V4L2] 驱动批复 0 个缓冲");
        }

        // 单帧容量：以 sizeimage 为准，并对 MJPEG/NV12 设置合理下限，防止驱动
        // 报告的 sizeimage 过小导致溢出（参考实现缺陷：sizeimage 为 0 时直接使用）。
        const std::size_t minimum = (config_.pixel_format == PixelFormat::kNv12)
                                        ? static_cast<std::size_t>(active_stride_) *
                                              active_height_ * 3u / 2u
                                        : static_cast<std::size_t>(active_width_) *
                                              active_height_ * 3u;
        const std::size_t frame_size =
            std::max<std::size_t>(active_sizeimage_, minimum);
        if (frame_size == 0)
        {
            return Status(StatusCode::kHardwareUnavailable, "[V4L2] 帧缓冲大小为 0");
        }

        channel_ = std::make_shared<RequeueChannel>();
        channel_->fd    = fd_;
        channel_->valid = true;
        slots_.clear();
        slots_.reserve(request.count);
        for (std::uint32_t i = 0; i < request.count; ++i)
        {
            auto buffer = std::make_shared<DmaBuffer>();
            const Status status = allocator_.Allocate(frame_size, buffer.get());
            if (!status)
            {
                TeardownDevice();
                return status;
            }
            auto slot        = std::make_shared<PoolSlot>();
            slot->buffer     = std::move(buffer);
            slot->index      = i;
            slots_.push_back(std::move(slot));
        }

        for (const auto& slot : slots_)
        {
            v4l2_buffer buffer{};
            buffer.index  = slot->index;
            buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_DMABUF;
            buffer.m.fd   = slot->buffer->Fd();
            buffer.length = static_cast<__u32>(slot->buffer->Size());
            if (Xioctl(fd_, VIDIOC_QBUF, &buffer) < 0)
            {
                const Status status = Status(
                    StatusCode::kHardwareUnavailable,
                    std::string("[V4L2] QBUF index=") + std::to_string(slot->index) +
                        " 失败: " + std::strerror(errno));
                TeardownDevice();
                return status;
            }
        }
        AM_LOG_INFO() << "[V4L2] DMABUF 池就绪: 请求 " << config_.buffer_count << " 批复 "
                      << request.count << " 单帧 " << frame_size << " 字节";
        return Status::MakeOk();
    }

    Status ApplyExposure()
    {
        if (config_.exposure_ms == 0)
        {
            // 尝试自动曝光；控制不存在仅告警（MIPI ISP 相机常无 V4L2 曝光控制）。
            v4l2_control ctrl{};
            ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
            ctrl.value = V4L2_EXPOSURE_AUTO;
            if (Xioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0)
            {
                AM_LOG_WARNING() << "[V4L2] 设置自动曝光失败（不视为致命）: "
                                 << std::strerror(errno);
            }
            return Status::MakeOk();
        }

        // 手动曝光：切 MANUAL 模式 + 设置 V4L2_CID_EXPOSURE_ABSOLUTE（单位 100us）。
        v4l2_queryctrl query{};
        query.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        if (Xioctl(fd_, VIDIOC_QUERYCTRL, &query) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] QUERYCTRL(EXPOSURE_ABSOLUTE) 失败: ") +
                              std::strerror(errno));
        }
        if (query.flags & V4L2_CTRL_FLAG_DISABLED)
        {
            return Status(StatusCode::kHardwareUnavailable, "[V4L2] 曝光控制被驱动禁用");
        }

        const std::int32_t exposure_100us = config_.exposure_ms * 10;
        if (exposure_100us < query.minimum || exposure_100us > query.maximum)
        {
            return Status(StatusCode::kInvalidArgument,
                          "[V4L2] 曝光值超出驱动支持范围 [" +
                              std::to_string(query.minimum / 10) + ", " +
                              std::to_string(query.maximum / 10) + "] ms");
        }

        v4l2_control mode_ctrl{};
        mode_ctrl.id    = V4L2_CID_EXPOSURE_AUTO;
        mode_ctrl.value = V4L2_EXPOSURE_MANUAL;
        if (Xioctl(fd_, VIDIOC_S_CTRL, &mode_ctrl) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] 设置手动曝光模式失败: ") + std::strerror(errno));
        }

        v4l2_control value_ctrl{};
        value_ctrl.id    = V4L2_CID_EXPOSURE_ABSOLUTE;
        value_ctrl.value = exposure_100us;
        if (Xioctl(fd_, VIDIOC_S_CTRL, &value_ctrl) < 0)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[V4L2] 设置曝光值失败: ") + std::strerror(errno));
        }
        AM_LOG_INFO() << "[V4L2] 手动曝光 " << config_.exposure_ms << " ms";
        return Status::MakeOk();
    }

    // 统一回收设备资源（幂等）：STREAMOFF → 归还通道失效 → REQBUFS(0) → close。
    // Start 失败回滚与 Stop/析构共用此路径（修复参考实现多路径回滚不一致的问题）。
    void TeardownDevice()
    {
        if (stream_on_ && fd_ >= 0)
        {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (Xioctl(fd_, VIDIOC_STREAMOFF, &type) < 0)
            {
                AM_LOG_WARNING() << "[V4L2] STREAMOFF 失败: " << std::strerror(errno);
            }
            stream_on_ = false;
        }

        // 先使归还通道失效：此后 FrameOwner 析构不再 QBUF（fd 即将关闭）。
        if (channel_)
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            channel_->valid = false;
            channel_->fd    = -1;
        }

        if (fd_ >= 0)
        {
            v4l2_requestbuffers release{};
            release.count  = 0;
            release.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            release.memory = V4L2_MEMORY_DMABUF;
            if (Xioctl(fd_, VIDIOC_REQBUFS, &release) < 0)
            {
                AM_LOG_WARNING() << "[V4L2] REQBUFS(0) 失败: " << std::strerror(errno);
            }
            if (::close(fd_) != 0)
            {
                AM_LOG_WARNING() << "[V4L2] close 失败: " << std::strerror(errno);
            }
            fd_ = -1;
        }
        slots_.clear();  // 在途帧仍持有各自槽位，最终随帧释放
    }

    void CaptureLoop()
    {
        while (!stop_requested_.load(std::memory_order_relaxed))
        {
            pollfd pfd{};
            pfd.fd     = fd_;
            pfd.events = POLLIN | POLLPRI;
            const int poll_ret = ::poll(&pfd, 1, kPollTimeoutMs);
            if (poll_ret == 0)
            {
                continue;  // 周期唤醒以检查停止标志
            }
            if (poll_ret < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                AM_LOG_ERROR() << "[V4L2] poll 失败: " << std::strerror(errno);
                break;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                AM_LOG_ERROR() << "[V4L2] poll 报告设备异常 revents=0x" << std::hex
                               << pfd.revents << std::dec;
                break;
            }
            if ((pfd.revents & (POLLIN | POLLPRI)) == 0)
            {
                continue;
            }

            v4l2_buffer buffer{};
            buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_DMABUF;
            if (Xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                {
                    continue;
                }
                AM_LOG_ERROR() << "[V4L2] DQBUF 失败: " << std::strerror(errno);
                break;
            }
            if (buffer.index >= slots_.size())
            {
                AM_LOG_ERROR() << "[V4L2] DQBUF 返回非法索引 " << buffer.index;
                break;
            }

            std::shared_ptr<PoolSlot> slot = slots_[buffer.index];
            // CPU 读窗口开始（交给下游前必须保持；归还时 FrameOwner 负责 END）。
            const Status sync_status = DmaSyncStartRead(slot->buffer->Fd());
            if (!sync_status)
            {
                AM_LOG_WARNING() << sync_status.Message();
            }

            VideoFramePtr frame = std::make_shared<VideoFrame>();
            frame->meta          = nominal_meta_;
            frame->meta.id.sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
            const std::int64_t sensor_ns = TimevalToNanoseconds(buffer.timestamp);
            frame->meta.timestamps.monotonic_ns =
                sensor_ns > 0 ? sensor_ns : NowNanoseconds(CLOCK_MONOTONIC);
            frame->meta.timestamps.wall_clock_ns = NowNanoseconds(CLOCK_REALTIME);

            if (config_.pixel_format == PixelFormat::kNv12)
            {
                const std::uint8_t* base  = slot->buffer->ByteAddress();
                const std::size_t   total = static_cast<std::size_t>(active_stride_) *
                                            active_height_ * 3u / 2u;
                PlaneView y_plane;
                y_plane.data       = base;
                y_plane.size       = total;
                y_plane.row_stride = active_stride_;
                PlaneView uv_plane;
                uv_plane.data       = base + static_cast<std::size_t>(active_stride_) *
                                              active_height_;
                uv_plane.size       = static_cast<std::size_t>(active_stride_) *
                                      active_height_ / 2u;
                uv_plane.row_stride = active_stride_;
                frame->planes = {y_plane, uv_plane};
            }
            else
            {
                PlaneView mjpeg_plane;
                mjpeg_plane.data = slot->buffer->ByteAddress();
                mjpeg_plane.size = buffer.bytesused;  // MJPEG：压缩帧，无行步长概念
                frame->planes    = {mjpeg_plane};
            }

            frame->owner = std::make_shared<FrameOwner>(slot, channel_);

            // 有界队列（drop_oldest）：满时自动丢弃最旧帧，其 owner 析构即归还缓冲。
            if (!queue_.Push(std::move(frame), kQueuePushTimeout))
            {
                if (stop_requested_.load(std::memory_order_relaxed))
                {
                    break;
                }
                // kDropOldest 策略下 Push 仅在队列关闭时失败，此处只可能是停止竞态。
                continue;
            }
        }

        // 退出采集循环：队列保持关闭前已入队的帧，由 WaitForFrame 排空。
        stop_requested_.store(true, std::memory_order_relaxed);
    }

    V4l2CameraConfig          config_;
    std::string               name_;
    FrameMeta                 nominal_meta_;
    DmaHeapAllocator          allocator_;
    BoundedQueue<VideoFramePtr> queue_;
    std::thread               capture_thread_;
    std::mutex                state_mutex_;
    std::atomic<bool>         stop_requested_{false};
    std::atomic<std::uint64_t> sequence_{0};

    int                       fd_              = -1;
    bool                      stream_on_       = false;
    std::uint32_t             active_width_    = 0;
    std::uint32_t             active_height_   = 0;
    std::uint32_t             active_stride_   = 0;
    std::uint32_t             active_sizeimage_ = 0;
    std::vector<std::shared_ptr<PoolSlot>> slots_;
    std::shared_ptr<RequeueChannel>        channel_;
};

V4l2Camera::V4l2Camera(V4l2CameraConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

V4l2Camera::~V4l2Camera() = default;

const char* V4l2Camera::Name() const
{
    return impl_->Name();
}

const FrameMeta& V4l2Camera::NominalMeta() const
{
    return impl_->NominalMeta();
}

Status V4l2Camera::Start()
{
    return impl_->Start();
}

void V4l2Camera::Stop()
{
    impl_->Stop();
}

Status V4l2Camera::WaitForFrame(VideoFramePtr* out, std::chrono::milliseconds timeout)
{
    return impl_->WaitForFrame(out, timeout);
}

}  // namespace animal_monitor
