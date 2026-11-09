#include "animal_monitor/common/timestamp_aligner.h"

#include <cstdlib>

namespace animal_monitor
{

TimestampAligner::TimestampAligner(std::int64_t tolerance_ns) : tolerance_ns_(tolerance_ns) {}

std::optional<AlignedPair> TimestampAligner::AddSample(int source_id, std::int64_t timestamp_ns)
{
    if (source_id == 0)
    {
        a_     = timestamp_ns;
        has_a_ = true;
    }
    else
    {
        b_     = timestamp_ns;
        has_b_ = true;
    }

    if (!has_a_ || !has_b_)
    {
        return std::nullopt;
    }

    const std::int64_t delta = a_ - b_;
    if (std::llabs(delta) <= tolerance_ns_)
    {
        const AlignedPair pair{a_, b_, delta};
        has_a_ = false;
        has_b_ = false;
        return pair;
    }

    // 超阈值：丢弃较旧样本，保留较新样本，等待下一路新样本。
    if (a_ < b_)
    {
        has_a_ = false;
    }
    else
    {
        has_b_ = false;
    }
    return std::nullopt;
}

void TimestampAligner::Reset()
{
    has_a_ = false;
    has_b_ = false;
    a_     = 0;
    b_     = 0;
}

}  // namespace animal_monitor
