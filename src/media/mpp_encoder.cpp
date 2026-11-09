// 文件名：src/media/mpp_encoder.cpp
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_B_*.md 审计结论重写；
//       API 形状参考 B lib/mpp_encoder.cpp 与 .ref_audit_RKMediaProject/src/rkmppenc.cpp
//       （只读审计），未逐行拷贝。
// 修复的审计缺陷：
//  - 输入 stride 统一 16 对齐（B#14：stride 未对齐、每帧 malloc 均修复为常驻池 + 行内拷贝）；
//  - PTS 由输入帧 timestamps 换算 us 写入（B#20：编码侧不设 PTS 修复）；
//  - 关键帧按 packet meta（KEY_OUTPUT_INTRA）+ Annex-B NALU 类型双重判定；
//  - GetHeader 首帧编码后可用且可重取（B#20：GetHeader 时机过早修复）；
//  - 缓冲复用：常驻 internal DRM 池（mpi_enc_test 简单 API 流程：
//    encode_put_frame 后 MPP 持有帧引用，消费完自动归还缓冲组；
//    mpp_buffer_get 在池耗尽时内部阻塞，即天然的背压/消费信号）。
// 头文件布局：与多路视频/mpp/include（扁平布局）对应，由 CMake 提供 include 路径。
#include "animal_monitor/media/mpp_encoder.h"

#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_venc_cfg.h"
#include "rk_venc_cmd.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "animal_monitor/logging/logger.h"

namespace animal_monitor
{

namespace
{

constexpr RK_S64      kMppIOTimeoutMs    = 200;  // encode_get_packet 内部单次等待上限
constexpr std::size_t kInputBufferCount  = 4;    // 常驻输入缓冲池（缓冲复用）
constexpr std::size_t kHeaderBufferCount = 1;    // SPS/PPS 头缓冲

std::uint32_t Align16(std::uint32_t value) { return (value + 15u) / 16u * 16u; }

MppCodingType ToMppCodingType(VideoCodec codec)
{
    switch (codec)
    {
        case VideoCodec::kH265:
            return MPP_VIDEO_CodingHEVC;
        case VideoCodec::kH264:
            return MPP_VIDEO_CodingAVC;
        default:
            return MPP_VIDEO_CodingUnused;
    }
}

const char* MppErrorName(MPP_RET ret)
{
    switch (ret)
    {
        case MPP_OK:
            return "MPP_OK";
        case MPP_NOK:
            return "MPP_NOK";
        case MPP_ERR_TIMEOUT:
            return "MPP_ERR_TIMEOUT";
        case MPP_ERR_NOMEM:
            return "MPP_ERR_NOMEM";
        case MPP_ERR_VALUE:
            return "MPP_ERR_VALUE";
        default:
            return "MPP_ERR_?";
    }
}

Status MakeMppStatus(const char* operation, MPP_RET ret)
{
    return Status(StatusCode::kInternal, std::string("[MPP-ENC] ") + operation + " 失败: " +
                                             MppErrorName(ret) + " (" + std::to_string(ret) + ")");
}

}  // namespace

class MppEncoder::Impl
{
   public:
    ~Impl() { Close(); }

    Status Initialize(const EncoderConfig& config)
    {
        if (initialized_)
        {
            return Status(StatusCode::kAlreadyExists, "[MPP-ENC] 已初始化");
        }
        const Status validate = ValidateConfig(config);
        if (!validate)
        {
            return validate;
        }

        coding_      = ToMppCodingType(config.codec);
        width_       = config.width;
        height_      = config.height;
        hor_stride_  = (config.hor_stride > 0) ? Align16(config.hor_stride) : Align16(width_);
        ver_stride_  = (config.ver_stride > 0) ? Align16(config.ver_stride) : Align16(height_);
        fps_         = config.fps;
        bitrate_bps_ = config.bitrate_bps;
        gop_         = config.gop;
        frame_size_  = static_cast<std::size_t>(hor_stride_) * ver_stride_ * 3u / 2u;

        MPP_RET ret = mpp_create(&ctx_, &api_);
        if (ret != MPP_OK || ctx_ == nullptr || api_ == nullptr)
        {
            return MakeMppStatus("mpp_create", ret);
        }
        ret = mpp_init(ctx_, MPP_CTX_ENC, coding_);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("mpp_init", ret);
        }

