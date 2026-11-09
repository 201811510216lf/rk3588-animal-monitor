#include "animal_monitor/inference/model_manifest.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "animal_monitor/common/sha256.h"
#include "animal_monitor/config/config_loader.h"

namespace animal_monitor::inference {

namespace {

using config::ConfigValue;

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

// 读取标量键；缺失或类型错误返回失败 Status。
Status GetScalar(const ConfigValue& map, const char* key, std::string* out)
{
    const ConfigValue* entry = map.Find(key);
    if (entry == nullptr)
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 缺少键 '") + key + "'");
    }
    if (!entry->IsScalar())
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是标量");
    }
    *out = entry->Scalar();
    return Status::MakeOk();
}

// 可选标量：缺失时保持默认值。
Status GetOptionalScalar(const ConfigValue& map, const char* key, std::string* out)
{
    const ConfigValue* entry = map.Find(key);
    if (entry == nullptr)
    {
        return Status::MakeOk();
    }
    if (!entry->IsScalar())
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是标量");
    }
    *out = entry->Scalar();
    return Status::MakeOk();
}

// 解析整数列表标量："[1, 640, 640, 3]" / "1, 640" / "[]"（空）。
// 返回 false 表示解析失败（含 "UNKNOWN"）。
bool ParseIntList(const std::string& text, std::vector<std::int64_t>* out)
{
    std::string body = text;
    if (body.size() >= 2 && body.front() == '[' && body.back() == ']')
    {
        body = body.substr(1, body.size() - 2);
    }
    out->clear();
    std::string        item;
    std::istringstream stream(body);
    while (std::getline(stream, item, ','))
    {
        const std::string trimmed = [&item]()
        {
            std::size_t begin = 0;
            while (begin < item.size() &&
                   std::isspace(static_cast<unsigned char>(item[begin])) != 0)
            {
                ++begin;
            }
            std::size_t end = item.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(item[end - 1])) != 0)
            {
                --end;
            }
            return item.substr(begin, end - begin);
        }();
        if (trimmed.empty())
        {
            continue;  // "[]" 或尾随逗号 → 空列表
        }
        if (trimmed == "UNKNOWN")
        {
            return false;
        }
        char*           end_ptr = nullptr;
        const long long value   = std::strtoll(trimmed.c_str(), &end_ptr, 10);
        if (end_ptr == trimmed.c_str() || *end_ptr != '\0')
        {
            return false;
        }
        out->push_back(static_cast<std::int64_t>(value));
    }
    return true;
}

// 解析浮点列表标量（mean/std）。"UNKNOWN"/空 → 空列表。
Status ParseDoubleList(const std::string& text, std::vector<double>* out)
{
    std::string body = text;
    if (body.size() >= 2 && body.front() == '[' && body.back() == ']')
    {
        body = body.substr(1, body.size() - 2);
    }
    out->clear();
    if (body.empty() || body == "UNKNOWN")
    {
        return Status::MakeOk();
    }
    std::string        item;
    std::istringstream stream(body);
    while (std::getline(stream, item, ','))
    {
        char*        end_ptr = nullptr;
        const double value   = std::strtod(item.c_str(), &end_ptr);
        if (end_ptr == item.c_str() || *end_ptr != '\0')
        {
            return Status(StatusCode::kConfigurationError, "无法解析浮点列表元素: " + item);
        }
        out->push_back(value);
    }
    return Status::MakeOk();
}

