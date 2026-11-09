// animal_monitor/inference/rknn_runtime.cpp
// 状态：hardware-blocked / UNVERIFIED —— 未在 RK3588 板卡编译运行验证。
// 本文件仅参与目标构建（ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON），宿主不编译。
// 设计依据：源项目 A/B 的 rknnPool.hpp（rknn_lite）与 postprocess.cpp 契约，
// 修复其 exit(-1)、返回值不检查、假定 n_output==3 等缺陷（审计 A #7）。
#include "animal_monitor/inference/rknn_runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace animal_monitor::inference {

namespace {

// yolov5s 标准锚点（A/B 实测使用，COCO 训练尺寸；UNVERIFIED：板卡实测确认）。
constexpr std::array<float, 6> kAnchorsStride8  = {10.0f,  13.0f, 16.0f, 30.0f, 33.0f,  23.0f};
constexpr std::array<float, 6> kAnchorsStride16 = {30.0f,  61.0f, 62.0f, 45.0f, 59.0f,  119.0f};
constexpr std::array<float, 6> kAnchorsStride32 = {116.0f, 90.0f, 156.0f, 198.0f, 373.0f, 326.0f};

constexpr std::array<int, 3> kSupportedStrides = {8, 16, 32};

std::string RknnError(const char* api, int ret)
{
    return std::string(api) + " 失败 ret=" + std::to_string(ret);
}

Status ReadFileBinary(const std::string& path, std::vector<std::uint8_t>* out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return Status(StatusCode::kNotFound, "cannot open model file: " + path);
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0)
    {
        return Status(StatusCode::kConfigurationError, "模型文件为空: " + path);
    }
    file.seekg(0, std::ios::beg);
    out->resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size));
    if (file.bad())
    {
        return Status(StatusCode::kUnavailable, "读取模型文件失败: " + path);
    }
    return Status::MakeOk();
}

// 双线性拉伸 RGB888（无 letterbox，UNVERIFIED：A/B 用 cv::resize INTER_LINEAR 等效）。
// bgr_input=true 时同时完成 BGR→RGB 通道交换。
void StretchRgb888(const std::uint8_t* src, std::size_t src_w, std::size_t src_h,
                   std::size_t src_row_stride, bool bgr_input, std::uint8_t* dst,
                   std::size_t dst_w, std::size_t dst_h)
{
    for (std::size_t dy = 0; dy < dst_h; ++dy)
    {
        const float src_y = (static_cast<float>(dy) + 0.5f) * static_cast<float>(src_h) /
                                static_cast<float>(dst_h) -
                            0.5f;
        const float       clamped_y = std::max(0.0f, src_y);
        const std::size_t y0        = static_cast<std::size_t>(clamped_y);
        const std::size_t y1        = std::min(y0 + 1, src_h - 1);
        const float       wy        = clamped_y - static_cast<float>(y0);
        for (std::size_t dx = 0; dx < dst_w; ++dx)
        {
            const float src_x = (static_cast<float>(dx) + 0.5f) * static_cast<float>(src_w) /
                                    static_cast<float>(dst_w) -
                                0.5f;
            const float       clamped_x = std::max(0.0f, src_x);
            const std::size_t x0        = static_cast<std::size_t>(clamped_x);
            const std::size_t x1        = std::min(x0 + 1, src_w - 1);
            const float       wx        = clamped_x - static_cast<float>(x0);
            for (std::size_t c = 0; c < 3; ++c)
            {
                const std::size_t src_c = bgr_input ? (2 - c) : c;
                const float       v00   = static_cast<float>(src[y0 * src_row_stride + x0 * 3 + src_c]);
                const float       v01   = static_cast<float>(src[y0 * src_row_stride + x1 * 3 + src_c]);
                const float       v10   = static_cast<float>(src[y1 * src_row_stride + x0 * 3 + src_c]);
                const float       v11   = static_cast<float>(src[y1 * src_row_stride + x1 * 3 + src_c]);
                const float       top   = v00 * (1.0f - wx) + v01 * wx;
                const float       bottom = v10 * (1.0f - wx) + v11 * wx;
                const float       value  = top * (1.0f - wy) + bottom * wy;
                dst[(dy * dst_w + dx) * 3 + c] = static_cast<std::uint8_t>(
                    std::max(0.0f, std::min(255.0f, value + 0.5f)));
            }
        }
    }
}

}  // namespace

RknnRuntime::~RknnRuntime()
{
    if (context_ != nullptr)
    {
        rknn_destroy(context_);
        context_ = nullptr;
    }
}

