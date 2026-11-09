// 文件名：include/animal_monitor/media/mpp_encoder.h
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_B_*.md 审计结论重写；
//       API 形状参考 B lib/mpp_encoder.cpp 与 .ref_audit_RKMediaProject（MppEncInstance），未逐行拷贝。
// 修复的审计缺陷：
//  - B#14 输入 stride 未 16 对齐 → 内部统一 16 对齐（config.hor_stride 传入时按 16 向上取整）；
//  - B#20 每帧 new 大缓冲不复用 → 常驻内部 DRM 缓冲池，缓冲复用；
//  - B#20 GetHeader 时机过早 → GetHeader 仅在至少一次 Encode 成功后可用，且可重复获取；
//  - 编码侧不设 PTS → 输入帧 timestamps 换算 us 写入 packet（缺失时按帧率回退单调递增）。
// 说明：本头文件不引入任何 MPP 类型（pimpl 隔离）；仅当
//       ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 时参与构建。
#pragma once

#include <chrono>
#include <memory>

#include "animal_monitor/media/video_encoder.h"

namespace animal_monitor {

// MPP 硬件编码器（H.264/H.265）。
// 配置：CBR、GOP、split=NONE、HEADER_MODE=EACH_IDR（每个 IDR 带 SPS/PPS）。
// Encode 阻塞语义：返回即视为编码器已消费输入帧（内部任务队列回收完成），
// 调用方可归还输入帧底层缓冲（video_encoder.h 契约）。
// 线程安全：非线程安全，Encode/GetPacket/GetHeader 由同一线程调用；
// Close() 可由其他线程调用以解除 GetPacket 的阻塞等待（幂等）。
class MppEncoder : public VideoEncoder
{
  public:
    MppEncoder();
    ~MppEncoder() override;

    MppEncoder(const MppEncoder&)            = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    Status Initialize(const EncoderConfig& config) override;

    // 送入一帧（NV12）；输入行内 padding 按 meta.stride 正确跳过。
    Status Encode(const VideoFrame& frame) override;

    // 取出一个 Annex-B 编码包；超时返回 kTimeout，Close 后返回 kCancelled。
    Status GetPacket(EncodedPacket* out, std::chrono::milliseconds timeout) override;

    // 取出 SPS/PPS 头信息（Annex-B）。首次 Encode 成功后才可调用，可重复获取。
    // out 的 pts/dts 置 0、key_frame 置 true（仅作为 extradata 语义标记）。
    Status GetHeader(EncodedPacket* out);

    // 解除 GetPacket/Encode 的阻塞等待（幂等；析构自动调用）。
    void Close();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
