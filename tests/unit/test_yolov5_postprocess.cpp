// tests/unit/test_yolov5_postprocess.cpp
// 状态：implemented（host）
// 说明：合成 int8 数据验证解码公式（sigmoid 版本）、量化反量化、
//       confidence = box_conf * class_conf、按类 NMS 与三头后处理入口。

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "test_harness.h"

#include "animal_monitor/inference/yolov5_postprocess.h"

using animal_monitor::inference::DecodeQuantizedHead;
using animal_monitor::inference::Detection;
using animal_monitor::inference::NmsPerClass;
using animal_monitor::inference::PostprocessYolov5Quantized;
using animal_monitor::inference::Yolov5HeadSpec;

namespace {

constexpr float kAnchorStride8[6]  = {10.0f, 13.0f, 16.0f, 30.0f, 33.0f, 23.0f};
constexpr float kAnchorStride16[6] = {30.0f, 61.0f, 62.0f, 45.0f, 59.0f, 119.0f};
constexpr float kAnchorStride32[6] = {116.0f, 90.0f, 156.0f, 198.0f, 373.0f, 326.0f};

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// 1x1 网格：index(anchor, attr, cell=0) = anchor * (5 + num_classes) + attr。
std::size_t CellIndex(int anchor, int attr, int num_classes)
{
    return static_cast<std::size_t>(anchor) * static_cast<std::size_t>(5 + num_classes) +
           static_cast<std::size_t>(attr);
}

Yolov5HeadSpec MakeSpec(int stride, const float* anchors, float scale, std::int32_t zp)
{
    Yolov5HeadSpec spec;
    spec.stride  = stride;
    spec.anchors = anchors;
    spec.grid_h  = 1;
    spec.grid_w  = 1;
    spec.scale   = scale;
    spec.zp      = zp;
    return spec;
}

AM_TEST(decode_known_values_grid_1x1)
{
    // zp=0、scale=1：tx=ty=tw=th=0 → sigmoid=0.5。
    // cx = (2*0.5 - 0.5 + 0) * 8 = 4；w = (2*0.5)^2 * 10 = 10；h = 13。
    std::vector<std::int8_t> data(3 * 6, 0);
    data[CellIndex(0, 4, 1)] = 100;   // box_conf：sigmoid(100) ≈ 1
    data[CellIndex(0, 5, 1)] = 100;   // class_conf
    data[CellIndex(1, 4, 1)] = -100;  // 其余 anchor 压低，避免 0.5 幻影检测
    data[CellIndex(2, 4, 1)] = -100;

    std::vector<Detection> detections;
    DecodeQuantizedHead(data.data(), data.size(), MakeSpec(8, kAnchorStride8, 1.0f, 0), 1,
                        &detections);
    AM_EXPECT_EQ(detections.size(), 1u);
    if (detections.size() != 1)
    {
        return;
    }
    const Detection& d = detections[0];
    AM_EXPECT_EQ(d.class_id, 0);
    AM_EXPECT(Near(d.bbox.x, -1.0f));
    AM_EXPECT(Near(d.bbox.y, -2.5f));
    AM_EXPECT(Near(d.bbox.w, 10.0f));
    AM_EXPECT(Near(d.bbox.h, 13.0f));
    AM_EXPECT(Near(d.confidence, 1.0f));
}

AM_TEST(dequantize_applied_affine)
{
    // zp=10、scale=0.5：坐标通道取 10 → deqnt 0 → 坐标同上；
    // 置信度通道取 20 → deqnt 5 → sigmoid(5)^2。
    std::vector<std::int8_t> data(3 * 6, 0);
    data[CellIndex(0, 0, 1)] = 10;
    data[CellIndex(0, 1, 1)] = 10;
    data[CellIndex(0, 2, 1)] = 10;
    data[CellIndex(0, 3, 1)] = 10;
    data[CellIndex(0, 4, 1)] = 20;
    data[CellIndex(0, 5, 1)] = 20;

    std::vector<Detection> detections;
    DecodeQuantizedHead(data.data(), data.size(), MakeSpec(8, kAnchorStride8, 0.5f, 10), 1,
                        &detections);
    AM_EXPECT_EQ(detections.size(), 1u);
    if (detections.size() != 1)
    {
        return;
    }
    const float      s5 = 1.0f / (1.0f + std::exp(-5.0f));
    const Detection& d  = detections[0];
    AM_EXPECT(Near(d.bbox.x, -1.0f));
    AM_EXPECT(Near(d.bbox.y, -2.5f));
    AM_EXPECT(Near(d.bbox.w, 10.0f));
    AM_EXPECT(Near(d.bbox.h, 13.0f));
    AM_EXPECT(Near(d.confidence, s5 * s5));
}

AM_TEST(threshold_gates_box_and_class)
{
    // box_conf 低于阈值 → 无检测。
    std::vector<std::int8_t> data(3 * 6, 0);
    data[CellIndex(0, 4, 1)] = -100;
    data[CellIndex(0, 5, 1)] = 100;
    data[CellIndex(1, 4, 1)] = -100;
    data[CellIndex(2, 4, 1)] = -100;
    std::vector<Detection> detections;
    DecodeQuantizedHead(data.data(), data.size(), MakeSpec(8, kAnchorStride8, 1.0f, 0), 1,
                        &detections);
    AM_EXPECT_EQ(detections.size(), 0u);

    // class_conf 低于阈值 → 无检测（A/B 双门语义）。
    data[CellIndex(0, 4, 1)] = 100;
    data[CellIndex(0, 5, 1)] = -100;
    DecodeQuantizedHead(data.data(), data.size(), MakeSpec(8, kAnchorStride8, 1.0f, 0), 1,
                        &detections);
    AM_EXPECT_EQ(detections.size(), 0u);
}

AM_TEST(class_argmax_selects_max_channel)
{
    std::vector<std::int8_t> data(3 * (5 + 3), 0);
    data[CellIndex(0, 4, 3)] = 100;  // box_conf
    data[CellIndex(0, 5, 3)] = -10;  // class 0
    data[CellIndex(0, 6, 3)] = 50;   // class 1 ← argmax
    data[CellIndex(0, 7, 3)] = -5;   // class 2
    data[CellIndex(1, 4, 3)] = -100;
    data[CellIndex(2, 4, 3)] = -100;

    std::vector<Detection> detections;
    DecodeQuantizedHead(data.data(), data.size(), MakeSpec(8, kAnchorStride8, 1.0f, 0), 3,
                        &detections);
    AM_EXPECT_EQ(detections.size(), 1u);
    if (!detections.empty())
    {
        AM_EXPECT_EQ(detections[0].class_id, 1);
    }
}

AM_TEST(nms_same_class_suppresses_overlap)
{
    std::vector<Detection> detections;
    Detection              a;
    a.class_id   = 0;
    a.bbox       = Detection::Bbox{0.0f, 0.0f, 10.0f, 10.0f};
    a.confidence = 0.9f;
    Detection b;
    b.class_id   = 0;
    b.bbox       = Detection::Bbox{1.0f, 1.0f, 10.0f, 10.0f};
    b.confidence = 0.8f;
    detections.push_back(a);
    detections.push_back(b);

    NmsPerClass(&detections, 0.45f);
    AM_EXPECT_EQ(detections.size(), 1u);
    if (!detections.empty())
    {
        AM_EXPECT(Near(detections[0].confidence, 0.9f));
    }
}

AM_TEST(nms_different_class_keeps_both)
{
    // 修复跨类抑制 bug：同类 IoU=1 但类别不同 → 必须都保留。
    std::vector<Detection> detections;
    Detection              a;
    a.class_id   = 0;
    a.bbox       = Detection::Bbox{0.0f, 0.0f, 10.0f, 10.0f};
    a.confidence = 0.9f;
    Detection b;
    b.class_id   = 1;
    b.bbox       = Detection::Bbox{0.0f, 0.0f, 10.0f, 10.0f};
    b.confidence = 0.8f;
    detections.push_back(a);
    detections.push_back(b);

    NmsPerClass(&detections, 0.45f);
    AM_EXPECT_EQ(detections.size(), 2u);
}

AM_TEST(postprocess_three_heads_keeps_per_class)
{
    const int num_classes = 3;
    // 每头 1x1 网格：head0→class0、head1→class1、head2→class2，类不同互不抑制。
    auto make_head_data = [num_classes](int selected_class)
    {
        std::vector<std::int8_t> data(3 * (5 + num_classes), 0);
        data[CellIndex(0, 4, num_classes)] = 100;
        data[CellIndex(1, 4, num_classes)] = -100;
        data[CellIndex(2, 4, num_classes)] = -100;
        for (int k = 0; k < num_classes; ++k)
        {
            data[CellIndex(0, 5 + k, num_classes)] =
                static_cast<std::int8_t>(k == selected_class ? 100 : -10);
        }
        return data;
    };

    const std::vector<std::int8_t> d0 = make_head_data(0);
    const std::vector<std::int8_t> d1 = make_head_data(1);
    const std::vector<std::int8_t> d2 = make_head_data(2);

    const std::int8_t* data[3]  = {d0.data(), d1.data(), d2.data()};
    const std::size_t  lens[3]  = {d0.size(), d1.size(), d2.size()};
    Yolov5HeadSpec     specs[3] = {MakeSpec(8, kAnchorStride8, 1.0f, 0),
                                   MakeSpec(16, kAnchorStride16, 1.0f, 0),
                                   MakeSpec(32, kAnchorStride32, 1.0f, 0)};

    std::vector<Detection> detections;
    const int              count =
        PostprocessYolov5Quantized(data, lens, specs, num_classes, 0.45f, &detections);
    AM_EXPECT_EQ(count, 3);
    AM_EXPECT_EQ(detections.size(), 3u);
}

AM_TEST(postprocess_suppresses_duplicate_head)
{
    // head0 与 head1 同 stride/锚点/类别（完全重合）→ NMS 保留 1；head2 类不同保留。
    const int num_classes    = 3;
    auto      make_head_data = [num_classes](int selected_class)
    {
        std::vector<std::int8_t> data(3 * (5 + num_classes), 0);
        data[CellIndex(0, 4, num_classes)] = 100;
        data[CellIndex(1, 4, num_classes)] = -100;
        data[CellIndex(2, 4, num_classes)] = -100;
        for (int k = 0; k < num_classes; ++k)
        {
            data[CellIndex(0, 5 + k, num_classes)] =
                static_cast<std::int8_t>(k == selected_class ? 100 : -10);
        }
        return data;
    };

    const std::vector<std::int8_t> d0 = make_head_data(0);
    const std::vector<std::int8_t> d1 = make_head_data(0);
    const std::vector<std::int8_t> d2 = make_head_data(2);

    const std::int8_t* data[3]  = {d0.data(), d1.data(), d2.data()};
    const std::size_t  lens[3]  = {d0.size(), d1.size(), d2.size()};
    Yolov5HeadSpec     specs[3] = {MakeSpec(8, kAnchorStride8, 1.0f, 0),
                                   MakeSpec(8, kAnchorStride8, 1.0f, 0),
                                   MakeSpec(32, kAnchorStride32, 1.0f, 0)};

    std::vector<Detection> detections;
    const int              count =
        PostprocessYolov5Quantized(data, lens, specs, num_classes, 0.45f, &detections);
    AM_EXPECT_EQ(count, 2);
    AM_EXPECT_EQ(detections.size(), 2u);
}

AM_TEST(postprocess_rejects_invalid_arguments)
{
    std::vector<Detection> detections;
    AM_EXPECT_EQ(PostprocessYolov5Quantized(nullptr, nullptr, nullptr, 1, 0.45f, &detections), -1);

    const std::int8_t* data[3]  = {nullptr, nullptr, nullptr};
    std::size_t        lens[3]  = {0, 0, 0};
    Yolov5HeadSpec     specs[3] = {MakeSpec(8, kAnchorStride8, 1.0f, 0),
                                   MakeSpec(16, kAnchorStride16, 1.0f, 0),
                                   MakeSpec(32, kAnchorStride32, 1.0f, 0)};
    AM_EXPECT_EQ(PostprocessYolov5Quantized(data, lens, specs, 1, 0.45f, &detections), -1);

    // 数据长度不足 → -1。
    std::vector<std::int8_t> short_data(4, 0);
    data[0] = short_data.data();
    data[1] = short_data.data();
    data[2] = short_data.data();
    lens[0] = short_data.size();
    lens[1] = short_data.size();
    lens[2] = short_data.size();
    AM_EXPECT_EQ(PostprocessYolov5Quantized(data, lens, specs, 1, 0.45f, &detections), -1);
}

}  // namespace

int main() { return am_test::RunAll(); }
