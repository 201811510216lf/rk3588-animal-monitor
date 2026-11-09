#pragma once

#include <cstdint>

namespace animal_monitor {

// 像素格式。源项目中实际出现的格式为 NV12/BGR888/MJPEG/H264；
// Bayer 相关项仅作预留，实际使用依赖板卡 BSP ISP（UNVERIFIED）。
enum class PixelFormat : std::uint32_t
{
    kUnknown   = 0,
    kNv12      = 1,
    kNv21      = 2,
    kYuv420p   = 3,
    kBgr888    = 4,
    kRgb888    = 5,
    kMjpeg     = 6,
    kH264      = 7,
    kH265      = 8,
    kRawBayer8 = 9,   // UNVERIFIED：Bayer 不得当作 RGB/NV12 使用
    kRawBayer10 = 10, // UNVERIFIED
};

const char* PixelFormatName(PixelFormat format);

struct FrameId
{
    std::uint32_t camera_id = 0;
    std::uint64_t sequence  = 0;  // 每路独立递增
};

// 内部时序一律使用单调时钟；墙上时间单独保存（AGENTS 规则）。
struct FrameTimestamps
{
    std::int64_t monotonic_ns = 0;
    std::int64_t wall_clock_ns = 0;
};

// 帧元数据：与像素数据分离，随帧在队列间流转。
struct FrameMeta
{
    FrameId         id;
    FrameTimestamps timestamps;
    std::uint32_t   width  = 0;
    std::uint32_t   height = 0;
    std::uint32_t   stride = 0;  // 字节；合成/转换前必须统一并校验 stride，禁止忽略
    PixelFormat     format = PixelFormat::kUnknown;
};

}  // namespace animal_monitor
