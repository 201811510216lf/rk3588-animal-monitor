// 文件名：src/media/stream_publisher.cpp
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_ref_RKMediaProject.md
// 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject/src/zlm_publisher.cpp（只读审计），未逐行拷贝。
// 与参考实现的差异（重写点）：
//  - 时间戳接口输入已是 ms（参考实现为 us 入参）：单调化逻辑在 ms 域重写，
//    保留“dts 严格递增、pts>=dts、缺失回退计数”思想（NormalizeTimestamp 重写）；
//  - 全部返回值检查、Close 幂等、无全局可变状态（参考实现的 gWebRtcServiceReady 全局已移除）；
//  - 端口冲突（rtc 与 http 同端口）自动避让（rtc_port + 1）。
#include "animal_monitor/media/stream_publisher.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include "animal_monitor/logging/logger.h"

namespace animal_monitor
{

namespace
{

constexpr const char*   kDefaultVhost           = "__defaultVhost__";
constexpr std::uint64_t kDefaultFrameIntervalMs = 33;  // 时间戳回退步长（~30fps）

}  // namespace

class StreamPublisher::Impl
{
   public:
    ~Impl() { Close(); }

    Status Initialize(const VideoPublisher::Config& config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_)
        {
            return Status(StatusCode::kAlreadyExists, "[ZLM] 已初始化");
        }

        config_ = config;

        // mk_env_init 为进程级初始化；重复调用可接受（幂等）。
        mk_config env_config{};
        env_config.thread_num    = 0;  // 0 = 自动
        env_config.log_level     = 0;
        env_config.log_mask      = LOG_CONSOLE;
        env_config.log_file_path = nullptr;
        env_config.log_file_days = 0;
        env_config.ini           = nullptr;
        env_config.ini_is_path   = 0;
        env_config.ssl           = nullptr;
        env_config.ssl_is_path   = 1;
        env_config.ssl_pwd       = nullptr;
        mk_env_init(&env_config);

        http_port_ = mk_http_server_start(config_.http_port, 0);
        if (http_port_ == 0)
        {
            RollbackLocked();
            return Status(
                StatusCode::kHardwareUnavailable,
                "[ZLM] HTTP server 启动失败（端口 " + std::to_string(config_.http_port) + "）");
        }
        rtsp_port_ = mk_rtsp_server_start(config_.rtsp_port, 0);
        if (rtsp_port_ == 0)
        {
            RollbackLocked();
            return Status(
                StatusCode::kHardwareUnavailable,
                "[ZLM] RTSP server 启动失败（端口 " + std::to_string(config_.rtsp_port) + "）");
        }

        if (config_.enable_webrtc)
        {
            std::uint16_t rtc_port = config_.rtc_port;
            if (rtc_port == http_port_)
            {
                rtc_port = (rtc_port < 65535u) ? static_cast<std::uint16_t>(rtc_port + 1u)
                                               : static_cast<std::uint16_t>(8001u);
                AM_LOG_WARNING() << "[ZLM] rtc_port 与 http_port 冲突，改用 " << rtc_port;
            }
            rtc_port_ = mk_rtc_server_start(rtc_port);
            if (rtc_port_ == 0)
            {
                // WebRTC 失败不致命：RTSP/HTTP 仍可用（单路降级，AGENTS 规则）。
                AM_LOG_WARNING() << "[ZLM] RTC server 启动失败，WebRTC 不可用";
            }
        }

        mk_ini media_option = mk_ini_create();
        if (media_option == nullptr)
        {
            RollbackLocked();
            return Status(StatusCode::kInternal, "[ZLM] mk_ini_create 失败");
        }
        mk_ini_set_option_int(media_option, "enable_rtsp", 1);
        mk_ini_set_option_int(media_option, "enable_rtmp", config_.enable_rtmp ? 1 : 0);
        mk_ini_set_option_int(media_option, "enable_hls", 0);
        mk_ini_set_option_int(media_option, "enable_hls_fmp4", 0);
        mk_ini_set_option_int(media_option, "enable_ts", 0);
        mk_ini_set_option_int(media_option, "enable_fmp4", 0);
        mk_ini_set_option_int(media_option, "enable_mp4", 0);
        mk_ini_set_option_int(media_option, "enable_audio", 0);