Status RknnRuntime::InitFromFile(const std::string& model_path, std::uint32_t npu_core_mask)
{
    std::vector<std::uint8_t> model_data;
    Status                    status = ReadFileBinary(model_path, &model_data);
    if (!status)
    {
        return status;
    }

    int ret = rknn_init(&context_, model_data.data(), static_cast<uint32_t>(model_data.size()),
                        0, nullptr);
    if (ret != RKNN_SUCC)
    {
        context_ = nullptr;
        return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_init", ret));
    }

    // NPU 核绑定：core_mask=0 为自动；其余为 core0=1|core1=2|core2=4 位掩码。
    if (npu_core_mask != 0)
    {
        ret = rknn_set_core_mask(context_, static_cast<rknn_core_mask>(npu_core_mask));
        if (ret != RKNN_SUCC)
        {
            return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_set_core_mask", ret));
        }
    }

    rknn_sdk_version version{};
    ret = rknn_query(context_, RKNN_QUERY_SDK_VERSION, &version, sizeof(version));
    if (ret != RKNN_SUCC)
    {
        return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_query SDK_VERSION", ret));
    }

    rknn_input_output_num io_num{};
    ret = rknn_query(context_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_query IN_OUT_NUM", ret));
    }

    input_attrs_.resize(io_num.n_input);
    for (std::uint32_t i = 0; i < io_num.n_input; ++i)
    {
        input_attrs_[i].index = i;
        ret = rknn_query(context_, RKNN_QUERY_INPUT_ATTR, &input_attrs_[i],
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_query INPUT_ATTR", ret));
        }
    }
    output_attrs_.resize(io_num.n_output);
    for (std::uint32_t i = 0; i < io_num.n_output; ++i)
    {
        output_attrs_[i].index = i;
        ret = rknn_query(context_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[i],
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_query OUTPUT_ATTR", ret));
        }
    }
    return Status::MakeOk();
}

Status RknnRuntime::Run(const rknn_input* inputs, std::uint32_t n_inputs, bool want_float,
                        RknnOutputs* outputs)
{
    if (context_ == nullptr)
    {
        return Status(StatusCode::kUnavailable, "RknnRuntime 未初始化");
    }
    if (inputs == nullptr || n_inputs == 0 || outputs == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "RknnRuntime::Run 参数非法");
    }

    int ret = rknn_inputs_set(context_, n_inputs, const_cast<rknn_input*>(inputs));
    if (ret != RKNN_SUCC)
    {
        return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_inputs_set", ret));
    }
    ret = rknn_run(context_, nullptr);
    if (ret != RKNN_SUCC)
    {
        return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_run", ret));
    }

    std::vector<rknn_output> outs(output_attrs_.size());
    for (std::size_t i = 0; i < outs.size(); ++i)
    {
        outs[i] = rknn_output{};
        outs[i].index      = static_cast<std::uint32_t>(i);
        outs[i].want_float = want_float ? 1 : 0;
    }
    ret = rknn_outputs_get(context_, static_cast<std::uint32_t>(outs.size()), outs.data(),
                           nullptr);
    if (ret != RKNN_SUCC)
    {
        return Status(StatusCode::kHardwareUnavailable, RknnError("rknn_outputs_get", ret));
    }
    outputs->Attach(context_, std::move(outs));
    return Status::MakeOk();
}

void RknnOutputs::Release()
{
    if (context_ != nullptr && !outputs_.empty())
    {
        rknn_outputs_release(context_, static_cast<std::uint32_t>(outputs_.size()),
                             outputs_.data());
    }
    context_ = nullptr;
    outputs_.clear();
}