// 解析字符串列表（classes）：块序列、单个标量或 "[a, b]" 标量记法。
Status ParseStringList(const ConfigValue& entry, std::vector<std::string>* out)
{
    out->clear();
    if (entry.IsSequence())
    {
        for (const ConfigValue& item : entry.Sequence())
        {
            if (!item.IsScalar())
            {
                return Status(StatusCode::kConfigurationError, "classes 序列元素必须是标量");
            }
            out->push_back(item.Scalar());
        }
        return Status::MakeOk();
    }
    if (entry.IsScalar())
    {
        std::string body = entry.Scalar();
        if (body.empty() || body == "[]")
        {
            return Status::MakeOk();
        }
        if (body.size() >= 2 && body.front() == '[' && body.back() == ']')
        {
            body = body.substr(1, body.size() - 2);
        }
        std::string        item;
        std::istringstream stream(body);
        while (std::getline(stream, item, ','))
        {
            // 去首尾空白与可选引号
            std::size_t begin = 0;
            while (begin < item.size() &&
                   std::isspace(static_cast<unsigned char>(item[begin])) != 0)
            {
                ++begin;
            }
            std::size_t end = item.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(item[end - 1])) != 0)
            {
                --end;
            }
            item = item.substr(begin, end - begin);
            if (item.size() >= 2 && item.front() == '"' && item.back() == '"')
            {
                item = item.substr(1, item.size() - 2);
            }
            if (!item.empty())
            {
                out->push_back(item);
            }
        }
        return Status::MakeOk();
    }
    return Status(StatusCode::kConfigurationError, "classes 必须是序列或标量");
}

Status GetOptionalBool(const ConfigValue& map, const char* key, bool* out)
{
    const ConfigValue* entry = map.Find(key);
    if (entry == nullptr)
    {
        return Status::MakeOk();
    }
    if (!entry->AsBool(out))
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是 true/false");
    }
    return Status::MakeOk();
}

Status GetOptionalInt(const ConfigValue& map, const char* key, std::int64_t* out)
{
    const ConfigValue* entry = map.Find(key);
    if (entry == nullptr)
    {
        return Status::MakeOk();
    }
    if (!entry->AsInt64(out))
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是整数");
    }
    return Status::MakeOk();
}

Status GetOptionalDouble(const ConfigValue& map, const char* key, double* out)
{
    const ConfigValue* entry = map.Find(key);
    if (entry == nullptr)
    {
        return Status::MakeOk();
    }
    if (!entry->AsDouble(out))
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是数值");
    }
    return Status::MakeOk();
}

// 可选量化参数：缺失 / "UNKNOWN" / 空 → present=false。
Status GetOptionalQuant(const ConfigValue& map, const char* key, double* value, bool* present)
{
    const ConfigValue* entry = map.Find(key);
    if (entry == nullptr)
    {
        *present = false;
        return Status::MakeOk();
    }
    if (!entry->IsScalar())
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是标量");
    }
    if (entry->Scalar() == "UNKNOWN" || entry->Scalar().empty())
    {
        *present = false;
        return Status::MakeOk();
    }
    if (!entry->AsDouble(value))
    {
        return Status(StatusCode::kConfigurationError,
                      std::string("manifest 键 '") + key + "' 必须是数值或 UNKNOWN");
    }
    *present = true;
    return Status::MakeOk();
}

Status ParseRknn(const ConfigValue& root, ModelRknnConfig* out)
{
    const ConfigValue* rknn = root.Find("rknn");
    if (rknn == nullptr)
    {
        return Status::MakeOk();  // 允许缺省
    }
    if (!rknn->IsMap())
    {
        return Status(StatusCode::kConfigurationError, "manifest 键 'rknn' 必须是映射");
    }
    Status st = GetOptionalScalar(*rknn, "sdk_version", &out->sdk_version);
    if (!st)
    {
        return st;
    }
    st = GetOptionalScalar(*rknn, "driver_version", &out->driver_version);
    if (!st)
    {
        return st;
    }
    std::int64_t core_mask = out->npu_core_mask;
    st                     = GetOptionalInt(*rknn, "npu_core_mask", &core_mask);
    if (!st)
    {
        return st;
    }
    out->npu_core_mask = static_cast<std::int32_t>(core_mask);
    return GetOptionalBool(*rknn, "wants_float", &out->wants_float);
}

