#include <cstdint>
#include <vector>

#include "test_harness.h"

#include "animal_monitor/media/frame_compositor.h"
#include "animal_monitor/media/video_frame.h"

using animal_monitor::CompositorConfig;
using animal_monitor::CompositorLayout;
using animal_monitor::ComputeTileRects;
using animal_monitor::FrameCompositor;
using animal_monitor::HeapBuffer;
using animal_monitor::TileRect;
using animal_monitor::VideoFrame;
using animal_monitor::VideoFramePtr;

namespace {

VideoFramePtr MakeSolidFrame(std::uint32_t width, std::uint32_t height, std::uint8_t b,
                             std::uint8_t g, std::uint8_t r)
{
    auto frame = std::make_shared<VideoFrame>();
    auto heap  = std::make_shared<HeapBuffer>(
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3, 0));
    std::uint8_t* data = heap->Bytes().data();
    for (std::size_t i = 0; i < heap->Bytes().size(); i += 3)
    {
        data[i + 0] = b;
        data[i + 1] = g;
        data[i + 2] = r;
    }
    frame->owner = heap;
    frame->planes.push_back(
        animal_monitor::PlaneView{heap->Bytes().data(), heap->Bytes().size(), width * 3});
    frame->meta.width  = width;
    frame->meta.height = height;
    frame->meta.stride = width * 3;
    frame->meta.format = animal_monitor::PixelFormat::kBgr888;
    return frame;
}

AM_TEST(compute_rects_side_by_side_two_sources)
{
    CompositorConfig config;
    config.layout     = CompositorLayout::kSideBySide;
    config.out_width  = 1280;
    config.out_height = 720;
    config.gap_px     = 0;

    std::vector<TileRect> rects;
    const auto            status = ComputeTileRects(config, 2, &rects);
    AM_EXPECT(status.Ok());
    AM_EXPECT_EQ(rects.size(), 2u);
    AM_EXPECT_EQ(rects[0].x, 0u);
    AM_EXPECT_EQ(rects[0].w, 640u);
    AM_EXPECT_EQ(rects[1].x, 640u);
    AM_EXPECT_EQ(rects[1].w, 640u);
    AM_EXPECT_EQ(rects[0].h, 720u);
}

AM_TEST(compute_rects_side_by_side_with_gap)
{
    CompositorConfig config;
    config.layout     = CompositorLayout::kSideBySide;
    config.out_width  = 1000;
    config.out_height = 720;
    config.gap_px     = 10;

    std::vector<TileRect> rects;
    AM_EXPECT(ComputeTileRects(config, 2, &rects).Ok());
    AM_EXPECT_EQ(rects[0].x, 0u);
    AM_EXPECT_EQ(rects[0].w, 495u);
    AM_EXPECT_EQ(rects[1].x, 505u);
    AM_EXPECT_EQ(rects[1].w, 495u);
}

AM_TEST(compute_rects_top_bottom)
{
    CompositorConfig config;
    config.layout     = CompositorLayout::kTopBottom;
    config.out_width  = 1280;
    config.out_height = 720;

    std::vector<TileRect> rects;
    AM_EXPECT(ComputeTileRects(config, 2, &rects).Ok());
    AM_EXPECT_EQ(rects[0].y, 0u);
    AM_EXPECT_EQ(rects[0].h, 360u);
    AM_EXPECT_EQ(rects[1].y, 360u);
    AM_EXPECT_EQ(rects[1].h, 360u);
}

AM_TEST(compute_rects_rejects_bad_args)
{
    CompositorConfig config;
    config.out_width  = 100;
    config.out_height = 720;
    config.gap_px     = 100;  // gap >= width
    std::vector<TileRect> rects;
    AM_EXPECT(!ComputeTileRects(config, 2, &rects).Ok());

    CompositorConfig zero;
    zero.out_width = 0;
    AM_EXPECT(!ComputeTileRects(zero, 2, &rects).Ok());
    AM_EXPECT(!ComputeTileRects(config, 0, &rects).Ok());
}

AM_TEST(compose_side_by_side_pixel_layout)
{
    CompositorConfig config;
    config.layout     = CompositorLayout::kSideBySide;
    config.out_width  = 8;
    config.out_height = 4;
    config.gap_px     = 0;

    FrameCompositor compositor(config);
    std::vector<VideoFramePtr> inputs;
    inputs.push_back(MakeSolidFrame(4, 4, 0, 0, 255));   // 左侧纯红
    inputs.push_back(MakeSolidFrame(4, 4, 255, 0, 0));   // 右侧纯蓝

    VideoFramePtr out;
    const auto    status = compositor.Compose(inputs, &out);
    AM_EXPECT(status.Ok());
    AM_EXPECT(out != nullptr);
    AM_EXPECT_EQ(out->meta.width, 8u);
    AM_EXPECT_EQ(out->meta.height, 4u);
    AM_EXPECT_EQ(out->planes[0].size, 8u * 4u * 3u);

    const std::uint8_t* data = out->planes[0].data;
    // 左上角（左侧 tile）：BGR = (0,0,255)
    AM_EXPECT_EQ(data[0], 0u);
    AM_EXPECT_EQ(data[1], 0u);
    AM_EXPECT_EQ(data[2], 255u);
    // 右上角（右侧 tile 首像素）：BGR = (255,0,0)
    AM_EXPECT_EQ(data[4 * 3 + 0], 255u);
    AM_EXPECT_EQ(data[4 * 3 + 1], 0u);
    AM_EXPECT_EQ(data[4 * 3 + 2], 0u);
}

AM_TEST(compose_with_gap_leaves_black)
{
    CompositorConfig config;
    config.layout     = CompositorLayout::kSideBySide;
    config.out_width  = 9;
    config.out_height = 2;
    config.gap_px     = 1;  // 4 + 1 + 4

    FrameCompositor compositor(config);
    std::vector<VideoFramePtr> inputs;
    inputs.push_back(MakeSolidFrame(4, 2, 0, 0, 255));
    inputs.push_back(MakeSolidFrame(4, 2, 255, 0, 0));

    VideoFramePtr out;
    AM_EXPECT(compositor.Compose(inputs, &out).Ok());
    const std::uint8_t* data = out->planes[0].data;
    // gap 列（x=4）：黑色
    AM_EXPECT_EQ(data[4 * 3 + 0], 0u);
    AM_EXPECT_EQ(data[4 * 3 + 1], 0u);
    AM_EXPECT_EQ(data[4 * 3 + 2], 0u);
    // 右侧 tile 首像素（x=5）
    AM_EXPECT_EQ(data[5 * 3 + 0], 255u);
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