Status RknnYolov5Model::Load(const ModelManifest& manifest)
{
    if (manifest.model_id.empty())
    {
        return Status(StatusCode::kInvalidArgument, "manifest.model_id 为空");
    }
    if (manifest.inputs.empty())
    {
        return Status(StatusCode::kConfigurationError, "manifest 未配置输入张量");
    }
    if (manifest.rknn.wants_float)
    {
        return Status(StatusCode::kUnimplemented,
                      "wants_float 输出的浮点后处理未实现（Phase 3 仅支持 int8）");
    }
    if (!manifest.inputs[0].mean.empty() || !manifest.inputs[0].std.empty())
    {
        return Status(StatusCode::kUnimplemented, "mean/std 归一化未实现（Phase 3）");
    }

    name_     = manifest.model_id;
    manifest_ = manifest;

    Status status = runtime_.InitFromFile(manifest.file,
                                          static_cast<std::uint32_t>(manifest.rknn.npu_core_mask));
    if (!status)
    {
        return status;
    }

    // 输入尺寸：优先 manifest.shape；缺失则取 rknn_query 属性（UNVERIFIED）。
    const ModelInputSpec& input_spec = manifest.inputs[0];
    if (!input_spec.shape.empty() && input_spec.shape.size() >= 4)
    {
        if (input_spec.layout == "NHWC")
        {
            input_height_   = static_cast<std::size_t>(input_spec.shape[1]);
            input_width_    = static_cast<std::size_t>(input_spec.shape[2]);
            input_channels_ = static_cast<std::size_t>(input_spec.shape[3]);
        }
        else if (input_spec.layout == "NCHW")
        {
            input_channels_ = static_cast<std::size_t>(input_spec.shape[1]);
            input_height_   = static_cast<std::size_t>(input_spec.shape[2]);
            input_width_    = static_cast<std::size_t>(input_spec.shape[3]);
        }
        else
        {
            return Status(StatusCode::kConfigurationError,
                          "不支持输入 layout: " + input_spec.layout);
        }
    }
    else if (!runtime_.InputAttrs().empty())
    {
        const rknn_tensor_attr& attr = runtime_.InputAttrs()[0];
        if (attr.fmt == RKNN_TENSOR_NHWC)
        {
            input_height_   = attr.dims[1];
            input_width_    = attr.dims[2];
            input_channels_ = attr.dims[3];
        }
        else
        {
            input_channels_ = attr.dims[1];
            input_height_   = attr.dims[2];
            input_width_    = attr.dims[3];
        }
    }
    if (input_height_ == 0 || input_width_ == 0 || input_channels_ != 3)
    {
        return Status(StatusCode::kConfigurationError, "输入尺寸无效（需 3 通道 RGB）");
    }
    if (input_spec.color_format != "RGB" && input_spec.color_format != "BGR")
    {
        return Status(StatusCode::kUnimplemented,
                      "仅支持 RGB/BGR888 输入（color_format=" + input_spec.color_format + "）");
    }

    return BuildHeadSpecs();
}

Status RknnYolov5Model::BuildHeadSpecs()
{
    const std::vector<rknn_tensor_attr>& output_attrs = runtime_.OutputAttrs();
    if (output_attrs.size() != 3)
    {
        return Status(StatusCode::kConfigurationError,
                      "yolov5 契约要求 3 个输出头，实际 " + std::to_string(output_attrs.size()));
    }

    const int known_classes = static_cast<int>(manifest_.classes.size());
    int       num_classes   = 0;

    heads_.clear();
    std::array<bool, 3> stride_used{false, false, false};
    for (std::size_t o = 0; o < output_attrs.size(); ++o)
    {
        // 反推 stride：size == 3 * (5 + nc) * (in_h/s) * (in_w/s)。
        int matched_stride = 0;
        int matched_nc     = 0;
        for (std::size_t s = 0; s < kSupportedStrides.size(); ++s)
        {
            const int stride = kSupportedStrides[s];
            if (input_height_ % static_cast<std::size_t>(stride) != 0 ||
                input_width_ % static_cast<std::size_t>(stride) != 0)
            {
                continue;
            }
            const std::size_t grid_len =
                (input_height_ / static_cast<std::size_t>(stride)) *
                (input_width_ / static_cast<std::size_t>(stride));
            const std::size_t plane_len = 3 * grid_len;
            const std::size_t size      = output_attrs[o].size;
            if (plane_len == 0 || size % plane_len != 0 || size / plane_len < 6)
            {
                continue;
            }
            const int nc = static_cast<int>(size / plane_len) - 5;
            if (nc < 1 || (known_classes > 0 && nc != known_classes))
            {
                continue;
            }
            if (matched_stride != 0)
            {
                return Status(StatusCode::kConfigurationError,
                              "输出张量尺寸与多档 stride 歧义，需以 rknn_query 实测修正");
            }
            matched_stride = stride;
            matched_nc     = nc;
        }
        if (matched_stride == 0)
        {
            return Status(StatusCode::kConfigurationError,
                          "输出张量尺寸与 yolov5 3 头假设不符（size=" +
                              std::to_string(output_attrs[o].size) + "），需板卡实测修正");
        }
        if (num_classes == 0)
        {
            num_classes = matched_nc;
        }
        else if (num_classes != matched_nc)
        {
            return Status(StatusCode::kConfigurationError, "各输出头类别数不一致");
        }

        std::size_t index = kSupportedStrides.size();
        for (std::size_t s = 0; s < kSupportedStrides.size(); ++s)
        {
            if (kSupportedStrides[s] == matched_stride)
            {
                index = s;
                break;
            }
        }
        if (index >= stride_used.size() || stride_used[index])
        {
            return Status(StatusCode::kConfigurationError, "输出头 stride 重复");
        }
        stride_used[index] = true;

        const float* anchors = kAnchorsStride8.data();
        if (matched_stride == 16)
        {
            anchors = kAnchorsStride16.data();
        }
        else if (matched_stride == 32)
        {
            anchors = kAnchorsStride32.data();
        }

        Yolov5HeadSpec spec;
        spec.stride         = matched_stride;
        spec.anchors        = anchors;
        spec.grid_h         = static_cast<int>(input_height_ / static_cast<std::size_t>(matched_stride));
        spec.grid_w         = static_cast<int>(input_width_ / static_cast<std::size_t>(matched_stride));
        spec.scale          = output_attrs[o].scale;
        spec.zp             = output_attrs[o].zp;
        spec.conf_threshold = static_cast<float>(manifest_.confidence_threshold);
        heads_.push_back(HeadInfo{spec, o});
    }
    for (const bool used : stride_used)
    {
        if (!used)
        {
            return Status(StatusCode::kConfigurationError, "未覆盖全部 8/16/32 输出头");
        }
    }
    num_classes_ = num_classes;
    return Status::MakeOk();
}