Status ParseInputs(const ConfigValue& root, std::vector<ModelInputSpec>* out)
{
    out->clear();
    const ConfigValue* inputs = root.Find("input");
    if (inputs == nullptr)
    {
        return Status::MakeOk();
    }
    if (!inputs->IsSequence())
    {
        return Status(StatusCode::kConfigurationError, "manifest 键 'input' 必须是序列");
    }
    for (const ConfigValue& item : inputs->Sequence())
    {
        if (!item.IsMap())
        {
            return Status(StatusCode::kConfigurationError, "input 序列元素必须是映射");
        }
        ModelInputSpec spec;
        Status         st = GetOptionalScalar(item, "name", &spec.name);
        if (!st)
        {
            return st;
        }
        const ConfigValue* shape = item.Find("shape");
        if (shape != nullptr)
        {
            if (!shape->IsScalar())
            {
                return Status(StatusCode::kConfigurationError, "input.shape 必须是标量列表记法");
            }
            if (shape->Scalar() != "UNKNOWN" && !ParseIntList(shape->Scalar(), &spec.shape))
            {
                return Status(StatusCode::kConfigurationError,
                              "无法解析 input.shape: " + shape->Scalar());
            }
        }
        st = GetOptionalScalar(item, "layout", &spec.layout);
        if (!st)
        {
            return st;
        }
        st = GetOptionalScalar(item, "data_type", &spec.data_type);
        if (!st)
        {
            return st;
        }
        st = GetOptionalScalar(item, "color_format", &spec.color_format);
        if (!st)
        {
            return st;
        }
        st = GetOptionalBool(item, "letterbox", &spec.letterbox);
        if (!st)
        {
            return st;
        }
        const ConfigValue* mean = item.Find("mean");
        if (mean != nullptr && mean->IsScalar())
        {
            st = ParseDoubleList(mean->Scalar(), &spec.mean);
            if (!st)
            {
                return st;
            }
        }
        const ConfigValue* std_entry = item.Find("std");
        if (std_entry != nullptr && std_entry->IsScalar())
        {
            st = ParseDoubleList(std_entry->Scalar(), &spec.std);
            if (!st)
            {
                return st;
            }
        }
        out->push_back(std::move(spec));
    }
    return Status::MakeOk();
}

Status ParseOutputs(const ConfigValue& root, std::vector<ModelOutputSpec>* out)
{
    out->clear();
    const ConfigValue* outputs = root.Find("output");
    if (outputs == nullptr)
    {
        return Status::MakeOk();
    }
    if (!outputs->IsSequence())
    {
        return Status(StatusCode::kConfigurationError, "manifest 键 'output' 必须是序列");
    }
    for (const ConfigValue& item : outputs->Sequence())
    {
        if (!item.IsMap())
        {
            return Status(StatusCode::kConfigurationError, "output 序列元素必须是映射");
        }
        ModelOutputSpec spec;
        Status          st = GetOptionalScalar(item, "name", &spec.name);
        if (!st)
        {
            return st;
        }
        const ConfigValue* shape = item.Find("shape");
        if (shape != nullptr)
        {
            if (!shape->IsScalar())
            {
                return Status(StatusCode::kConfigurationError, "output.shape 必须是标量列表记法");
            }
            if (shape->Scalar() != "UNKNOWN" && !ParseIntList(shape->Scalar(), &spec.shape))
            {
                return Status(StatusCode::kConfigurationError,
                              "无法解析 output.shape: " + shape->Scalar());
            }
        }
        st = GetOptionalScalar(item, "data_type", &spec.data_type);
        if (!st)
        {
            return st;
        }
        st = GetOptionalQuant(item, "quantization_scale", &spec.quantization_scale,
                              &spec.has_quantization_scale);
        if (!st)
        {
            return st;
        }
        double zp_value = 0.0;
        st = GetOptionalQuant(item, "quantization_zp", &zp_value, &spec.has_quantization_zp);
        if (!st)
        {
            return st;
        }
        spec.quantization_zp = static_cast<std::int32_t>(zp_value);
        st                   = GetOptionalScalar(item, "postprocess", &spec.postprocess);
        if (!st)
        {
            return st;
        }
        out->push_back(std::move(spec));
    }
    return Status::MakeOk();
}

