// 文件名：include/animal_monitor/media/stream_publisher.h
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_ref_RKMediaProject.md
// 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject（ZlmPublisher），未逐行拷贝。
// 说明：ZLMediaKit C API（mk_*）头文件由构建系统提供 include 路径，
//       本仓库不拷贝 ZLM 头；头缺失仅在本文件参与构建
//       （ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON）时才会报编译错误。
// 端口/认证全部参数化（AGENTS：网络服务绑定可配置端口，无凭据硬编码）。
// 已知限制：WebRTC 播放所需的 /index/api/webrtc SDP 交换 HTTP 接口暂未接入
//           （planned），enable_webrtc 仅启动 RTC 服务器端口。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "mk_mediakit.h"

#include "animal_monitor/media/video_publisher.h"

namespace animal_monitor
{

// ZLMediaKit 内嵌发布器：mk_env_init → http/rtsp/(rtc) server → mk_media_create2
// → mk_track H264 → init_track/init_complete；FeedH264 拆 Annex-B NALU 逐条输入。
// 时间戳：接口输入已是 ms；内部强制单调（dts 严格递增、pts>=dts，缺失回退计数）。
// 线程安全：FeedH264/Close/RtspUrl 内部加锁，可跨线程调用。
class StreamPublisher : public VideoPublisher
{
   public:
    StreamPublisher();
    ~StreamPublisher() override;

    StreamPublisher(const StreamPublisher&)            = delete;
    StreamPublisher& operator=(const StreamPublisher&) = delete;

    Status Initialize(const Config& config) override;

    // 喂入 H.264 Annex-B 码流（dts_ms/pts_ms，负值视为缺失并回退）。
    Status FeedH264(const std::uint8_t* data, std::size_t size, std::int64_t dts_ms,
                    std::int64_t pts_ms) override;

    void Close() override;

    // RTSP 拉流地址（含 <设备IP> 占位符，端口为实际监听端口）。
    std::string RtspUrl() const override;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
