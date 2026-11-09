#pragma once

// animal_monitor/inference/yolov5_postprocess.h
// 状态：implemented（host 可编译，纯逻辑，无 RKNN/OpenCV 依赖）
// 说明：yolov5 量化(int8)输出后处理——修复 A/B 版全部已审缺陷的重写实现：
//   - confidence 必须赋值（审计 B 缺陷 #2：prop 丢失 + 恒用 labels[0]）；
//   - NMS 按类别分组，修复跨类抑制 bug（审计 A #8：内层条件恒假）；
//   - 无全局静态 labels / 无 memset(sizeof(指针)) 类竞态与越界（审计 A/B #10）；
//   - 解码公式按任务书（与官方 rknn_model_zoo 一致）：
//       cx = (sigmoid(tx)*2 - 0.5 + grid_x) * stride
//       w  = (sigmoid(tw)*2)^2 * anchor_w
//     confidence = sigmoid(box_conf) * sigmoid(class_conf)。
//     注意：A/B 实际代码省略了 sigmoid（relu 模型输出疑似已内置 sigmoid 激活），
//     本实现遵循任务书公式，需目标板实测确认（UNVERIFIED，见 src/inference/README.md）。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "animal_monitor/inference/inference_types.h"

namespace animal_monitor::inference
{

// 单输出头解码参数。anchors 指向 3 组宽高共 6 个 float（生命周期覆盖调用）。
struct Yolov5HeadSpec
{
    int          stride         = 0;        // 8 / 16 / 32
    const float* anchors        = nullptr;  // 3 组 (w, h)，共 6 值
    int          grid_h         = 0;
    int          grid_w         = 0;
    float        scale          = 1.0f;   // 量化 scale（rknn 输出张量属性）
    std::int32_t zp             = 0;      // 量化零点
    float        conf_threshold = 0.25f;  // box_conf 与 class_conf 的双重阈值门
};

// 解码单个量化输出头。数据布局与 A/B 实测索引一致（每通道平面 grid_len 连续，
// 扁平 NCHW）：index(c, cell) = c * grid_len + cell，c = anchor * (5 + num_classes) + attr。
// 反量化：deqnt = (int8 - zp) * scale。data_len 不足或参数非法时静默返回（不追加输出）。
void DecodeQuantizedHead(const std::int8_t* data, std::size_t data_len, const Yolov5HeadSpec& spec,
                         int num_classes, std::vector<Detection>* out);

// 按类别分组做 NMS：组内按 confidence 降序贪心抑制（IoU > threshold 抑制），
// 不同类别互不抑制（修复 A/B 跨类抑制 bug）。
void NmsPerClass(std::vector<Detection>* detections, float iou_threshold);

// 三头 yolov5 量化后处理：解码 → 拼接 → 按类 NMS。
// 返回最终检测数量；参数非法（空指针 / num_classes<=0 / 任一数据长度不足等）返回 -1。
int PostprocessYolov5Quantized(const std::int8_t** head_data, const std::size_t* head_lens,
                               const Yolov5HeadSpec* heads, int num_classes, float nms_threshold,
                               std::vector<Detection>* out);

}  // namespace animal_monitor::inference