        ret = mpp_enc_cfg_init(&enc_cfg_);
        if (ret != MPP_OK || enc_cfg_ == nullptr)
        {
            ReleaseAll();
            return MakeMppStatus("mpp_enc_cfg_init", ret);
        }
        ret = api_->control(ctx_, MPP_ENC_GET_CFG, enc_cfg_);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("MPP_ENC_GET_CFG", ret);
        }

        // 逐项写入并检查返回值（审计规则：未检查返回值禁止带入）。
        // 任一失败即回滚：SET_CFG 阶段失败会导致配置半写状态不可控。
        auto SetCfgS32 = [this](const char* name, RK_S32 value) -> Status
        {
            const MPP_RET set_ret = mpp_enc_cfg_set_s32(enc_cfg_, name, value);
            if (set_ret != MPP_OK)
            {
                return MakeMppStatus(name, set_ret);
            }
            return Status::MakeOk();
        };

        Status set_status = Status::MakeOk();

        // ---- 码率控制：CBR、固定帧率、GOP ----
        set_status = SetCfgS32("rc:mode", MPP_ENC_RC_MODE_CBR);
        if (set_status)
        {
            set_status = SetCfgS32("rc:fps_in_num", static_cast<RK_S32>(fps_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("rc:fps_in_denorm", 1);
        }
        if (set_status)
        {
            set_status = SetCfgS32("rc:fps_out_num", static_cast<RK_S32>(fps_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("rc:fps_out_denorm", 1);
        }
        if (set_status)
        {
            set_status = SetCfgS32("rc:gop", static_cast<RK_S32>(gop_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("rc:bps_target", static_cast<RK_S32>(bitrate_bps_));
        }

        // ---- 输入控制：分辨率/stride（16 对齐）/格式 NV12 ----
        if (set_status)
        {
            set_status = SetCfgS32("prep:width", static_cast<RK_S32>(width_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("prep:height", static_cast<RK_S32>(height_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("prep:hor_stride", static_cast<RK_S32>(hor_stride_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("prep:ver_stride", static_cast<RK_S32>(ver_stride_));
        }
        if (set_status)
        {
            set_status = SetCfgS32("prep:format", MPP_FMT_YUV420SP);
        }

        // ---- 协议控制 ----
        if (set_status)
        {
            set_status = SetCfgS32("codec:type", static_cast<RK_S32>(coding_));
        }
        if (set_status && coding_ == MPP_VIDEO_CodingAVC)
        {
            set_status = SetCfgS32("h264:profile", 100);  // High Profile
            if (set_status)
            {
                set_status = SetCfgS32("h264:level", PickH264Level());
            }
            if (set_status)
            {
                set_status = SetCfgS32("h264:cabac_en", 1);
            }
        }

        // split NONE：一帧对应一个输出包（任务书要求），输出为 Annex-B。
        if (set_status)
        {
            set_status = SetCfgS32("split:mode", MPP_ENC_SPLIT_NONE);
        }
        if (set_status)
        {
            set_status = SetCfgS32("split:arg", 0);
        }
        if (set_status)
        {
            set_status = SetCfgS32("split:out", 0);
        }

        if (!set_status)
        {
            ReleaseAll();
            return set_status;
        }

        ret = api_->control(ctx_, MPP_ENC_SET_CFG, enc_cfg_);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("MPP_ENC_SET_CFG", ret);
        }

        // 每个 IDR 都携带 SPS/PPS（保证关键帧独立可解）。
        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
        ret                          = api_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("MPP_ENC_SET_HEADER_MODE", ret);
        }

        RK_S64 timeout_ms = kMppIOTimeoutMs;
        ret               = api_->control(ctx_, MPP_SET_INPUT_TIMEOUT, &timeout_ms);
        if (ret == MPP_OK)
        {
            ret = api_->control(ctx_, MPP_SET_OUTPUT_TIMEOUT, &timeout_ms);
        }
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("设置 IO 超时", ret);
        }

        // ---- 常驻缓冲池（缓冲复用，修复每帧 malloc 缺陷）----
        ret = mpp_buffer_group_get_internal(&input_group_, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("创建输入缓冲组", ret);
        }
        ret = mpp_buffer_group_limit_config(input_group_, frame_size_,
                                            static_cast<RK_S32>(kInputBufferCount));
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("配置输入缓冲组", ret);
        }

        // SPS/PPS 头缓冲组：按单帧上界预留。
        ret = mpp_buffer_group_get_internal(&header_group_, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("创建头缓冲组", ret);
        }
        ret = mpp_buffer_group_limit_config(header_group_, frame_size_,
                                            static_cast<RK_S32>(kHeaderBufferCount));
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("配置头缓冲组", ret);
        }
        ret = mpp_buffer_get(header_group_, &header_buf_, frame_size_);
        if (ret != MPP_OK || header_buf_ == nullptr)
        {
            ReleaseAll();
            return MakeMppStatus("获取头缓冲", ret);
        }

        stop_requested_.store(false, std::memory_order_relaxed);
        initialized_ = true;
        AM_LOG_INFO() << "[MPP-ENC] 初始化: " << width_ << "x" << height_ << " stride("
                      << hor_stride_ << "," << ver_stride_ << ") fps=" << fps_
                      << " bitrate=" << bitrate_bps_ << " gop=" << gop_;
        return Status::MakeOk();
    }

    Status Encode(const VideoFrame& frame)
    {
        if (!initialized_)
        {
            return Status(StatusCode::kConfigurationError, "[MPP-ENC] 未初始化");
        }
        if (stop_requested_.load(std::memory_order_relaxed))
        {
            return Status(StatusCode::kCancelled, "[MPP-ENC] 已关闭");
        }
        if (frame.meta.format != PixelFormat::kNv12)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] 输入必须为 NV12");
        }
        if (frame.meta.width != width_ || frame.meta.height != height_)
        {
            return Status(StatusCode::kInvalidArgument,
                          "[MPP-ENC] 输入分辨率与配置不符: " + std::to_string(frame.meta.width) +
                              "x" + std::to_string(frame.meta.height) +
                              " != " + std::to_string(width_) + "x" + std::to_string(height_));
        }
        if (frame.planes.empty() || frame.planes[0].data == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] 输入帧无平面数据");
        }

        // 获取常驻输入缓冲：耗尽时 mpp_buffer_get 内部阻塞（有界背压）。
        MppBuffer buffer = nullptr;
        MPP_RET   ret    = mpp_buffer_get(input_group_, &buffer, frame_size_);
        if (ret != MPP_OK || buffer == nullptr)
        {
            return MakeMppStatus("获取输入缓冲", ret);
        }

        const Status copy_status = CopyNv12IntoBuffer(frame, buffer);
        if (!copy_status)
        {
            mpp_buffer_put(buffer);
            return copy_status;
        }

        MppFrame input_frame = nullptr;
        ret                  = mpp_frame_init(&input_frame);
        if (ret != MPP_OK || input_frame == nullptr)
        {
            mpp_buffer_put(buffer);
            return MakeMppStatus("mpp_frame_init", ret);
        }
        mpp_frame_set_width(input_frame, width_);
        mpp_frame_set_height(input_frame, height_);
        mpp_frame_set_hor_stride(input_frame, hor_stride_);
        mpp_frame_set_ver_stride(input_frame, ver_stride_);
        mpp_frame_set_fmt(input_frame, MPP_FMT_YUV420SP);
        mpp_frame_set_buffer(input_frame, buffer);

        // PTS：输入帧单调时间戳换算 us；缺失时按帧率回退（单调递增）。
        std::int64_t pts_us = frame.meta.timestamps.monotonic_ns / 1000LL;
        if (pts_us <= 0)
        {
            pts_us = next_fallback_pts_us_;
            next_fallback_pts_us_ += 1'000'000LL / static_cast<std::int64_t>(fps_);
        }
        mpp_frame_set_pts(input_frame, pts_us);
        mpp_frame_set_dts(input_frame, pts_us);

        ret = api_->encode_put_frame(ctx_, input_frame);
        mpp_frame_deinit(&input_frame);
        if (ret != MPP_OK)
        {
            // 注意：put_frame 失败时 MPP 未持有帧引用，mpp_frame_deinit 已连带归还缓冲，
            // 此处不得再 mpp_buffer_put（避免双重释放，按 mpi_enc_test 流程）。
            return MakeMppStatus("encode_put_frame", ret);
        }
        // 成功后 MPP 持有帧/缓冲引用：编码消费完成时自动归还缓冲组，
        // 下一次 mpp_buffer_get 在池耗尽时阻塞等待（背压与“已消费”信号）。
        // 输入帧数据已拷贝进内部缓冲，返回后调用方可安全归还其底层缓冲。

        ++submitted_frames_;
        return Status::MakeOk();
    }

    Status GetPacket(EncodedPacket* out, std::chrono::milliseconds timeout)
    {
        if (!initialized_)
        {
            return Status(StatusCode::kConfigurationError, "[MPP-ENC] 未初始化");
        }
        if (out == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] out 为空");
        }

        const auto start = std::chrono::steady_clock::now();
        while (!stop_requested_.load(std::memory_order_relaxed))
        {
            MppPacket     packet = nullptr;
            const MPP_RET ret    = api_->encode_get_packet(ctx_, &packet);
            if (ret == MPP_OK && packet != nullptr)
            {
                void*  ptr = mpp_packet_get_pos(packet);
                size_t len = mpp_packet_get_length(packet);
                if (ptr != nullptr && len > 0)
                {
                    out->data.assign(static_cast<const std::uint8_t*>(ptr),
                                     static_cast<const std::uint8_t*>(ptr) + len);
                    out->pts_us    = mpp_packet_get_pts(packet);
                    out->dts_us    = mpp_packet_get_dts(packet);
                    out->key_frame = IsKeyFramePacket(packet);
                }
                mpp_packet_deinit(&packet);
                if (out->data.empty())
                {
                    return Status(StatusCode::kInternal, "[MPP-ENC] 输出包为空");
                }
                return Status::MakeOk();
            }
            if (ret != MPP_ERR_TIMEOUT && ret != MPP_OK)
            {
                return MakeMppStatus("encode_get_packet", ret);
            }
            if (std::chrono::steady_clock::now() - start >= timeout)
            {
                return Status(StatusCode::kTimeout, "[MPP-ENC] 取包超时");
            }
        }
        return Status(StatusCode::kCancelled, "[MPP-ENC] 已关闭");
    }

    Status GetHeader(EncodedPacket* out)
    {
        if (!initialized_)
        {
            return Status(StatusCode::kConfigurationError, "[MPP-ENC] 未初始化");
        }
        if (out == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] out 为空");
        }
        // 审计缺陷修复：首帧编码前取头可能为空；要求至少一次 Encode 成功后调用。
        if (submitted_frames_ == 0)
        {
            return Status(StatusCode::kUnavailable,
                          "[MPP-ENC] SPS/PPS 尚未生成：请至少 Encode 一帧后再取头");
        }

        MppPacket packet = nullptr;
        MPP_RET   ret    = mpp_packet_init_with_buffer(&packet, header_buf_);
        if (ret != MPP_OK || packet == nullptr)
        {
            return MakeMppStatus("mpp_packet_init_with_buffer", ret);
        }
        mpp_packet_set_length(packet, 0);

        ret = api_->control(ctx_, MPP_ENC_GET_HDR_SYNC, packet);
        if (ret != MPP_OK)
        {
            mpp_packet_deinit(&packet);
            return MakeMppStatus("MPP_ENC_GET_HDR_SYNC", ret);
        }

        void*  ptr = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);
        if (ptr == nullptr || len == 0)
        {
            mpp_packet_deinit(&packet);
            return Status(StatusCode::kUnavailable, "[MPP-ENC] 编码头为空");
        }
        out->data.assign(static_cast<const std::uint8_t*>(ptr),
                         static_cast<const std::uint8_t*>(ptr) + len);
        mpp_packet_deinit(&packet);

        out->pts_us    = 0;
        out->dts_us    = 0;
        out->key_frame = true;  // extradata 语义标记
        return Status::MakeOk();
    }

