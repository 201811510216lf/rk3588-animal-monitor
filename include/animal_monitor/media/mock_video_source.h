#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "animal_monitor/camera/video_source.h"
#include "animal_monitor/media/video_frame.h"

namespace animal_monitor {

// 明确标注的 MOCK 视频源：合成 BGR888 测试图（纯色底 + 移动亮块）。
// 用于 host 流水线测试与演示；不代表任何真实相机/ISP 行为。
class MockVideoSource : public VideoSource
{
  public:
    struct Config
    {
        std::uint32_t width       = 640;
        std::uint32_t height      = 480;
        std::uint32_t fps         = 30;
        std::int64_t  duration_ms = 0;  // 0 = 无限（Stop 结束）
        std::uint8_t  base_b      = 0;
        std::uint8_t  base_g      = 0;
        std::uint8_t  base_r      = 0;
        std::string   name        = "mock_video_source";
    };

    explicit MockVideoSource(Config config);
    ~MockVideoSource() override;

    const char*        Name() const override;
    const FrameMeta&   NominalMeta() const override;
    Status             Start() override;
    void               Stop() override;
    Status             WaitForFrame(VideoFramePtr* out,
                                    std::chrono::milliseconds timeout) override;

    // 运行期统计。
    struct Snapshot
    {
        std::uint64_t produced  = 0;
        std::uint64_t consumed  = 0;
        std::uint64_t dropped   = 0;
        std::uint64_t queue_size = 0;
    };
    Snapshot GetSnapshot() const;

  private:
    void ProduceLoop();

    class Impl;
    std::unique_ptr<Impl>     impl_;
    Config                    config_;
    FrameMeta                 nominal_;
    std::thread               producer_;
    std::atomic<bool>         stop_requested_{false};
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> consumed_{0};
};

}  // namespace animal_monitor
