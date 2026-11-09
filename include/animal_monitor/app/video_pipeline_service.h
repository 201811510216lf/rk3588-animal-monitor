#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "animal_monitor/app/pipeline_factory.h"
#include "animal_monitor/app/service.h"
#include "animal_monitor/media/frame_compositor.h"
#include "animal_monitor/media/frame_synchronizer.h"
#include "animal_monitor/media/video_encoder.h"
#include "animal_monitor/media/video_publisher.h"

namespace animal_monitor {

// 视频流水线服务：双路 VideoSource → FrameSynchronizer → FrameCompositor →
// VideoEncoder → VideoPublisher（composite 默认模式）。
// 线程：sync（同步+合成）、enc_in（送编码）、enc_out（取包→发布）。
// host 上以 MOCK 组件运行；目标板（deps ON）接入 V4L2/MPP/RGA/ZLM 真实实现。
class VideoPipelineService : public Service
{
  public:
    // 从配置树构建（cameras/composition/encoder/network 段）。
    static Status BuildFromConfig(const config::ConfigValue& root,
                                  std::unique_ptr<VideoPipelineService>* out);

    ~VideoPipelineService() override;

    const char* Name() const override;
    Status      Start() override;
    void        Stop() override;

    struct Snapshot
    {
        FrameSynchronizer::Metrics sync;
        std::uint64_t              composed = 0;
        std::uint64_t              encoded  = 0;
        std::uint64_t              published_packets = 0;
        std::uint64_t              published_bytes   = 0;
        std::uint64_t              encode_errors     = 0;
        std::uint64_t              compose_errors    = 0;
    };
    Snapshot GetSnapshot() const;

  private:
    VideoPipelineService() = default;

    void CaptureLoop(int camera_id);
    void SyncLoop();
    void EncInLoop();
    void EncOutLoop();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