Status RknnYolov5Model::Infer(const VideoFrame& frame, InferenceResult* result)
{
    if (result == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "RknnYolov5Model::Infer: result 为空");
    }
    *result = InferenceResult{};
    if (!runtime_.IsReady())
    {
        return Status(StatusCode::kUnavailable, "RknnYolov5Model 未加载");
    }
    if (frame.planes.empty() || frame.planes[0].data == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "帧无 CPU 可访问平面数据");
    }
    const bool bgr_input = frame.meta.format == PixelFormat::kBgr888;
    if (!bgr_input && frame.meta.format != PixelFormat::kRgb888)
    {
        return Status(StatusCode::kUnimplemented, "仅支持 RGB/BGR888 输入（NV12 路径待 RGA）");
    }

    const std::size_t src_w = frame.meta.width;
    const std::size_t src_h = frame.meta.height;
    if (src_w == 0 || src_h == 0)
    {
        return Status(StatusCode::kInvalidArgument, "帧尺寸元数据无效");
    }
    const std::size_t src_stride =
        frame.planes[0].row_stride != 0 ? frame.planes[0].row_stride : src_w * 3;
    const std::size_t required = src_stride * (src_h - 1) + src_w * 3;
    if (frame.planes[0].size < required)
    {
        return Status(StatusCode::kInvalidArgument, "帧缓冲不足（planes[0].size < 期望）");
    }

    // 预处理：BGR→RGB（如需）+ 双线性拉伸到模型输入尺寸（letterbox=false，UNVERIFIED）。
    std::vector<std::uint8_t> input_buffer(input_height_ * input_width_ * input_channels_);
    StretchRgb888(frame.planes[0].data, src_w, src_h, src_stride, bgr_input,
                  input_buffer.data(), input_width_, input_height_);

    rknn_input input{};
    input.index        = 0;
    input.buf          = input_buffer.data();
    input.size         = static_cast<std::uint32_t>(input_buffer.size());
    input.pass_through = 0;  // 与 A/B 一致：内部拷贝，非零拷贝（待 RGA 优化）
    input.type         = RKNN_TENSOR_UINT8;
    input.fmt          = RKNN_TENSOR_NHWC;

    RknnOutputs outputs;
    Status      status = runtime_.Run(&input, 1, false, &outputs);
    if (!status)
    {
        return status;
    }

    std::array<const std::int8_t*, 3> head_data{nullptr, nullptr, nullptr};
    std::array<std::size_t, 3>        head_lens{0, 0, 0};
    std::array<Yolov5HeadSpec, 3>     head_specs{};
    const std::vector<rknn_output>&   outs = outputs.Outputs();
    for (std::size_t h = 0; h < heads_.size(); ++h)
    {
        const rknn_output& out = outs[heads_[h].output_index];
        head_data[h] = reinterpret_cast<const std::int8_t*>(out.buf);
        head_lens[h] = out.size;
        head_specs[h] = heads_[h].spec;
    }

    const int count = PostprocessYolov5Quantized(head_data.data(), head_lens.data(),
                                                 head_specs.data(), num_classes_,
                                                 static_cast<float>(manifest_.nms_threshold),
                                                 &result->detections);
    if (count < 0)
    {
        return Status(StatusCode::kInternal, "yolov5 后处理失败（参数或长度校验不通过）");
    }
    result->valid         = true;
    result->model_id      = name_;
    result->model_version = manifest_.version;
    return Status::MakeOk();
}

ModelManifest RknnYolov5Model::Manifest() const
{
    return manifest_;
}

}  // namespace animal_monitor::inference