        media_ = mk_media_create2(kDefaultVhost, config_.app.c_str(), config_.stream.c_str(), 0.0f,
                                  media_option);
        mk_ini_release(media_option);
        if (media_ == nullptr)
        {
            RollbackLocked();
            return Status(StatusCode::kInternal, "[ZLM] mk_media_create2 失败（app=" + config_.app +
                                                     " stream=" + config_.stream + "）");
        }

        codec_args track_args{};
        mk_track   track = mk_track_create(MKCodecH264, &track_args);
        if (track == nullptr)
        {
            RollbackLocked();
            return Status(StatusCode::kInternal, "[ZLM] mk_track_create(H264) 失败");
        }
        mk_media_init_track(media_, track);
        mk_track_unref(track);
        mk_media_init_complete(media_);

        initialized_ = true;
        AM_LOG_INFO() << "[ZLM] 发布就绪: rtsp=" << rtsp_port_ << " http=" << http_port_
                      << " rtc=" << rtc_port_ << " app/stream=" << config_.app << "/"
                      << config_.stream;
        return Status::MakeOk();
    }

    Status FeedH264(const std::uint8_t* data, std::size_t size, std::int64_t dts_ms,
                    std::int64_t pts_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_ || media_ == nullptr)
        {
            return Status(StatusCode::kUnavailable, "[ZLM] 未初始化");
        }
        if (data == nullptr || size == 0)
        {
            return Status(StatusCode::kInvalidArgument, "[ZLM] 输入码流为空");
        }
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return Status(StatusCode::kInvalidArgument, "[ZLM] 单次输入超过 int 上限");
        }
        if (!IsAnnexBStartCode(data, size))
        {
            return Status(StatusCode::kInvalidArgument, "[ZLM] 输入非 Annex-B 码流（缺少起始码）");
        }

        // 时间戳单调化（ms 域；参考 NormalizeTimestamp 思想重写）。
        std::uint64_t       dts_out         = 0;
        std::uint64_t       pts_out         = 0;
        const std::uint64_t fallback_before = fallback_count_;
        NormalizeTimestampMs(dts_ms, pts_ms, &dts_out, &pts_out);
        if (fallback_count_ != fallback_before && fallback_count_ == 1)
        {
            AM_LOG_WARNING() << "[ZLM] 输入时间戳缺失/回退，已启用单调化回退（后续不再提示）";
        }

        // 拆 Annex-B NALU 逐条送入（mk_media_input_h264 按 NALU 处理）。
        std::vector<NaluRange> nalus;
        if (!SplitAnnexBNalus(data, size, &nalus) || nalus.empty())
        {
            return Status(StatusCode::kInvalidArgument, "[ZLM] Annex-B 拆分失败");
        }
        for (const NaluRange& nalu : nalus)
        {
            if (nalu.data == nullptr || nalu.len == 0)
            {
                return Status(StatusCode::kInternal, "[ZLM] 拆分出空 NALU");
            }
            const int ret = mk_media_input_h264(media_, nalu.data, static_cast<int>(nalu.len),
                                                dts_out, pts_out);
            if (ret == 0)
            {
                return Status(StatusCode::kInternal,
                              "[ZLM] mk_media_input_h264 失败（发布端已断开？）");
            }
        }
        return Status::MakeOk();
    }

    void Close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RollbackLocked();
        has_last_       = false;
        last_dts_ms_    = 0;
        fallback_count_ = 0;
    }

    std::string RtspUrl() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rtsp_port_ == 0)
        {
            return "";
        }
        return "rtsp://<设备IP>:" + std::to_string(rtsp_port_) + "/" + config_.app + "/" +
               config_.stream;
    }

   private:
    struct NaluRange
    {
        const std::uint8_t* data = nullptr;
        std::size_t         len  = 0;
    };

    // 失败/关停统一回收（幂等）：media → 所有 server → 状态复位。
    void RollbackLocked()
    {
        if (media_ != nullptr)
        {
            mk_media_release(media_);
            media_ = nullptr;
        }
        mk_stop_all_server();
        http_port_   = 0;
        rtsp_port_   = 0;
        rtc_port_    = 0;
        initialized_ = false;
    }

    // ms 域时间戳单调化：
    //  - 负值视为缺失：有历史则 +33ms 递增，无历史则从 0 开始；
    //  - dts 强制严格递增（>= last+1）；pts >= dts。
    void NormalizeTimestampMs(std::int64_t dts_in, std::int64_t pts_in, std::uint64_t* dts_out,
                              std::uint64_t* pts_out)
    {
        std::int64_t dts = dts_in;
        std::int64_t pts = pts_in;

        if (dts < 0 && pts >= 0)
        {
            dts = pts;
        }
        else if (pts < 0 && dts >= 0)
        {
            pts = dts;
        }
        if (dts < 0 || pts < 0)
        {
            if (!has_last_)
            {
                dts = 0;
                pts = 0;
            }
            else
            {
                dts = static_cast<std::int64_t>(last_dts_ms_ + kDefaultFrameIntervalMs);
                pts = dts;
            }
            ++fallback_count_;
        }
        if (has_last_ && static_cast<std::uint64_t>(dts) <= last_dts_ms_)
        {
            dts = static_cast<std::int64_t>(last_dts_ms_ + 1);
            ++fallback_count_;
        }
        if (pts < dts)
        {
            pts = dts;
        }

        last_dts_ms_ = static_cast<std::uint64_t>(dts);
        has_last_    = true;
        *dts_out     = last_dts_ms_;
        *pts_out     = static_cast<std::uint64_t>(pts);
    }

    bool IsAnnexBStartCode(const std::uint8_t* data, std::size_t size) const
    {
        if (data == nullptr || size < 3)
        {
            return false;
        }
        if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01)
        {
            return true;
        }
        return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01;
    }

    std::size_t FindAnnexBPrefix(const std::uint8_t* data, std::size_t size, std::size_t pos) const
    {
        if (pos + 4 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
            data[pos + 2] == 0x00 && data[pos + 3] == 0x01)
        {
            return 4;
        }
        if (pos + 3 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01)
        {
            return 3;
        }
        return 0;
    }

    // 扫描所有起始码，按“起始码到下一起始码”切分（含起始码本身）；
    // 每个 NALU 末尾的填充 0x00 剔除（参考实现保留，此处收紧）。
    bool SplitAnnexBNalus(const std::uint8_t* data, std::size_t size,
                          std::vector<NaluRange>* out) const
    {
        std::vector<std::size_t> starts;
        starts.reserve(16);
        for (std::size_t pos = 0; pos + 3 <= size;)
        {
            const std::size_t prefix = FindAnnexBPrefix(data, size, pos);
            if (prefix != 0)
            {
                starts.push_back(pos);
                pos += prefix;
            }
            else
            {
                ++pos;
            }
        }
        if (starts.empty() || starts[0] != 0)
        {
            return false;
        }

        for (std::size_t i = 0; i < starts.size(); ++i)
        {
            const std::size_t begin = starts[i];
            std::size_t       end   = (i + 1 < starts.size()) ? starts[i + 1] : size;
            // 剔除下一起始码前的 0x00 填充。
            while (end > begin && data[end - 1] == 0x00)
            {
                --end;
            }
            if (end > begin)
            {
                out->push_back(NaluRange{data + begin, end - begin});
            }
        }
        return !out->empty();
    }

    VideoPublisher::Config config_;
    mk_media               media_       = nullptr;
    std::uint16_t          http_port_   = 0;
    std::uint16_t          rtsp_port_   = 0;
    std::uint16_t          rtc_port_    = 0;
    bool                   initialized_ = false;

    // 时间戳单调化状态（仅 FeedH264 访问，由 mutex_ 保护）。
    bool          has_last_       = false;
    std::uint64_t last_dts_ms_    = 0;
    std::uint64_t fallback_count_ = 0;

    mutable std::mutex mutex_;
};

StreamPublisher::StreamPublisher() : impl_(std::make_unique<Impl>()) {}

StreamPublisher::~StreamPublisher() = default;

Status StreamPublisher::Initialize(const Config& config) { return impl_->Initialize(config); }

Status StreamPublisher::FeedH264(const std::uint8_t* data, std::size_t size, std::int64_t dts_ms,
                                 std::int64_t pts_ms)
{
    return impl_->FeedH264(data, size, dts_ms, pts_ms);
}

void StreamPublisher::Close() { impl_->Close(); }

std::string StreamPublisher::RtspUrl() const { return impl_->RtspUrl(); }

}  // namespace animal_monitor
