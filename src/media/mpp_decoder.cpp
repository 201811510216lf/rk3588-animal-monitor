// 文件名：src/media/mpp_decoder.cpp
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_{A,B}_*.md 审计结论重写；
//       API 形状参考 A/B lib/mpp_decoder.cpp 与 .ref_audit_RKMediaProject/src/rkmppdec.cpp
//       （只读审计），未逐行拷贝。
// 修复的审计缺陷：
//  - A/B#3 局部变量遮蔽成员：上下文由 Impl 成员唯一持有；
//  - A/B#8 info-change 失败静默 break：全部上抛 Status（kHardwareUnavailable）；
//  - 输出帧 BufferOwner 持 mpp_frame，析构 mpp_frame_deinit + mpp_buffer_put；
//  - stride 写入 meta（消费侧必须校验，AGENTS 禁止忽略 stride）；
//  - 绝不 exit()；所有 MPP 返回值检查。
// 头文件布局：与多路视频/mpp/include（扁平布局）对应，由 CMake 提供 include 路径。
#include "animal_monitor/media/mpp_decoder.h"

#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_vdec_cfg.h"

#include <chrono>
#include <cstring>
#include <string>
#include <time.h>
#include <utility>

#include "dma_heap_allocator.h"
#include "animal_monitor/logging/logger.h"

namespace animal_monitor
{

namespace
{

constexpr RK_S64      kMppIOTimeoutMs         = 500;       // MPP 内部输入/输出超时
constexpr std::size_t kOutputBufferCount      = 24;        // 输出池上限（mpi_dec_test 惯例）
constexpr std::size_t kInputBufferCount       = 4;         // 输入包缓冲池
constexpr std::size_t kMaxInputBufferSize     = 8u << 20;  // 单包输入上限（8MB）
constexpr auto        kDrainBeforeFeedTimeout = std::chrono::milliseconds(20);
constexpr auto        kDrainAfterFeedTimeout  = std::chrono::milliseconds(3000);

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

// 本 SDK 版本无 mpp_get_err_string，仅映射常见错误码，其余输出十进制。
const char* MppErrorName(MPP_RET ret)
{
    switch (ret)
    {
        case MPP_OK:
            return "MPP_OK";
        case MPP_NOK:
            return "MPP_NOK";
        case MPP_ERR_UNKNOW:
            return "MPP_ERR_UNKNOW";
        case MPP_ERR_NULL_PTR:
            return "MPP_ERR_NULL_PTR";
        case MPP_ERR_MALLOC:
            return "MPP_ERR_MALLOC";
        case MPP_ERR_OPEN_FILE:
            return "MPP_ERR_OPEN_FILE";
        case MPP_ERR_VALUE:
            return "MPP_ERR_VALUE";
        case MPP_ERR_TIMEOUT:
            return "MPP_ERR_TIMEOUT";
        case MPP_ERR_INIT:
            return "MPP_ERR_INIT";
        case MPP_ERR_STREAM:
            return "MPP_ERR_STREAM";
        case MPP_ERR_NOMEM:
            return "MPP_ERR_NOMEM";
        case MPP_ERR_PROTOL:
            return "MPP_ERR_PROTOL";
        case MPP_ERR_VPUHW:
            return "MPP_ERR_VPUHW";
        case MPP_EOS_STREAM_REACHED:
            return "MPP_EOS_STREAM_REACHED";
        case MPP_ERR_BUFFER_FULL:
            return "MPP_ERR_BUFFER_FULL";
        default:
            return "MPP_ERR_?";
    }
}

Status MakeMppStatus(const char* operation, MPP_RET ret)
{
    return Status(StatusCode::kInternal, std::string("[MPP-DEC] ") + operation + " 失败: " +
                                             MppErrorName(ret) + " (" + std::to_string(ret) + ")");
}

}  // namespace

// 输出帧所有权：持有 MppFrame 与其底层 MppBuffer 的独立引用。
// 析构 mpp_frame_deinit + mpp_buffer_put；下游持有时缓冲不回池
// （设计依据：docs/architecture/buffer_lifecycle.md）。
class MppFrameOwner : public FdBufferOwner
{
   public:
    explicit MppFrameOwner(MppFrame frame) : frame_(frame), buffer_(mpp_frame_get_buffer(frame))
    {
        if (buffer_ != nullptr)
        {
            mpp_buffer_inc_ref(buffer_);  // 独立持有一份引用，frame 释放后缓冲仍有效
        }
    }

