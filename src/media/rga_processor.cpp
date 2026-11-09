// 文件名：src/media/rga_processor.cpp
// 状态：implemented（模板）/ hardware-blocked —— 未经目标板编译与实测（UNVERIFIED）
// 来源：按 docs/migration/source_audit.md 与 _audit_reports/audit_ref_RKMediaProject.md
// 审计结论重写；
//       API 形状参考 .ref_audit_RKMediaProject/src/rkrga.cpp（只读审计），未逐行拷贝。
// 命名坑说明（继承参考实现的注释约定，避免踩坑）：
//   librga 的 rga_buffer_t 字段 wstride = 水平步长（像素），hstride = 垂直步长（像素）；
//   本项目 FrameMeta.stride 为字节。换算：stride_px = stride_bytes / 每像素字节数。
//   NV12：字节 == 像素；RGB888：字节 = 像素 × 3。
// 头文件布局：与多路视频/rga/include 对应，由 CMake 提供 include 路径。
#include "animal_monitor/media/rga_processor.h"

#include "im2d.h"
#include "im2d_buffer.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "dma_heap_allocator.h"
#include "animal_monitor/logging/logger.h"

namespace animal_monitor
{

namespace
{

// PixelFormat → RGA 格式映射；0 表示不支持。
int MapToRgaFormat(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::kNv12:
            return RK_FORMAT_YCbCr_420_SP;
        case PixelFormat::kNv21:
            return RK_FORMAT_YCrCb_420_SP;
        case PixelFormat::kYuv420p:
            return RK_FORMAT_YCbCr_420_P;
        case PixelFormat::kBgr888:
            return RK_FORMAT_BGR_888;
        case PixelFormat::kRgb888:
            return RK_FORMAT_RGB_888;
        default:
            return 0;
    }
}

// 每像素字节数（用于 stride 字节→像素换算）。
std::uint32_t BytesPerPixel(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::kRgb888:
        case PixelFormat::kBgr888:
            return 3;
        default:
            return 1;  // NV12/NV21/I420 的 stride 以字节计 == 像素
    }
}

std::uint32_t Align16(std::uint32_t value) { return (value + 15u) / 16u * 16u; }

Status MakeRgaStatus(const char* operation, IM_STATUS status)
{
    return Status(StatusCode::kInternal,
                  std::string("[RGA] ") + operation + " 失败: " + imStrError_t(status));
}

}  // namespace

class RgaProcessor::Impl
{
   public:
    explicit Impl(RgaProcessorConfig config) : allocator_(std::move(config.dma_heap_path)) {}

    Status Initialize()
    {
        if (initialized_)
        {
            return Status::MakeOk();
        }
        const IM_STATUS status = imcheckHeader();
        if (status != IM_STATUS_SUCCESS)
        {
            return Status(StatusCode::kHardwareUnavailable,
                          std::string("[RGA] imcheckHeader 失败: ") + imStrError_t(status));
        }
        AM_LOG_INFO() << "[RGA] 能力: " << querystring(RGA_ALL);
        initialized_ = true;
        return Status::MakeOk();
    }

