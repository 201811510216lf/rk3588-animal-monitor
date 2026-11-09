#pragma once

#include <cstdint>
#include <vector>

#include "animal_monitor/common/status.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor {

enum class CompositorLayout : std::uint32_t
{
    kSideBySide = 0,
    kTopBottom  = 1,
};

struct CompositorConfig
{
    CompositorLayout layout      = CompositorLayout::kSideBySide;
    std::uint32_t    out_width   = 1280;
    std::uint32_t    out_height  = 720;
    std::uint32_t    gap_px      = 0;
    PixelFormat      out_format  = PixelFormat::kBgr888;
};

// 合成画布中的 tile 矩形（像素坐标，左上原点）。
struct TileRect
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

// 纯函数：按布局计算 N 路输入的 tile 矩形。
// 布局/间隙/尺寸规则见 docs/decisions/ADR-0002-dual-camera-composition.md。
Status ComputeTileRects(const CompositorConfig& config, int num_sources,
                        std::vector<TileRect>* out);

// 帧合成器（可移植 CPU 实现，host 测试/演示与目标板回退路径）。
// 目标板零拷贝路径使用 RGA（src/media/rga_processor.cpp）。
// 输入要求：kBgr888 单平面、宽高与 tile 矩形不一致时做最近邻缩放（CPU 路径
// 为演示用途，目标板由 RGA 处理缩放）。
class FrameCompositor
{
  public:
    explicit FrameCompositor(CompositorConfig config);

    // inputs 数量须与构造时的源数量一致（2 路默认）；输出为单帧 BGR888。
    Status Compose(const std::vector<VideoFramePtr>& inputs, VideoFramePtr* out);

    const CompositorConfig& Config() const { return config_; }

  private:
    CompositorConfig config_;
};

}  // namespace animal_monitor
