#include "animal_monitor/inference/yolov5_postprocess.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace animal_monitor::inference {

namespace {

constexpr int kAnchorCount       = 3;
constexpr int kBoxAttributeCount = 5;  // tx, ty, tw, th, box_conf（类别跟随其后）

float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

float Dequantize(std::int8_t qnt, std::int32_t zp, float scale)
{
    return (static_cast<float>(qnt) - static_cast<float>(zp)) * scale;
}

// IoU（x,y,w,h 表示法，无 +1 像素修正；A 版有 +1 修正，差异可忽略且更标准）。
float IoU(const Detection::Bbox& a, const Detection::Bbox& b)
{
    const float a_x2       = a.x + a.w;
    const float a_y2       = a.y + a.h;
    const float b_x2       = b.x + b.w;
    const float b_y2       = b.y + b.h;
    const float inter_w    = std::max(0.0f, std::min(a_x2, b_x2) - std::max(a.x, b.x));
    const float inter_h    = std::max(0.0f, std::min(a_y2, b_y2) - std::max(a.y, b.y));
    const float inter      = inter_w * inter_h;
    const float union_area = a.w * a.h + b.w * b.h - inter;
    return union_area <= 0.0f ? 0.0f : inter / union_area;
}

}  // namespace

void DecodeQuantizedHead(const std::int8_t* data, std::size_t data_len, const Yolov5HeadSpec& spec,
                         int num_classes, std::vector<Detection>* out)
{
    if (out == nullptr || data == nullptr || num_classes <= 0 || spec.stride <= 0 ||
        spec.grid_h <= 0 || spec.grid_w <= 0 || spec.anchors == nullptr)
    {
        return;
    }
    const int         attributes = kBoxAttributeCount + num_classes;
    const std::size_t grid_len =
        static_cast<std::size_t>(spec.grid_h) * static_cast<std::size_t>(spec.grid_w);
    const std::size_t required =
        static_cast<std::size_t>(kAnchorCount) * static_cast<std::size_t>(attributes) * grid_len;
    if (data_len < required)
    {
        return;
    }

    for (int anchor = 0; anchor < kAnchorCount; ++anchor)
    {
        const std::size_t plane =
            static_cast<std::size_t>(anchor) * static_cast<std::size_t>(attributes) * grid_len;
        const float anchor_w = spec.anchors[anchor * 2 + 0];
        const float anchor_h = spec.anchors[anchor * 2 + 1];

        for (int row = 0; row < spec.grid_h; ++row)
        {
            for (int col = 0; col < spec.grid_w; ++col)
            {
                const std::size_t cell =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(spec.grid_w) +
                    static_cast<std::size_t>(col);
                const std::size_t base = plane + cell;
                const float       box_conf =
                    Sigmoid(Dequantize(data[base + 4 * grid_len], spec.zp, spec.scale));
                if (box_conf < spec.conf_threshold)
                {
                    continue;
                }

                // 类别 argmax：反量化为单调仿射变换，直接在 int8 上比较等价（与 A/B 一致）。
                std::int8_t max_prob  = data[base + 5 * grid_len];
                int         max_class = 0;
                for (int k = 1; k < num_classes; ++k)
                {
                    const std::int8_t prob =
                        data[base + static_cast<std::size_t>(5 + k) * grid_len];
                    if (prob > max_prob)
                    {
                        max_prob  = prob;
                        max_class = k;
                    }
                }
                const float class_conf = Sigmoid(Dequantize(max_prob, spec.zp, spec.scale));
                if (class_conf < spec.conf_threshold)
                {
                    continue;
                }

                const float tx =
                    Sigmoid(Dequantize(data[base + 0 * grid_len], spec.zp, spec.scale));
                const float ty =
                    Sigmoid(Dequantize(data[base + 1 * grid_len], spec.zp, spec.scale));
                const float tw =
                    Sigmoid(Dequantize(data[base + 2 * grid_len], spec.zp, spec.scale));
                const float th =
                    Sigmoid(Dequantize(data[base + 3 * grid_len], spec.zp, spec.scale));

                const float stride = static_cast<float>(spec.stride);
                const float cx     = (tx * 2.0f - 0.5f + static_cast<float>(col)) * stride;
                const float cy     = (ty * 2.0f - 0.5f + static_cast<float>(row)) * stride;
                const float half_w = tw * 2.0f;
                const float half_h = th * 2.0f;
                const float w      = half_w * half_w * anchor_w;
                const float h      = half_h * half_h * anchor_h;

                Detection detection;
                detection.class_id   = max_class;
                detection.bbox.x     = cx - w / 2.0f;
                detection.bbox.y     = cy - h / 2.0f;
                detection.bbox.w     = w;
                detection.bbox.h     = h;
                detection.confidence = box_conf * class_conf;
                out->push_back(detection);
            }
        }
    }
}

void NmsPerClass(std::vector<Detection>* detections, float iou_threshold)
{
    if (detections == nullptr || detections->empty())
    {
        return;
    }

    // 按类别分组：不同类别永远不互相抑制（修复 A/B 跨类抑制 bug）。
    std::map<std::int32_t, std::vector<Detection>> groups;
    for (Detection& detection : *detections)
    {
        groups[detection.class_id].push_back(std::move(detection));
    }

    std::vector<Detection> kept;
    for (auto& entry : groups)
    {
        std::vector<Detection>& candidates = entry.second;
        std::sort(candidates.begin(), candidates.end(), [](const Detection& a, const Detection& b)
                  { return a.confidence > b.confidence; });

        std::vector<bool> suppressed(candidates.size(), false);
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            if (suppressed[i])
            {
                continue;
            }
            kept.push_back(candidates[i]);
            for (std::size_t j = i + 1; j < candidates.size(); ++j)
            {
                if (!suppressed[j] && IoU(candidates[i].bbox, candidates[j].bbox) > iou_threshold)
                {
                    suppressed[j] = true;
                }
            }
        }
    }
    *detections = std::move(kept);
}

int PostprocessYolov5Quantized(const std::int8_t** head_data, const std::size_t* head_lens,
                               const Yolov5HeadSpec* heads, int num_classes, float nms_threshold,
                               std::vector<Detection>* out)
{
    if (out == nullptr || head_data == nullptr || head_lens == nullptr || heads == nullptr ||
        num_classes <= 0 || nms_threshold < 0.0f)
    {
        return -1;
    }
    out->clear();

    constexpr int kHeadCount = 3;
    // 严格校验：3 头缺一不可（A/B 契约假定 n_output==3），数据不足直接报错而非静默丢头。
    const int attributes = kBoxAttributeCount + num_classes;
    for (int h = 0; h < kHeadCount; ++h)
    {
        if (head_data[h] == nullptr || heads[h].stride <= 0 || heads[h].grid_h <= 0 ||
            heads[h].grid_w <= 0 || heads[h].anchors == nullptr)
        {
            return -1;
        }
        const std::size_t grid_len =
            static_cast<std::size_t>(heads[h].grid_h) * static_cast<std::size_t>(heads[h].grid_w);
        const std::size_t required = static_cast<std::size_t>(kAnchorCount) *
                                     static_cast<std::size_t>(attributes) * grid_len;
        if (head_lens[h] < required)
        {
            return -1;
        }
    }

    for (int h = 0; h < kHeadCount; ++h)
    {
        DecodeQuantizedHead(head_data[h], head_lens[h], heads[h], num_classes, out);
    }
    NmsPerClass(out, nms_threshold);
    return static_cast<int>(out->size());
}

}  // namespace animal_monitor::inference