    Status CvtColorResize(const VideoFrame& in, std::uint32_t width, std::uint32_t height,
                          PixelFormat dst_format, VideoFramePtr* out)
    {
        if (!initialized_)
        {
            return Status(StatusCode::kConfigurationError, "[RGA] 未初始化（先调用 Initialize）");
        }
        if (out == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[RGA] out 为空");
        }
        if (width == 0 || height == 0)
        {
            return Status(StatusCode::kInvalidArgument, "[RGA] 目标宽高必须非零");
        }

        const int src_fmt = MapToRgaFormat(in.meta.format);
        const int dst_fmt = MapToRgaFormat(dst_format);
        if (src_fmt == 0 || dst_fmt == 0)
        {
            return Status(StatusCode::kInvalidArgument, "[RGA] 不支持的像素格式组合");
        }

        const Status geometry_status = ValidateFrameGeometry(in);
        if (!geometry_status)
        {
            return geometry_status;
        }

        // 目标 stride：按格式 16 字节对齐（审计缺陷：stride 未对齐禁止带入）。
        const std::uint32_t dst_stride_bytes =
            (BytesPerPixel(dst_format) == 3) ? Align16(width * 3u) : Align16(width);
        const std::uint32_t dst_stride_px = dst_stride_bytes / BytesPerPixel(dst_format);
        const std::size_t   dst_size =
            (BytesPerPixel(dst_format) == 3)
                  ? static_cast<std::size_t>(dst_stride_bytes) * height
                  : static_cast<std::size_t>(dst_stride_bytes) * height * 3u / 2u;

        DmaBuffer dst_buffer;
        Status    status = allocator_.Allocate(dst_size, &dst_buffer);
        if (!status)
        {
            return status;
        }

        RgaBuf src{};
        status = BuildInputRgaBuf(in, src_fmt, &src);
        if (!status)
        {
            return status;
        }
        RgaBuf dst{};
        status = BuildOutputRgaBuf(dst_buffer, width, height, dst_stride_px, dst_fmt, &dst);
        if (!status)
        {
            ReleaseRgaHandle(src);
            return status;
        }

        IM_STATUS im_status = IM_STATUS_FAILED;
        if (src_fmt == dst_fmt)
        {
            // 同格式：单次 imresize。
            im_status = imresize(src.buffer, dst.buffer);
        }
        else
        {
            // 跨格式：先同尺寸 imcvtcolor 到目标格式，再 imresize（两步）。
            // 临时缓冲容量按目标格式计算（如 NV12→RGB888 需 3 字节/像素，按源格式算会溢出）。
            const std::uint32_t temp_stride_px = (BytesPerPixel(dst_format) == 3)
                                                     ? Align16(in.meta.width * 3u) / 3u
                                                     : Align16(in.meta.width);
            const std::size_t   temp_size =
                (BytesPerPixel(dst_format) == 3)
                      ? static_cast<std::size_t>(temp_stride_px) * 3u * in.meta.height
                      : static_cast<std::size_t>(temp_stride_px) * in.meta.height * 3u / 2u;
            DmaBuffer temp_buffer;
            status = allocator_.Allocate(temp_size, &temp_buffer);
            if (!status)
            {
                ReleaseRgaHandle(src);
                ReleaseRgaHandle(dst);
                return status;
            }
            RgaBuf temp{};
            status = BuildOutputRgaBuf(temp_buffer, in.meta.width, in.meta.height, temp_stride_px,
                                       dst_fmt, &temp);
            if (!status)
            {
                ReleaseRgaHandle(src);
                ReleaseRgaHandle(dst);
                return status;
            }
            im_status = imcvtcolor(src.buffer, temp.buffer, src_fmt, dst_fmt);
            if (im_status == IM_STATUS_SUCCESS)
            {
                im_status = imresize(temp.buffer, dst.buffer);
            }
            ReleaseRgaHandle(temp);
        }
        ReleaseRgaHandle(src);
        ReleaseRgaHandle(dst);
        if (im_status != IM_STATUS_SUCCESS)
        {
            return MakeRgaStatus("cvtcolor/resize", im_status);
        }

        *out = BuildOutputFrame(in, width, height, dst_format, dst_stride_bytes,
                                std::move(dst_buffer));
        return Status::MakeOk();
    }

