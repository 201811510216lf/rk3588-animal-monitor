#pragma once

#include <chrono>

#include "animal_monitor/common/frame_meta.h"
#include "animal_monitor/common/status.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor {

// 视频源抽象：V4L2 相机（目标板）、mock 源（host 测试/演示）。
// 实现约定：
//  - Start() 失败必须完整回滚（RAII）；
//  - Stop() 幂等，唤醒阻塞的 WaitForFrame；
//  - 帧在内部以有界队列缓冲（容量/策略配置化），不得无限积压。
class VideoSource
{
  public:
    virtual ~VideoSource() = default;

    // 展示名（状态表使用）。
    virtual const char* Name() const = 0;

    // 名义输出格式（队列中帧的 meta 以实际为准）。
    virtual const FrameMeta& NominalMeta() const = 0;

    virtual Status Start() = 0;
    virtual void   Stop()  = 0;

    // 取一帧；超时返回 kTimeout（不视为错误）。关停后返回 kCancelled。
    virtual Status WaitForFrame(VideoFramePtr* out, std::chrono::milliseconds timeout) = 0;
};

}  // namespace animal_monitor
