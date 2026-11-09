#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "animal_monitor/app/service.h"

namespace animal_monitor
{

// 明确标注的 MOCK 流水线：不接触任何真实硬件/SDK。
// 用途：在 host 上验证基础设施（有界队列、指标、优雅停机）的接线正确性。
// 禁止把该服务的输出当作真实视频/推理结果。
class MockPipelineService : public Service
{
   public:
    // producer_interval：模拟帧间隔；duration：运行时长（0 表示无限，靠 Stop 结束）。
    MockPipelineService(std::chrono::milliseconds producer_interval,
                        std::chrono::milliseconds duration);

    ~MockPipelineService() override;

    const char* Name() const override;
    Status      Start() override;
    void        Stop() override;

    // 运行期统计快照（供主线程打印）。
    struct Snapshot
    {
        std::uint64_t produced   = 0;
        std::uint64_t consumed   = 0;
        std::uint64_t dropped    = 0;
        std::uint64_t queue_size = 0;
    };
    Snapshot GetSnapshot() const;

   private:
    void ProducerLoop();
    void ConsumerLoop();

    const std::chrono::milliseconds producer_interval_;
    const std::chrono::milliseconds duration_;

    std::atomic<bool> stop_requested_{false};
    std::thread       producer_;
    std::thread       consumer_;

    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> consumed_{0};

    // 有界队列（容量 8，满则丢最旧——视频链路默认策略）
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace animal_monitor