    Status CompositeSideBySide(const VideoFrame& left, const VideoFrame& right, VideoFramePtr* out)
    {
        if (!initialized_)
        {
            return Status(StatusCode::kConfigurationError, "[RGA] 未初始化（先调用 Initialize）");
        }
        if (out == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[RGA] out 为空");
        }
        if (left.meta.format != PixelFormat::kNv12 || right.meta.format != PixelFormat::kNv12)
        {
            return Status(StatusCode::kInvalidArgument,
                          "[RGA] 拼接输入必须同为 NV12（不同格式先 CvtColorResize 归一化）");
        }

        Status status = ValidateFrameGeometry(left);
        if (!status)
        {
            return status;
        }
        status = ValidateFrameGeometry(right);
        if (!status)
        {
            return status;
        }

        // 画布：宽 = 左 + 右，高 = 两路较大值；stride 16 对齐（字节==像素，NV12）。
        const std::uint32_t canvas_width  = left.meta.width + right.meta.width;
        const std::uint32_t canvas_height = std::max(left.meta.height, right.meta.height);
        const std::uint32_t canvas_stride = Align16(canvas_width);
        const std::size_t   canvas_size =
            static_cast<std::size_t>(canvas_stride) * canvas_height * 3u / 2u;

        DmaBuffer canvas;
        status = allocator_.Allocate(canvas_size, &canvas);
        if (!status)
        {
            return status;
        }

        RgaBuf left_src{};
        status = BuildInputRgaBuf(left, RK_FORMAT_YCbCr_420_SP, &left_src);
        if (!status)
        {
            return status;
        }
        RgaBuf right_src{};
        status = BuildInputRgaBuf(right, RK_FORMAT_YCbCr_420_SP, &right_src);
        if (!status)
        {
            ReleaseRgaHandle(left_src);
            return status;
        }

        // 左半区域：画布 VA 偏移 0；右半区域：VA 偏移 left_w（Y 平面）。
        // 注意：wrapbuffer_virtualaddr_t 按 va + wstride×hstride 计算 UV 平面地址，
        // 因此右半区 va 偏移 left_w 后 UV 自动落在画布 UV 区的对应偏移（同名坑见文件头）。
        const std::uint32_t left_width  = canvas_width / 2u;
        const std::uint32_t right_width = canvas_width - left_width;
        rga_buffer_t        dst_left    = wrapbuffer_virtualaddr_t(
            canvas.Address(), static_cast<int>(left_width), static_cast<int>(canvas_height),
            static_cast<int>(canvas_stride), static_cast<int>(canvas_height),
            RK_FORMAT_YCbCr_420_SP);
        rga_buffer_t dst_right = wrapbuffer_virtualaddr_t(
            canvas.ByteAddress() + left_width, static_cast<int>(right_width),
            static_cast<int>(canvas_height), static_cast<int>(canvas_stride),
            static_cast<int>(canvas_height), RK_FORMAT_YCbCr_420_SP);

        IM_STATUS im_status = imresize(left_src.buffer, dst_left);
        if (im_status == IM_STATUS_SUCCESS)
        {
            im_status = imresize(right_src.buffer, dst_right);
        }
        ReleaseRgaHandle(left_src);
        ReleaseRgaHandle(right_src);
        if (im_status != IM_STATUS_SUCCESS)
        {
            return MakeRgaStatus("side-by-side imresize", im_status);
        }

        *out = BuildOutputFrame(left, canvas_width, canvas_height, PixelFormat::kNv12,
                                canvas_stride, std::move(canvas));
        return Status::MakeOk();
    }

   private:
    // RGA 缓冲包装（fd 导入优先，虚拟地址回退）+ 句柄释放封装。
    struct RgaBuf
    {
        rga_buffer_t        buffer{};
        rga_buffer_handle_t handle = 0;
    };

    static void ReleaseRgaHandle(const RgaBuf& buf)
    {
        if (buf.handle != 0)
        {
            (void)releasebuffer_handle(buf.handle);
        }
    }

