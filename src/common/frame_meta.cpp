#include "animal_monitor/common/frame_meta.h"

namespace animal_monitor {

const char* PixelFormatName(PixelFormat format)
{
    switch (format)
    {
        case PixelFormat::kUnknown:   return "unknown";
        case PixelFormat::kNv12:      return "nv12";
        case PixelFormat::kNv21:      return "nv21";
        case PixelFormat::kYuv420p:   return "yuv420p";
        case PixelFormat::kBgr888:    return "bgr888";
        case PixelFormat::kRgb888:    return "rgb888";
        case PixelFormat::kMjpeg:     return "mjpeg";
        case PixelFormat::kH264:      return "h264";
        case PixelFormat::kH265:      return "h265";
        case PixelFormat::kRawBayer8: return "raw_bayer8";
        case PixelFormat::kRawBayer10: return "raw_bayer10";
    }
    return "unknown";
}

}  // namespace animal_monitor