    ~MppFrameOwner() override
    {
        if (frame_ != nullptr)
        {
            mpp_frame_deinit(&frame_);
            frame_ = nullptr;
        }
        if (buffer_ != nullptr)
        {
            mpp_buffer_put(buffer_);
            buffer_ = nullptr;
        }
    }

    MppFrameOwner(const MppFrameOwner&)            = delete;
    MppFrameOwner& operator=(const MppFrameOwner&) = delete;

    int         Fd() const override { return buffer_ != nullptr ? mpp_buffer_get_fd(buffer_) : -1; }
    std::size_t Size() const override
    {
        return buffer_ != nullptr ? mpp_buffer_get_size(buffer_) : 0;
    }

   private:
    MppFrame  frame_  = nullptr;
    MppBuffer buffer_ = nullptr;
};

class MppDecoder::Impl
{
   public:
    ~Impl() { ReleaseAll(); }

    Status Initialize(const MppDecoderConfig& config)
    {
        if (initialized_)
        {
            return Status(StatusCode::kAlreadyExists, "[MPP-DEC] 已初始化");
        }

        const MppCodingType coding = ToMppCodingType(config.codec);
        if (coding == MPP_VIDEO_CodingUnused)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-DEC] codec 仅支持 kH264 / kH265");
        }

        MPP_RET ret = mpp_create(&ctx_, &api_);
        if (ret != MPP_OK || ctx_ == nullptr || api_ == nullptr)
        {
            return MakeMppStatus("mpp_create", ret);
        }
        ret = mpp_init(ctx_, MPP_CTX_DEC, coding);
        if (ret != MPP_OK)
        {
            ReleaseAll();  // mpp_init 失败后 ctx_ 仍需 mpp_destroy 回收
            return MakeMppStatus("mpp_init", ret);
        }

