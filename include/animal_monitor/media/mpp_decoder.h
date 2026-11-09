// 文件名：include/animal_monitor/media/mpp_decoder.h
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_{A,B}_*.md 审计结论重写；
//       API 形状参考 A/B 的 mpp_decoder 与 .ref_audit_RKMediaProject（MppDecInstance），未逐行拷贝。
// 修复的审计缺陷：
//  - A/B#3 局部变量遮蔽成员 → 本类 pimpl 唯一持有 MPP 上下文，无遮蔽路径；
//  - A/B#8 info-change 静默失败 → 全部返回 Status，绝不 break 吞错；
//  - 绝不调用 exit()；所有 MPP 返回值检查；
//  - 输出帧 BufferOwner 持 mpp_frame（析构 mpp_frame_deinit + mpp_buffer_put），stride 写入 meta。
// 说明：本头文件不引入任何 MPP 类型（pimpl 隔离）；仅当
//       ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与构建。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "animal_monitor/common/status.h"
#include "animal_monitor/media/video_encoder.h"  // VideoCodec
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor {

// MPP 硬解配置。width/height 预留：当前实现以码流 SPS（info-change）为准，
// 0 表示不预设（任务书：网络/文件输入源使用；V4L2 相机直出 NV12 时不需要解码器）。
struct MppDecoderConfig
{
    VideoCodec    codec = VideoCodec::kH264;  // 输入码流类型：kH264 / kH265
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// MPP 硬件解码器（H.264/HEVC，阻塞式单帧接口）。
// 标准流程（设计依据：source_audit.md §4.3，mpi_dec_test 流程）：
//   split_parse=1 → internal DRM group → MPP_DEC_SET_EXT_BUF_GROUP（limit 24）
//   → MPP_DEC_SET_INFO_CHANGE_READY；输出强制 NV12。
// 线程安全：非线程安全，Decode/Reset 由同一线程调用（内部无共享并发状态）。
class MppDecoder
{
  public:
    MppDecoder();
    ~MppDecoder();

    MppDecoder(const MppDecoder&)            = delete;
    MppDecoder& operator=(const MppDecoder&) = delete;

    // 初始化硬件上下文；重复调用返回 kAlreadyExists（对象生命周期内仅初始化一次）。
    Status Initialize(const MppDecoderConfig& config);

    // 送入一包码流并阻塞取出一帧（B 帧重排下内部缓存多余输出帧，下次 Decode 先排空）。
    // out 的 BufferOwner 持有 MPP frame/buffer：下游持有时不得调用 Reset/析构本对象。
    // 返回：kOk / kTimeout（时限内无输出帧）/ kInvalidArgument / kInternal / kHardwareUnavailable。
    Status Decode(const std::uint8_t* data, std::size_t size, std::int64_t pts_us,
                  VideoFramePtr* out);

    // 重置解码器：清空内部帧缓存与 info-change 状态（断流重连后调用）。
    void Reset();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
