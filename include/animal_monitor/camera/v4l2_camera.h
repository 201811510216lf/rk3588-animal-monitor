// 文件名：include/animal_monitor/camera/v4l2_camera.h
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_ref_RKMediaProject.md 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject（V4L2_Camera），未逐行拷贝。
// 说明：本头文件不引入任何 Linux/V4L2 类型（pimpl 隔离），仅当
//       ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与构建。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "animal_monitor/camera/video_source.h"
#include "animal_monitor/common/frame_meta.h"

namespace animal_monitor {

// V4L2 采集配置：设备路径/分辨率/帧率全部参数化（AGENTS：禁止硬编码板级配置）。
struct V4l2CameraConfig
{
    std::string   device        = "/dev/video0";
    std::uint32_t width         = 1920;
    std::uint32_t height        = 1080;
    std::uint32_t fps           = 30;
    // 仅支持 kNv12 / kMjpeg；S_FMT 后以驱动协商结果为准（NominalMeta 返回实际值）。
    PixelFormat   pixel_format  = PixelFormat::kNv12;
    // 0 = 尝试切换自动曝光（控制缺失仅告警不致命，MIPI ISP 相机常无该控制）；
    // >0 = 手动曝光，单位 ms（通过 V4L2_CID_EXPOSURE_ABSOLUTE，100us 单位换算）。
    std::int32_t  exposure_ms   = 0;
    // V4L2_MEMORY_DMABUF 缓冲池数量（>=2；驱动可能批复更少）。
    std::uint32_t buffer_count  = 8;
    std::string   dma_heap_path = "/dev/dma_heap/system";
    // 帧有界队列容量（满时丢弃最旧帧，防止延迟累积；设计依据：AGENTS 有界队列规则）。
    std::size_t   queue_capacity = 4;
};

// V4L2 相机采集源：V4L2_MEMORY_DMABUF + dma-heap 池 + 采集线程（poll + DQBUF）。
// 生命周期契约（设计依据：docs/architecture/buffer_lifecycle.md）：
//  - 帧的 BufferOwner 析构即归还缓冲（SYNC_END + QBUF），下游持有时不得归还；
//  - Start 失败完整回滚；Stop 幂等并唤醒 WaitForFrame；
//  - 析构顺序：采集线程 join → STREAMOFF → REQBUFS(0) → close。
// 线程模型：采集线程唯一，WaitForFrame 可由多线程并发调用（内部有界队列线程安全）。
class V4l2Camera : public VideoSource
{
  public:
    explicit V4l2Camera(V4l2CameraConfig config);
    ~V4l2Camera() override;

    V4l2Camera(const V4l2Camera&)            = delete;
    V4l2Camera& operator=(const V4l2Camera&) = delete;

    const char*        Name() const override;
    // 名义输出格式：Start 前为配置估算值，Start 后为驱动协商结果（G_FMT 读回）。
    const FrameMeta&   NominalMeta() const override;

    Status Start() override;
    void   Stop() override;

    // 取一帧：成功 kOk；超时 kTimeout（非错误）；关停后 kCancelled。
    Status WaitForFrame(VideoFramePtr* out, std::chrono::milliseconds timeout) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
