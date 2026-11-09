#pragma once

// animal_monitor/fusion/animal_tracker.h
// 状态：implemented（host 可编译，纯逻辑，无外部依赖）
// 依据：任务书 CODEX_MASTER_TASK.md §10（Dynamic ROI / Individual baseline 前的目标跟踪）；
//       AGENTS.md（C++17、k 常量、尾下划线成员、无全局可变状态）；
//       审计结论：源项目 A/B 仅有多模型检测融合（IoU/IoM/加权/NMS，缺陷见 audit_B），
//       无跨帧动物跟踪器，本模块为全新实现。
// 说明：
//   - IoU 贪婪关联跟踪器：逐帧检测框与现有 track 按 IoU 贪心匹配；未匹配检测新建 track；
//     连续未匹配超过 max_miss 的 track 移除；容量超过 max_tracks 时淘汰最陈旧的 track。
//   - 纯逻辑、确定性（同输入必同输出），不做卡尔曼滤波等运动模型，后续可替换实现。

#include <cstdint>
#include <vector>

#include "animal_monitor/inference/inference_types.h"

namespace animal_monitor::fusion {

// 单条跟踪轨迹。
struct Track
{
    std::int32_t  track_id      = 0;
    float         bbox[4]       = {0.0f, 0.0f, 0.0f, 0.0f};  // {x, y, w, h}
    std::uint64_t last_seen_seq = 0;                          // 最近一次匹配到的帧序号
    std::uint32_t miss_count    = 0;                          // 连续未匹配帧数
};

// IoU 关联跟踪器。
class AnimalTracker
{
  public:
    struct Config
    {
        std::uint32_t max_miss      = 3;     // 连续 miss 超过该值即移除（条件 miss_count > max_miss）
        float         iou_threshold = 0.3f;  // 关联 IoU 下限
        std::uint32_t max_tracks    = 16;    // 活跃 track 上限
    };

    // 构造时对非法配置做钳制：iou_threshold 限制在 [0,1]，max_miss / max_tracks 至少为 1。
    explicit AnimalTracker(Config config = Config());

    // 输入一帧检测结果与帧序号（期望单调递增；乱序不检测）。
    // 返回当前活跃 tracks。匹配成功的 track 更新 bbox 并清零 miss_count；
    // 未匹配的 track miss_count+1；未匹配检测生成新 track（本帧不计 miss）；
    // 超容量时淘汰最陈旧者（miss 多者优先，并列取更早 last_seen_seq，
    // 再并列取更小 track_id，保证确定性）。
    // 非法检测框（宽高为负或含非有限值）跳过，不参与关联。
    std::vector<Track> Update(const std::vector<inference::Detection>& detections,
                              std::uint64_t                           seq);

  private:
    Config             config_;
    std::int32_t       next_id_ = 0;
    std::vector<Track> tracks_;
};

}  // namespace animal_monitor::fusion
