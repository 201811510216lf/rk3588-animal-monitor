#pragma once

// animal_monitor/inference/model_manifest.h
// 状态：implemented（host 可编译）
// 说明：模型 manifest 契约，对应 models/manifests/model_template.yaml。
//   - 解析复用 config::ConfigLoader 的 YAML 子集（不支持 flow 集合），
//     形状等列表以 "[1, 640, 640, 3]" 标量记法解析；
//   - 未填写/未实测字段保持空串或 UNKNOWN 语义（has_* 标记），禁止编造；
//   - sha256 校验见 VerifyManifestFileSha256。

#include <cstdint>
#include <string>
#include <vector>

#include "animal_monitor/common/status.h"

namespace animal_monitor::inference
{

// 迁移前未补算真实 sha256 时的占位值（legacy_detectors.yaml）。
constexpr char kManifestSha256Placeholder[] = "NOT_COMPUTED";

// manifest.rknn 段：RKNN 运行时信息。
struct ModelRknnConfig
{
    std::string  sdk_version    = "UNKNOWN";  // rknn_api 版本（如 1.6.0）
    std::string  driver_version = "UNKNOWN";  // 板上 rknn_server/NPU driver 版本
    std::int32_t npu_core_mask  = 7;      // RK3588 三核位掩码（core0=1|core1=2|core2=4），实测决定
    bool         wants_float    = false;  // 是否浮点输出（Phase 3 仅支持 int8 输出）
};

// manifest.input 条目。
struct ModelInputSpec
{
    std::string               name = "images";
    std::vector<std::int64_t> shape;  // 如 [1,640,640,3]；空 = UNKNOWN（以 rknn_query 为准）
    std::string               layout       = "NHWC";
    std::string               data_type    = "uint8";
    std::string               color_format = "RGB";
    bool                      letterbox    = false;  // false = 直接拉伸（A/B 实测行为）
    std::vector<double>       mean;  // Phase 3 未实现归一化；配置非空即报 kUnimplemented
    std::vector<double>       std;
};

// manifest.output 条目。
struct ModelOutputSpec
{
    std::string               name = "output0";
    std::vector<std::int64_t> shape;  // 空 = UNKNOWN（以 rknn_query 实测为准）
    std::string               data_type              = "int8";
    double                    quantization_scale     = 0.0;  // has_* 为 false 表示 UNKNOWN
    std::int32_t              quantization_zp        = 0;
    bool                      has_quantization_scale = false;
    bool                      has_quantization_zp    = false;
    std::string               postprocess = "UNKNOWN";  // yolov5 | behavior_head | UNKNOWN
};

// 模型 manifest。对应 model_template.yaml 的全部字段。
struct ModelManifest
{
    std::string model_id;                // 必填，非空
    std::string model_kind = "UNKNOWN";  // behavior | detector | thermal_supplement
    std::string task;                    // grooming / feeding / rearing ...
    std::string file;                    // 模型文件路径（相对进程工作目录解析）
    std::string sha256;                  // 真实 SHA256（空或 NOT_COMPUTED 表示未提供）

    ModelRknnConfig rknn;

    std::vector<ModelInputSpec>  inputs;
    std::vector<ModelOutputSpec> outputs;
    std::vector<std::string>     classes;

    double confidence_threshold = 0.25;
    double nms_threshold        = 0.45;

    std::string version;
    std::string status = "UNKNOWN";  // 未实测/未确认一律 UNKNOWN
};

// 从 manifest 文件加载（YAML 子集解析）。失败返回 Status（消息含行号/键名）。
// 必填校验：model_id 非空；file、sha256 键存在（值可为空）。
Status LoadManifestFile(const std::string& path, ModelManifest* out);

// 校验模型文件 sha256：
//   - 文件存在：计算并比对（十六进制大小写不敏感）；不匹配返回 kConfigurationError；
//   - sha256 未填写（空/NOT_COMPUTED）：kConfigurationError；
//   - 文件不存在：kNotFound，message 说明"模型文件缺失，应用可按无 AI 模式运行"。
Status VerifyManifestFileSha256(const ModelManifest& manifest);

}  // namespace animal_monitor::inference
