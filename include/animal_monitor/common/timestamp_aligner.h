#pragma once

#include <cstdint>
#include <optional>

namespace animal_monitor {

// 双路帧时间戳对齐结果。
struct AlignedPair
{
    std::int64_t timestamp_a_ns = 0;
    std::int64_t timestamp_b_ns = 0;
    std::int64_t delta_ns       = 0;  // a - b
};

// 最近邻流式对齐器。
//
// 语义：每路保留最新样本；两路都有样本时计算时间差，
// 若 |delta| <= tolerance_ns 则配对并清空；
// 否则丢弃较旧样本、保留较新样本（标记为不同步，由调用方记录指标）。
//
// 内部使用单调时钟纳秒时间戳；墙上时间另行保存（AGENTS 规则）。
class TimestampAligner
{
  public:
    explicit TimestampAligner(std::int64_t tolerance_ns);

    // 提交一路新样本（source_id 取 0/1）；返回是否产生对齐结果。
    std::optional<AlignedPair> AddSample(int source_id, std::int64_t timestamp_ns);

    void Reset();

  private:
    const std::int64_t tolerance_ns_;
    bool               has_a_ = false;
    bool               has_b_ = false;
    std::int64_t       a_     = 0;
    std::int64_t       b_     = 0;
};

}  // namespace animal_monitor
