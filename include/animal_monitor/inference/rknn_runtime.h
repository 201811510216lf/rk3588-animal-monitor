#pragma once

// animal_monitor/inference/rknn_runtime.h
// 状态：hardware-blocked / UNVERIFIED —— 未在 RK3588 板卡编译运行验证。
// 说明：rknn_api.h（Rockchip RKNN 运行时）的 RAII 封装：
//   rknn_init / rknn_query / rknn_inputs_set / rknn_run / rknn_outputs_get /
//   rknn_outputs_release / rknn_destroy。
//   仅限目标构建：需定义 ANIMAL_MONITOR_ENABLE_TARGET_DEPS（CMake 接线时透传）。
//   禁止 exit(-1)：所有 rknn 返回值转换为 Status。

#if !defined(ANIMAL_MONITOR_ENABLE_TARGET_DEPS)
#error "rknn_runtime.h 仅限目标构建（需定义 ANIMAL_MONITOR_ENABLE_TARGET_DEPS）"
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rknn_api.h"

#include "animal_monitor/common/status.h"
#include "animal_monitor/inference/i_inference_model.h"
#include "animal_monitor/inference/model_manifest.h"
#include "animal_monitor/inference/yolov5_postprocess.h"

namespace animal_monitor::inference {

// rknn_outputs_get 所得缓冲的 RAII 持有：析构自动 rknn_outputs_release。
class RknnOutputs
{
  public:
    RknnOutputs() = default;
    ~RknnOutputs() { Release(); }

    RknnOutputs(const RknnOutputs&)            = delete;
    RknnOutputs& operator=(const RknnOutputs&) = delete;

    void Release();

    const std::vector<rknn_output>& Outputs() const { return outputs_; }

    // 供 RknnRuntime::Run 填充（内部接口）。
    void Attach(rknn_context context, std::vector<rknn_output> outputs)
    {
        Release();
        context_ = context;
        outputs_ = std::move(outputs);
    }

  private:
    rknn_context             context_ = nullptr;
    std::vector<rknn_output> outputs_;
};

// rknn_context 的 RAII 封装。InitFromFile 失败后不可用；析构自动 rknn_destroy。
class RknnRuntime
{
  public:
    RknnRuntime() = default;
    ~RknnRuntime();

    RknnRuntime(const RknnRuntime&)            = delete;
    RknnRuntime& operator=(const RknnRuntime&) = delete;

    // 加载模型文件并初始化：rknn_init → rknn_set_core_mask → query 版本与输入/输出属性。
    // npu_core_mask 为 RK3588 位掩码（core0=1|core1=2|core2=4，0=自动）。
    Status InitFromFile(const std::string& model_path, std::uint32_t npu_core_mask);

    bool IsReady() const { return context_ != nullptr; }

    const std::vector<rknn_tensor_attr>& InputAttrs() const { return input_attrs_; }
    const std::vector<rknn_tensor_attr>& OutputAttrs() const { return output_attrs_; }

    // 同步推理：inputs_set → run → outputs_get。outputs 为 RAII 输出持有
    // （析构自动 outputs_release）。want_float 控制输出是否反量化为 float。
    Status Run(const rknn_input* inputs, std::uint32_t n_inputs, bool want_float,
               RknnOutputs* outputs);

  private:
    rknn_context                  context_ = nullptr;
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
};

// manifest + RknnRuntime 的 yolov5 检测模型实现（IInferenceModel）。
// 预处理假设与限制（均 UNVERIFIED，待目标板实测修正，见 src/inference/README.md）：
//   - 输入假设 RGB888；BGR888 输入自动交换 R/B 通道；
//   - manifest.letterbox=false 时直接双线性拉伸（A/B 用 cv::resize 等效）；
//   - mean/std 非空返回 kUnimplemented（Phase 3 未实现归一化）；
//   - 仅支持 int8 输出（wants_float=true 返回 kUnimplemented）；
//   - 3 输出头，stride 由输出字节数与输入尺寸反推（8/16/32），不依赖输出顺序；
//   - 解码公式见 yolov5_postprocess.h（sigmoid 版本，与 A/B 实际代码差异待板卡实测）。
class RknnYolov5Model : public IInferenceModel
{
  public:
    // 构造后稳定；Load 成功后为 manifest.model_id（空则保持默认）。
    const char* Name() const override { return name_.c_str(); }

    Status        Load(const ModelManifest& manifest) override;
    Status        Infer(const VideoFrame& frame, InferenceResult* result) override;
    ModelManifest Manifest() const override;

  private:
    struct HeadInfo
    {
        Yolov5HeadSpec spec;
        std::size_t    output_index = 0;  // 对应 output_attrs 下标（与 manifest 输出顺序解耦）
    };

    // 校验输出张量尺寸并反推 stride 与类别数（UNVERIFIED，板卡实测确认）。
    Status BuildHeadSpecs();

    std::string           name_ = "rknn_yolov5";
    ModelManifest         manifest_;
    RknnRuntime           runtime_;
    std::vector<HeadInfo> heads_;
    int                   num_classes_    = 0;
    std::size_t           input_height_   = 0;
    std::size_t           input_width_    = 0;
    std::size_t           input_channels_ = 3;
};

}  // namespace animal_monitor::inference
