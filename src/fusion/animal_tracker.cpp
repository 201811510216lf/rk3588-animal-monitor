// ============================================================================
// 文件：src/fusion/animal_tracker.cpp
// 状态：implemented（纯逻辑，host 可编译；MSVC /W4 /permissive- 零告警）
// 依据：任务书 CODEX_MASTER_TASK.md §10；include/animal_monitor/fusion/animal_tracker.h
// ============================================================================

#include "animal_monitor/fusion/animal_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>

namespace animal_monitor::fusion {

namespace {

// 两框 IoU。框为 {x, y, w, h}；任一面积为 0 或不相交时返回 0。
float BboxIoU(const float a[4], const float b[4])
{
    const double ax1 = static_cast<double>(a[0]);
    const double ay1 = static_cast<double>(a[1]);
    const double ax2 = static_cast<double>(a[0] + a[2]);
    const double ay2 = static_cast<double>(a[1] + a[3]);
    const double bx1 = static_cast<double>(b[0]);
    const double by1 = static_cast<double>(b[1]);
    const double bx2 = static_cast<double>(b[0] + b[2]);
    const double by2 = static_cast<double>(b[1] + b[3]);

    const double inter_w = std::max(0.0, std::min(ax2, bx2) - std::max(ax1, bx1));
    const double inter_h = std::max(0.0, std::min(ay2, by2) - std::max(ay1, by1));
    const double inter   = inter_w * inter_h;
    const double area_a  = static_cast<double>(a[2]) * static_cast<double>(a[3]);
    const double area_b  = static_cast<double>(b[2]) * static_cast<double>(b[3]);
    const double union_area = area_a + area_b - inter;
    if (union_area <= 0.0)
    {
        return 0.0f;
    }
    return static_cast<float>(inter / union_area);
}

bool IsValidDetectionBbox(const inference::Detection& detection)
{
    return std::isfinite(detection.bbox.x) && std::isfinite(detection.bbox.y) &&
           std::isfinite(detection.bbox.w) && std::isfinite(detection.bbox.h) &&
           detection.bbox.w >= 0.0f && detection.bbox.h >= 0.0f;
}

}  // namespace

AnimalTracker::AnimalTracker(Config config) : config_(config)
{
    // 非法配置钳制为可运行值（保证有界、确定性，语义见头文件）。
    if (config_.iou_threshold < 0.0f)
    {
        config_.iou_threshold = 0.0f;
    }
    if (config_.iou_threshold > 1.0f)
    {
        config_.iou_threshold = 1.0f;
    }
    if (config_.max_miss < 1u)
    {
        config_.max_miss = 1u;
    }
    if (config_.max_tracks < 1u)
    {
        config_.max_tracks = 1u;
    }
}

std::vector<Track> AnimalTracker::Update(const std::vector<inference::Detection>& detections,
                                         std::uint64_t                           seq)
{
    // matched[i]：tracks_[i] 本帧是否已关联（新建 track 记为已关联，本帧不计 miss）。
    std::vector<bool> matched(tracks_.size(), false);

    for (const inference::Detection& detection : detections)
    {
        if (!IsValidDetectionBbox(detection))
        {
            continue;
        }
        const float bbox[4] = {detection.bbox.x, detection.bbox.y, detection.bbox.w,
                               detection.bbox.h};

        // 贪心关联：取未匹配 track 中 IoU 最大且不低于阈值者。
        std::size_t best_index  = 0;
        bool        has_best    = false;
        float       best_iou    = 0.0f;
        for (std::size_t i = 0; i < tracks_.size(); ++i)
        {
            if (matched[i])
            {
                continue;
            }
            const float iou = BboxIoU(bbox, tracks_[i].bbox);
            if (iou >= config_.iou_threshold && (!has_best || iou > best_iou))
            {
                best_index = i;
                best_iou   = iou;
                has_best   = true;
            }
        }

        if (has_best)
        {
            Track& track = tracks_[best_index];
            track.bbox[0]       = bbox[0];
            track.bbox[1]       = bbox[1];
            track.bbox[2]       = bbox[2];
            track.bbox[3]       = bbox[3];
            track.last_seen_seq = seq;
            track.miss_count    = 0;
            matched[best_index] = true;
        }
        else
        {
            Track track;
            track.track_id      = next_id_;
            ++next_id_;
            track.bbox[0]       = bbox[0];
            track.bbox[1]       = bbox[1];
            track.bbox[2]       = bbox[2];
            track.bbox[3]       = bbox[3];
            track.last_seen_seq = seq;
            track.miss_count    = 0;
            tracks_.push_back(track);
            matched.push_back(true);  // 本帧新建，不计 miss
        }
    }

    // 未匹配 track 累加 miss，超过 max_miss 移除。
    for (std::size_t i = 0; i < tracks_.size(); ++i)
    {
        if (!matched[i])
        {
            ++tracks_[i].miss_count;
        }
    }
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [this](const Track& track)
                                 { return track.miss_count > config_.max_miss; }),
                  tracks_.end());

    // 容量裁剪：确定性淘汰最陈旧者（miss 多 → 更早 last_seen_seq → 更小 track_id）。
    if (tracks_.size() > config_.max_tracks)
    {
        const std::size_t remove_count = tracks_.size() - config_.max_tracks;
        std::vector<std::size_t> order(tracks_.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(),
                  [this](std::size_t a, std::size_t b)
                  {
                      const Track& ta = tracks_[a];
                      const Track& tb = tracks_[b];
                      if (ta.miss_count != tb.miss_count)
                      {
                          return ta.miss_count > tb.miss_count;
                      }
                      if (ta.last_seen_seq != tb.last_seen_seq)
                      {
                          return ta.last_seen_seq < tb.last_seen_seq;
                      }
                      return ta.track_id < tb.track_id;
                  });
        std::vector<bool> drop(tracks_.size(), false);
        for (std::size_t k = 0; k < remove_count; ++k)
        {
            drop[order[k]] = true;
        }
        std::vector<Track> kept;
        kept.reserve(config_.max_tracks);
        for (std::size_t i = 0; i < tracks_.size(); ++i)
        {
            if (!drop[i])
            {
                kept.push_back(tracks_[i]);
            }
        }
        tracks_ = std::move(kept);
    }

    return tracks_;
}

}  // namespace animal_monitor::fusion
