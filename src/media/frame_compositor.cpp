#include "animal_monitor/media/frame_compositor.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace animal_monitor
{

Status ComputeTileRects(const CompositorConfig& config, int num_sources, std::vector<TileRect>* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "null output");
    }
    if (num_sources < 1)
    {
        return Status(StatusCode::kInvalidArgument, "num_sources must be >= 1");
    }
    if (config.out_width == 0 || config.out_height == 0)
    {
        return Status(StatusCode::kInvalidArgument, "output size must be non-zero");
    }

    const std::uint32_t gap_total = static_cast<std::uint32_t>(num_sources - 1) * config.gap_px;
    if (config.layout == CompositorLayout::kSideBySide)
    {
        if (gap_total >= config.out_width)
        {
            return Status(StatusCode::kInvalidArgument, "gaps exceed output width");
        }
        const std::uint32_t tile_w = (config.out_width - gap_total) / num_sources;
        if (tile_w == 0)
        {
            return Status(StatusCode::kInvalidArgument, "tile width is zero");
        }
        out->clear();
        out->reserve(num_sources);
        for (int i = 0; i < num_sources; ++i)
        {
            out->push_back(TileRect{i * (tile_w + config.gap_px), 0, tile_w, config.out_height});
        }
        return Status::MakeOk();
    }

    // kTopBottom
    if (gap_total >= config.out_height)
    {
        return Status(StatusCode::kInvalidArgument, "gaps exceed output height");
    }
    const std::uint32_t tile_h = (config.out_height - gap_total) / num_sources;
    if (tile_h == 0)
    {
        return Status(StatusCode::kInvalidArgument, "tile height is zero");
    }
    out->clear();
    out->reserve(num_sources);
    for (int i = 0; i < num_sources; ++i)
    {
        out->push_back(TileRect{0, i * (tile_h + config.gap_px), config.out_width, tile_h});
    }
    return Status::MakeOk();
}

FrameCompositor::FrameCompositor(CompositorConfig config) : config_(config) {}

Status FrameCompositor::Compose(const std::vector<VideoFramePtr>& inputs, VideoFramePtr* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "null output");
    }
    if (config_.out_format != PixelFormat::kBgr888)
    {
        return Status(StatusCode::kUnimplemented,
                      "CPU compositor supports BGR888 only (target path uses RGA)");
    }

    std::vector<TileRect> rects;
    const Status status = ComputeTileRects(config_, static_cast<int>(inputs.size()), &rects);
    if (!status)
    {
        return status;
    }

    // 输出画布：黑色填充（gap 与未覆盖区域）。
    const std::size_t out_bytes =
        static_cast<std::size_t>(config_.out_width) * config_.out_height * 3;
    auto canvas   = std::make_shared<VideoFrame>(FrameMeta{});
    auto heap     = std::make_shared<HeapBuffer>(std::vector<std::uint8_t>(out_bytes, 0));
    canvas->owner = heap;
    canvas->planes.push_back(PlaneView{heap->Bytes().data(), out_bytes, config_.out_width * 3});
    canvas->meta.width                   = config_.out_width;
    canvas->meta.height                  = config_.out_height;
    canvas->meta.stride                  = config_.out_width * 3;
    canvas->meta.format                  = PixelFormat::kBgr888;
    canvas->meta.id.camera_id            = 0xFFFFFFFFu;  // 合成帧无单一相机来源
    canvas->meta.id.sequence             = 0;
    canvas->meta.timestamps.monotonic_ns = 0;

    std::uint8_t* canvas_data = heap->Bytes().data();

    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        const VideoFramePtr& input = inputs[i];
        const TileRect&      rect  = rects[i];
        if (input == nullptr || input->planes.empty() || input->planes[0].data == nullptr)
        {
            return Status(StatusCode::kInvalidArgument, "input frame has no CPU planes");
        }
        if (input->meta.format != PixelFormat::kBgr888)
        {
            return Status(StatusCode::kUnimplemented, "CPU compositor accepts BGR888 inputs only");
        }

        const std::uint32_t src_w = input->meta.width;
        const std::uint32_t src_h = input->meta.height;
        const std::uint32_t src_stride =
            input->planes[0].row_stride != 0
                ? input->planes[0].row_stride
                : (input->meta.stride != 0 ? input->meta.stride : src_w * 3);
        if (src_w == 0 || src_h == 0)
        {
            return Status(StatusCode::kInvalidArgument, "input frame has zero size");
        }
        const std::uint8_t* src_data = input->planes[0].data;

        // 最近邻缩放写入 tile（CPU 演示路径；目标板由 RGA 完成）。
        for (std::uint32_t dy = 0; dy < rect.h; ++dy)
        {
            const std::uint32_t sy =
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(dy) * src_h / rect.h);
            std::uint8_t* dst_row =
                canvas_data +
                (static_cast<std::size_t>(rect.y + dy) * config_.out_width + rect.x) * 3;
            const std::uint8_t* src_row = src_data + static_cast<std::size_t>(sy) * src_stride;
            for (std::uint32_t dx = 0; dx < rect.w; ++dx)
            {
                const std::uint32_t sx =
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(dx) * src_w / rect.w);
                dst_row[dx * 3 + 0] = src_row[sx * 3 + 0];
                dst_row[dx * 3 + 1] = src_row[sx * 3 + 1];
                dst_row[dx * 3 + 2] = src_row[sx * 3 + 2];
            }
        }
    }

    *out = std::move(canvas);
    return Status::MakeOk();
}

}  // namespace animal_monitor
