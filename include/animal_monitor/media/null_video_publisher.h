#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "animal_monitor/media/video_publisher.h"

namespace animal_monitor {

// 明确标注的 MOCK 发布器：不产生网络数据，仅统计喂入字节数/包数。
class NullVideoPublisher : public VideoPublisher
{
  public:
    Status Initialize(const Config& config) override;
    Status FeedH264(const std::uint8_t* data, std::size_t size, std::int64_t dts_ms,
                    std::int64_t pts_ms) override;
    void   Close() override;
    std::string RtspUrl() const override;

    struct Snapshot
    {
        std::uint64_t packets    = 0;
        std::uint64_t bytes      = 0;
    };
    Snapshot GetSnapshot() const;

  private:
    Config                     config_;
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> bytes_{0};
};

}  // namespace animal_monitor