        // split_parse=1：送入一包不完整数据也能尽快吐出一帧（参考 A/B 框架）。
        MppDecCfg dec_cfg = nullptr;
        ret               = mpp_dec_cfg_init(&dec_cfg);
        if (ret == MPP_OK)
        {
            ret = api_->control(ctx_, MPP_DEC_GET_CFG, dec_cfg);
        }
        if (ret == MPP_OK)
        {
            ret = mpp_dec_cfg_set_u32(dec_cfg, "base:split_parse", 1);
        }
        if (ret == MPP_OK)
        {
            ret = api_->control(ctx_, MPP_DEC_SET_CFG, dec_cfg);
        }
        if (dec_cfg != nullptr)
        {
            mpp_dec_cfg_deinit(dec_cfg);
        }
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("配置 split_parse", ret);
        }

        // 输出强制 NV12，便于下游（RGA/编码器）统一处理。
        RK_U32 output_format = MPP_FMT_YUV420SP;
        ret                  = api_->control(ctx_, MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("设置输出格式 NV12", ret);
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

        // 输入包缓冲池：internal DRM group，mpp_buffer_get 在耗尽时阻塞（天然背压）。
        ret = mpp_buffer_group_get_internal(&input_group_, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("创建输入缓冲组", ret);
        }
        ret = mpp_buffer_group_limit_config(input_group_, kMaxInputBufferSize,
                                            static_cast<RK_S32>(kInputBufferCount));
        if (ret != MPP_OK)
        {
            ReleaseAll();
            return MakeMppStatus("配置输入缓冲组", ret);
        }

        initialized_ = true;
        return Status::MakeOk();
    }

    Status Decode(const std::uint8_t* data, std::size_t size, std::int64_t pts_us,
                  VideoFramePtr* out)
    {
        if (!initialized_)
        {
            return Status(StatusCode::kConfigurationError, "[MPP-DEC] 未初始化");
        }
        if (data == nullptr || size == 0 || out == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-DEC] 输入参数无效");
        }
        if (size > kMaxInputBufferSize)
        {
            return Status(StatusCode::kInvalidArgument, "[MPP-DEC] 单包超过 8MB 上限");
        }

        // 1) 先排空上一包可能残留的输出帧（B 帧重排下可多帧输出）。
        Status status = DrainFrames(kDrainBeforeFeedTimeout, out);
        if (status.Ok())
        {
            return status;
        }
        if (status.Code() != StatusCode::kTimeout)
        {
            return status;
        }

        // 2) 送包：输入数据拷入内部缓冲（阻塞式单帧接口，包数据无需调用方保活）。
        status = FeedPacket(data, size, pts_us);
        if (!status)
        {
            return status;
        }

        // 3) 阻塞取一帧。
        return DrainFrames(kDrainAfterFeedTimeout, out);
    }

    void Reset()
    {
        if (ctx_ != nullptr && api_ != nullptr)
        {
            (void)api_->reset(ctx_);
        }
        if (output_group_ != nullptr)
        {
            (void)mpp_buffer_group_clear(output_group_);
        }
        info_change_done_ = false;
        width_            = 0;
        height_           = 0;
        hor_stride_       = 0;
        ver_stride_       = 0;
    }

   private:
    Status FeedPacket(const std::uint8_t* data, std::size_t size, std::int64_t pts_us)
    {
        MppBuffer buffer = nullptr;
        MPP_RET   ret    = mpp_buffer_get(input_group_, &buffer, size);
        if (ret != MPP_OK || buffer == nullptr)
        {
            return MakeMppStatus("获取输入缓冲", ret);
        }

        // mpp_buffer_write 的 data 参数为 void*（C 接口），const_cast 仅用于适配签名。
        ret = mpp_buffer_write(buffer, 0, const_cast<std::uint8_t*>(data), size);
        if (ret != MPP_OK)
        {
            mpp_buffer_put(buffer);
            return MakeMppStatus("写入输入缓冲", ret);
        }

        MppPacket packet = nullptr;
        ret              = mpp_packet_init_with_buffer(&packet, buffer);
        if (ret != MPP_OK || packet == nullptr)
        {
            mpp_buffer_put(buffer);
            return MakeMppStatus("创建输入包", ret);
        }
        mpp_buffer_put(buffer);  // 引用已转移给 packet

        mpp_packet_set_pts(packet, pts_us);
        mpp_packet_set_dts(packet, pts_us);

        ret = api_->decode_put_packet(ctx_, packet);
        // 按 mpi_dec_test 流程：put 后立即 deinit（MPP 内部持有引用）。
        mpp_packet_deinit(&packet);
        if (ret != MPP_OK)
        {
            return MakeMppStatus("decode_put_packet", ret);
        }
        return Status::MakeOk();
    }

    // 在 deadline 内取出一帧；无帧返回 kTimeout。
    // 一包可能解码出多帧（B 帧重排）：多余帧留在 MPP 内部队列，
    // 下一次 Decode 的 DrainFrames 快路径（20ms）先排空再送新包。
    Status DrainFrames(std::chrono::milliseconds deadline, VideoFramePtr* out)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < deadline)
        {
            MppFrame frame = nullptr;
            MPP_RET  ret   = api_->decode_get_frame(ctx_, &frame);
            if (ret == MPP_ERR_TIMEOUT)
            {
                continue;  // 输出超时由 MPP_SET_OUTPUT_TIMEOUT 控制，循环直到 deadline
            }
            if (ret != MPP_OK)
            {
                return MakeMppStatus("decode_get_frame", ret);
            }
            if (frame == nullptr)
            {
                continue;
            }

            if (mpp_frame_get_info_change(frame))
            {
                // info-change：重建输出缓冲组并 READY；失败必须上抛（审计缺陷 #8）。
                const Status status = HandleInfoChange(frame);
                mpp_frame_deinit(&frame);
                if (!status)
                {
                    return status;
                }
                continue;
            }
            if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame))
            {
                mpp_frame_deinit(&frame);
                continue;  // 解码错误/丢弃帧：跳过（统计由上层决定）
            }

            const Status status = BuildOutputFrame(frame, out);
            if (status.Ok())
            {
                return status;  // frame 已移交给 owner，不得再 deinit
            }
            mpp_frame_deinit(&frame);
            return status;
        }
        return Status(StatusCode::kTimeout, "[MPP-DEC] 时限内无输出帧");
    }

    Status HandleInfoChange(MppFrame frame)
    {
        const RK_U32 width      = mpp_frame_get_width(frame);
        const RK_U32 height     = mpp_frame_get_height(frame);
        const RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
        const RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
        if (width == 0 || height == 0 || hor_stride == 0 || ver_stride == 0)
        {
            return Status(StatusCode::kInternal, "[MPP-DEC] info-change 几何参数非法");
        }

        MPP_RET ret = MPP_OK;
        if (output_group_ == nullptr)
        {
            ret = mpp_buffer_group_get_internal(&output_group_, MPP_BUFFER_TYPE_DRM);
            if (ret != MPP_OK)
            {
                return MakeMppStatus("创建输出缓冲组", ret);
            }
        }
        else if (info_change_done_)
        {
            // 分辨率变更：先清空旧缓冲再重新配置（阻塞式单帧模式下无在途输出帧）。
            ret = mpp_buffer_group_clear(output_group_);
            if (ret != MPP_OK)
            {
                return MakeMppStatus("清空输出缓冲组", ret);
            }
        }

        std::size_t buffer_size = mpp_frame_get_buf_size(frame);
        if (buffer_size == 0)
        {
            // 部分版本 buf_size 为 0：按 NV12 上限估算（hor/ver stride 乘积 ×3/2）。
            buffer_size = static_cast<std::size_t>(hor_stride) * ver_stride * 3u / 2u;
        }
        ret = mpp_buffer_group_limit_config(output_group_, buffer_size,
                                            static_cast<RK_S32>(kOutputBufferCount));
        if (ret != MPP_OK)
        {
            return MakeMppStatus("配置输出缓冲组 limit", ret);
        }
        ret = api_->control(ctx_, MPP_DEC_SET_EXT_BUF_GROUP, output_group_);
        if (ret != MPP_OK)
        {
            return MakeMppStatus("SET_EXT_BUF_GROUP", ret);
        }
        ret = api_->control(ctx_, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        if (ret != MPP_OK)
        {
            return MakeMppStatus("INFO_CHANGE_READY", ret);
        }

        width_      = width;
        height_     = height;
        hor_stride_ = hor_stride;
        ver_stride_ = ver_stride;
        if (hor_stride % 16u != 0u)
        {
            // MPP 输出通常 16 对齐；未对齐时告警并继续（真实 stride 已写入 meta）。
            AM_LOG_WARNING() << "[MPP-DEC] 输出 hor_stride=" << hor_stride << " 未 16 对齐";
        }
        info_change_done_ = true;
        AM_LOG_INFO() << "[MPP-DEC] info-change: " << width_ << "x" << height_ << " stride("
                      << hor_stride_ << "," << ver_stride_ << ")";
        return Status::MakeOk();
    }

    // 构建输出 VideoFrame：owner 接管 frame（成功后调用方不得 deinit）。
    Status BuildOutputFrame(MppFrame frame, VideoFramePtr* out)
    {
        MppBuffer buffer = mpp_frame_get_buffer(frame);
        if (buffer == nullptr)
        {
            return Status(StatusCode::kInternal, "[MPP-DEC] 输出帧无缓冲");
        }
        const RK_U32 format = mpp_frame_get_fmt(frame) & MPP_FRAME_FMT_MASK;
        if (format != MPP_FMT_YUV420SP)
        {
            return Status(StatusCode::kInternal, "[MPP-DEC] 输出格式非 NV12（异常）");
        }
        void* ptr = mpp_buffer_get_ptr(buffer);
        if (ptr == nullptr)
        {
            return Status(StatusCode::kInternal, "[MPP-DEC] 输出缓冲无映射地址");
        }

        const RK_U32 width      = mpp_frame_get_width(frame);
        const RK_U32 height     = mpp_frame_get_height(frame);
        const RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
        const RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
        if (width == 0 || height == 0 || hor_stride == 0 || ver_stride == 0)
        {
            return Status(StatusCode::kInternal, "[MPP-DEC] 输出帧几何参数非法");
        }

        FrameMeta meta;
        meta.width  = width;
        meta.height = height;
        meta.stride =
            hor_stride;  // stride 必须写入 meta 供下游校验（审计缺陷：stride 未对齐/缺失）
        meta.format = PixelFormat::kNv12;

        const std::int64_t frame_pts = mpp_frame_get_pts(frame);
        if (frame_pts >= 0)
        {
            meta.timestamps.monotonic_ns = frame_pts * 1000LL;
        }
        else
        {
            timespec ts{};
            if (::clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
            {
                meta.timestamps.monotonic_ns =
                    static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
            }
        }

        auto frame_ptr   = std::make_shared<VideoFrame>(meta);
        frame_ptr->owner = std::make_shared<MppFrameOwner>(frame);

        const std::size_t   y_size = static_cast<std::size_t>(hor_stride) * ver_stride;
        const std::uint8_t* base   = static_cast<std::uint8_t*>(ptr);
        PlaneView           y_plane;
        y_plane.data       = base;
        y_plane.size       = y_size + y_size / 2u;
        y_plane.row_stride = hor_stride;
        PlaneView uv_plane;
        uv_plane.data       = base + y_size;
        uv_plane.size       = y_size / 2u;
        uv_plane.row_stride = hor_stride;
        frame_ptr->planes   = {y_plane, uv_plane};

        *out = std::move(frame_ptr);
        return Status::MakeOk();
    }

    void ReleaseAll()
    {
        if (ctx_ != nullptr && api_ != nullptr)
        {
            (void)api_->reset(ctx_);
        }
        if (output_group_ != nullptr)
        {
            (void)mpp_buffer_group_clear(output_group_);
            (void)mpp_buffer_group_put(output_group_);
            output_group_ = nullptr;
        }
        if (input_group_ != nullptr)
        {
            (void)mpp_buffer_group_clear(input_group_);
            (void)mpp_buffer_group_put(input_group_);
            input_group_ = nullptr;
        }
        if (ctx_ != nullptr)
        {
            (void)mpp_destroy(ctx_);
            ctx_ = nullptr;
            api_ = nullptr;
        }
        initialized_ = false;
    }

    MppCtx         ctx_              = nullptr;
    MppApi*        api_              = nullptr;
    MppBufferGroup output_group_     = nullptr;
    MppBufferGroup input_group_      = nullptr;
    bool           initialized_      = false;
    bool           info_change_done_ = false;
    std::uint32_t  width_            = 0;
    std::uint32_t  height_           = 0;
    std::uint32_t  hor_stride_       = 0;
    std::uint32_t  ver_stride_       = 0;
};

MppDecoder::MppDecoder() : impl_(std::make_unique<Impl>()) {}

MppDecoder::~MppDecoder() = default;

Status MppDecoder::Initialize(const MppDecoderConfig& config) { return impl_->Initialize(config); }

Status MppDecoder::Decode(const std::uint8_t* data, std::size_t size, std::int64_t pts_us,
                          VideoFramePtr* out)
{
    return impl_->Decode(data, size, pts_us, out);
}

void MppDecoder::Reset() { impl_->Reset(); }

}  // namespace animal_monitor