    void Close()
    {
        stop_requested_.store(true, std::memory_order_relaxed);
        if (!initialized_)
        {
            return;
        }
        ReleaseAll();
        initialized_ = false;
    }

   private:
    Status ValidateConfig(const EncoderConfig& config) const
    {
        if (config.width == 0 || config.height == 0 || config.fps == 0)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] width/height/fps 必须非零");
        }
        if (config.bitrate_bps <= 0 || config.gop == 0)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] bitrate/gop 必须非零");
        }
        if (config.codec != VideoCodec::kH264 && config.codec != VideoCodec::kH265)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] codec 仅支持 kH264 / kH265");
        }
        if (config.input_format != PixelFormat::kNv12)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] 输入格式仅支持 kNv12");
        }
        return Status::MakeOk();
    }

    // H.264 level 按像素量选取（合成双画面 3840x1080 需 level 5.0+，1080p 用 4.0）。
    RK_S32 PickH264Level() const
    {
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(height_);
        if (pixels <= 1920ULL * 1080ULL)
        {
            return 40;
        }
        if (pixels <= 2560ULL * 1440ULL)
        {
            return 50;
        }
        return 51;
    }

    // 行内拷贝：按 meta.stride（或平面 row_stride）逐行复制有效宽度，
    // 忽略行内 padding（审计缺陷：stride 未对齐导致花屏/越界）。
    Status CopyNv12IntoBuffer(const VideoFrame& frame, MppBuffer buffer)
    {
        void* dst_ptr = mpp_buffer_get_ptr(buffer);
        if (dst_ptr == nullptr)
        {
            return Status(StatusCode::kInternal, "[MPP-ENC] 输入缓冲无映射地址");
        }

        const std::uint32_t src_stride = (frame.meta.stride > 0) ? frame.meta.stride : width_;
        const PlaneView&    y_plane    = frame.planes[0];
        const std::uint32_t y_row_stride =
            (y_plane.row_stride > 0) ? y_plane.row_stride : src_stride;
        const std::uint8_t* uv_src        = nullptr;
        std::uint32_t       uv_row_stride = 0;
        if (frame.planes.size() > 1 && frame.planes[1].data != nullptr)
        {
            uv_src = frame.planes[1].data;
            uv_row_stride =
                (frame.planes[1].row_stride > 0) ? frame.planes[1].row_stride : src_stride;
        }
        else
        {
            // NV12 单平面视图：UV 紧随 Y（高度 × 行步长之后）。
            uv_src        = y_plane.data + static_cast<std::size_t>(y_row_stride) * height_;
            uv_row_stride = y_row_stride;
        }

        if (y_row_stride < width_ || uv_row_stride < width_)
        {
            return Status(StatusCode::kInvalidArgument,
                          "[MPP-ENC] 输入行步长小于宽度（stride 非法）");
        }
        const std::size_t required = static_cast<std::size_t>(y_row_stride) * height_ * 3u / 2u;
        if (y_plane.size > 0 && y_plane.size < required)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-ENC] 输入平面容量不足");
        }

        std::uint8_t* dst = static_cast<std::uint8_t*>(dst_ptr);
        for (std::uint32_t row = 0; row < height_; ++row)
        {
            std::memcpy(dst + static_cast<std::size_t>(row) * hor_stride_,
                        y_plane.data + static_cast<std::size_t>(row) * y_row_stride, width_);
        }
        std::uint8_t* dst_uv = dst + static_cast<std::size_t>(hor_stride_) * ver_stride_;
        for (std::uint32_t row = 0; row < height_ / 2u; ++row)
        {
            std::memcpy(dst_uv + static_cast<std::size_t>(row) * hor_stride_,
                        uv_src + static_cast<std::size_t>(row) * uv_row_stride, width_);
        }
        return Status::MakeOk();
    }

    // 关键帧判定：优先 packet meta KEY_OUTPUT_INTRA；缺失时回退 Annex-B NALU 类型。
    bool IsKeyFramePacket(MppPacket packet) const
    {
        if (mpp_packet_has_meta(packet) != 0)
        {
            MppMeta meta     = mpp_packet_get_meta(packet);
            RK_S32  is_intra = 0;
            if (meta != nullptr && mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra) == MPP_OK)
            {
                return is_intra != 0;
            }
        }

        void*               ptr   = mpp_packet_get_pos(packet);
        size_t              len   = mpp_packet_get_length(packet);
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(ptr);
        // 跳过 Annex-B 起始码（3/4 字节）。
        if (len >= 5 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1)
        {
            bytes += 4;
            len -= 4;
        }
        else if (len >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1)
        {
            bytes += 3;
            len -= 3;
        }
        else
        {
            return false;
        }
        if (len == 0)
        {
            return false;
        }
        const std::uint32_t nal_type = bytes[0] & 0x1Fu;
        if (coding_ == MPP_VIDEO_CodingAVC)
        {
            return nal_type == 5u;  // H.264 IDR
        }
        if (coding_ == MPP_VIDEO_CodingHEVC)
        {
            return nal_type == 19u || nal_type == 20u;  // HEVC IDR_W_RADL / IDR_N_LP
        }
        return false;
    }

    void ReleaseAll()
    {
        if (header_buf_ != nullptr)
        {
            (void)mpp_buffer_put(header_buf_);
            header_buf_ = nullptr;
        }
        if (header_group_ != nullptr)
        {
            (void)mpp_buffer_group_clear(header_group_);
            (void)mpp_buffer_group_put(header_group_);
            header_group_ = nullptr;
        }
        if (input_group_ != nullptr)
        {
            (void)mpp_buffer_group_clear(input_group_);
            (void)mpp_buffer_group_put(input_group_);
            input_group_ = nullptr;
        }
        if (enc_cfg_ != nullptr)
        {
            (void)mpp_enc_cfg_deinit(enc_cfg_);
            enc_cfg_ = nullptr;
        }
        if (ctx_ != nullptr)
        {
            (void)mpp_destroy(ctx_);
            ctx_ = nullptr;
            api_ = nullptr;
        }
        submitted_frames_ = 0;
    }

    MppCtx            ctx_                  = nullptr;
    MppApi*           api_                  = nullptr;
    MppEncCfg         enc_cfg_              = nullptr;
    MppBufferGroup    input_group_          = nullptr;
    MppBufferGroup    header_group_         = nullptr;
    MppBuffer         header_buf_           = nullptr;
    MppCodingType     coding_               = MPP_VIDEO_CodingAVC;
    std::uint32_t     width_                = 0;
    std::uint32_t     height_               = 0;
    std::uint32_t     hor_stride_           = 0;
    std::uint32_t     ver_stride_           = 0;
    std::uint32_t     fps_                  = 0;
    std::uint32_t     gop_                  = 0;
    std::int64_t      bitrate_bps_          = 0;
    std::size_t       frame_size_           = 0;
    std::uint64_t     submitted_frames_     = 0;
    std::int64_t      next_fallback_pts_us_ = 0;
    std::atomic<bool> stop_requested_{false};
    bool              initialized_ = false;
};

MppEncoder::MppEncoder() : impl_(std::make_unique<Impl>()) {}

MppEncoder::~MppEncoder() = default;

Status MppEncoder::Initialize(const EncoderConfig& config) { return impl_->Initialize(config); }

Status MppEncoder::Encode(const VideoFrame& frame) { return impl_->Encode(frame); }

Status MppEncoder::GetPacket(EncodedPacket* out, std::chrono::milliseconds timeout)
{
    return impl_->GetPacket(out, timeout);
}

Status MppEncoder::GetHeader(EncodedPacket* out) { return impl_->GetHeader(out); }

void MppEncoder::Close() { impl_->Close(); }

}  // namespace animal_monitor