    Status ValidateFrameGeometry(const VideoFrame& frame) const
    {
        if (frame.meta.width == 0 || frame.meta.height == 0 || frame.meta.stride == 0)
        {
            return Status(StatusCode::kInvalidArgument,
                          "[RGA] 输入帧 meta 缺少 width/height/stride");
        }
        if (frame.planes.empty() || frame.planes[0].data == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "[RGA] 输入帧无平面数据（非 CPU 可访问）");
        }
        return Status::MakeOk();
    }

    // 输入缓冲：owner 为 FdBufferOwner 且 fd 有效 → importbuffer_fd（零拷贝）；
    // 失败/不可用 → wrapbuffer_virtualaddr_t 虚拟地址回退（任务书要求）。
    // stride 显式传递：meta.stride（字节）→ wstride/hstride（像素）。
    Status BuildInputRgaBuf(const VideoFrame& frame, int rga_format, RgaBuf* out)
    {
        const std::uint32_t stride_px = frame.meta.stride / BytesPerPixel(frame.meta.format);
        if (stride_px < frame.meta.width)
        {
            return Status(StatusCode::kInvalidArgument, "[RGA] 输入 stride 小于宽度");
        }

        const FdBufferOwner* fd_owner = dynamic_cast<const FdBufferOwner*>(frame.owner.get());
        if (fd_owner != nullptr && fd_owner->Fd() >= 0 && fd_owner->Size() > 0)
        {
            const int fd   = fd_owner->Fd();
            const int size = static_cast<int>(fd_owner->Size());
            out->handle    = importbuffer_fd(fd, size);
            if (out->handle != 0)
            {
                out->buffer = wrapbuffer_handle(out->handle, static_cast<int>(frame.meta.width),
                                                static_cast<int>(frame.meta.height), rga_format,
                                                static_cast<int>(stride_px),
                                                static_cast<int>(frame.meta.height));
                return Status::MakeOk();
            }
            AM_LOG_WARNING() << "[RGA] importbuffer_fd 失败，回退虚拟地址路径";
        }

        out->handle = 0;
        // wrapbuffer_virtualaddr_t 的 vir_addr 为 void*：const_cast 仅用于适配 C 接口签名，
        // RGA 只读输入缓冲，运行期不会修改源数据。
        out->buffer = wrapbuffer_virtualaddr_t(
            const_cast<std::uint8_t*>(frame.planes[0].data), static_cast<int>(frame.meta.width),
            static_cast<int>(frame.meta.height), static_cast<int>(stride_px),
            static_cast<int>(frame.meta.height), rga_format);
        return Status::MakeOk();
    }

    // 输出缓冲：新鲜 dma-heap fd → importbuffer_fd（优先）；失败回退虚拟地址。
    Status BuildOutputRgaBuf(const DmaBuffer& buffer, std::uint32_t width, std::uint32_t height,
                             std::uint32_t stride_px, int rga_format, RgaBuf* out)
    {
        out->handle = importbuffer_fd(buffer.Fd(), static_cast<int>(buffer.Size()));
        if (out->handle != 0)
        {
            out->buffer = wrapbuffer_handle(out->handle, static_cast<int>(width),
                                            static_cast<int>(height), rga_format,
                                            static_cast<int>(stride_px), static_cast<int>(height));
            return Status::MakeOk();
        }
        AM_LOG_WARNING() << "[RGA] 输出 importbuffer_fd 失败，回退虚拟地址路径";
        out->handle = 0;
        out->buffer = wrapbuffer_virtualaddr_t(
            buffer.Address(), static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(stride_px), static_cast<int>(height), rga_format);
        return Status::MakeOk();
    }

    // 输出帧：owner 独占 dma-heap fd（析构释放）；planes 按格式拆分；时间戳随源帧。
    VideoFramePtr BuildOutputFrame(const VideoFrame& src, std::uint32_t width, std::uint32_t height,
                                   PixelFormat format, std::uint32_t stride_bytes, DmaBuffer buffer)
    {
        FrameMeta meta;
        meta.id         = src.meta.id;
        meta.timestamps = src.meta.timestamps;
        meta.width      = width;
        meta.height     = height;
        meta.stride     = stride_bytes;  // stride 必须写入 meta（审计缺陷：stride 缺失）
        meta.format     = format;

        auto                frame = std::make_shared<VideoFrame>(meta);
        auto                owner = std::make_shared<DmaHeapFrameOwner>(std::move(buffer));
        const std::uint8_t* base  = static_cast<const std::uint8_t*>(owner->Address());
        frame->owner              = std::move(owner);

        if (format == PixelFormat::kRgb888 || format == PixelFormat::kBgr888)
        {
            PlaneView plane;
            plane.data       = base;
            plane.size       = static_cast<std::size_t>(stride_bytes) * height;
            plane.row_stride = stride_bytes;
            frame->planes    = {plane};
        }
        else
        {
            const std::size_t y_size = static_cast<std::size_t>(stride_bytes) * height;
            PlaneView         y_plane;
            y_plane.data       = base;
            y_plane.size       = y_size + y_size / 2u;
            y_plane.row_stride = stride_bytes;
            PlaneView uv_plane;
            uv_plane.data       = base + y_size;
            uv_plane.size       = y_size / 2u;
            uv_plane.row_stride = stride_bytes;
            frame->planes       = {y_plane, uv_plane};
        }
        return frame;
    }

    DmaHeapAllocator allocator_;
    bool             initialized_ = false;
};

RgaProcessor::RgaProcessor(RgaProcessorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{}

RgaProcessor::~RgaProcessor() = default;

Status RgaProcessor::Initialize() { return impl_->Initialize(); }

Status RgaProcessor::CvtColorResize(const VideoFrame& in, std::uint32_t width, std::uint32_t height,
                                    PixelFormat dst_format, VideoFramePtr* out)
{
    return impl_->CvtColorResize(in, width, height, dst_format, out);
}

Status RgaProcessor::CompositeSideBySide(const VideoFrame& left, const VideoFrame& right,
                                         VideoFramePtr* out)
{
    return impl_->CompositeSideBySide(left, right, out);
}

}  // namespace animal_monitor