Status ParseThresholds(const ConfigValue& root, ModelManifest* manifest)
{
    const ConfigValue* thresholds = root.Find("thresholds");
    if (thresholds == nullptr)
    {
        return Status::MakeOk();
    }
    if (!thresholds->IsMap())
    {
        return Status(StatusCode::kConfigurationError, "manifest 键 'thresholds' 必须是映射");
    }
    double value = manifest->confidence_threshold;
    Status st    = GetOptionalDouble(*thresholds, "confidence", &value);
    if (!st)
    {
        return st;
    }
    manifest->confidence_threshold = value;
    value                          = manifest->nms_threshold;
    st                             = GetOptionalDouble(*thresholds, "nms", &value);
    if (!st)
    {
        return st;
    }
    manifest->nms_threshold = value;
    return Status::MakeOk();
}

}  // namespace

Status LoadManifestFile(const std::string& path, ModelManifest* out)
{
    if (out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "LoadManifestFile: out 输出参数为空");
    }
    ConfigValue root;
    Status      st = config::ConfigLoader::LoadFromFile(path, &root);
    if (!st)
    {
        return Status(st.Code(), "解析 manifest 失败（" + path + "）: " + st.Message());
    }
    if (!root.IsMap())
    {
        return Status(StatusCode::kConfigurationError, "manifest 根节点必须是映射: " + path);
    }

    ModelManifest manifest;
    st = GetScalar(root, "model_id", &manifest.model_id);
    if (!st)
    {
        return Status(st.Code(), st.Message() + "（文件 " + path + "）");
    }
    if (manifest.model_id.empty())
    {
        return Status(StatusCode::kConfigurationError,
                      "manifest.model_id 不能为空（文件 " + path + "）");
    }
    st = GetScalar(root, "file", &manifest.file);
    if (!st)
    {
        return Status(st.Code(), st.Message() + "（文件 " + path + "）");
    }
    st = GetScalar(root, "sha256", &manifest.sha256);
    if (!st)
    {
        return Status(st.Code(), st.Message() + "（文件 " + path + "）");
    }

    st = GetOptionalScalar(root, "model_kind", &manifest.model_kind);
    if (!st)
    {
        return st;
    }
    st = GetOptionalScalar(root, "task", &manifest.task);
    if (!st)
    {
        return st;
    }
    st = GetOptionalScalar(root, "version", &manifest.version);
    if (!st)
    {
        return st;
    }
    st = GetOptionalScalar(root, "status", &manifest.status);
    if (!st)
    {
        return st;
    }
    st = ParseRknn(root, &manifest.rknn);
    if (!st)
    {
        return st;
    }
    st = ParseInputs(root, &manifest.inputs);
    if (!st)
    {
        return st;
    }
    st = ParseOutputs(root, &manifest.outputs);
    if (!st)
    {
        return st;
    }
    const ConfigValue* classes = root.Find("classes");
    if (classes != nullptr)
    {
        st = ParseStringList(*classes, &manifest.classes);
        if (!st)
        {
            return st;
        }
    }
    st = ParseThresholds(root, &manifest);
    if (!st)
    {
        return st;
    }

    *out = std::move(manifest);
    return Status::MakeOk();
}

Status VerifyManifestFileSha256(const ModelManifest& manifest)
{
    if (manifest.file.empty())
    {
        return Status(StatusCode::kNotFound,
                      "模型文件缺失（manifest 未指定 file），应用可按无 AI 模式运行");
    }
    if (manifest.sha256.empty() || manifest.sha256 == kManifestSha256Placeholder)
    {
        return Status(StatusCode::kConfigurationError, "manifest.sha256 未填写真实值（空或 " +
                                                           std::string(kManifestSha256Placeholder) +
                                                           "），无法校验完整性");
    }
    std::string actual;
    Status      st = Sha256File(manifest.file, &actual);
    if (!st)
    {
        if (st.Code() == StatusCode::kNotFound)
        {
            return Status(StatusCode::kNotFound,
                          "模型文件缺失（" + manifest.file + "），应用可按无 AI 模式运行");
        }
        return st;
    }
    if (!EqualsIgnoreCase(actual, manifest.sha256))
    {
        return Status(StatusCode::kConfigurationError,
                      "sha256 校验失败：期望 " + manifest.sha256 + "，实际 " + actual);
    }
    return Status::MakeOk();
}

}  // namespace animal_monitor::inference
