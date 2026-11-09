#include "animal_monitor/inference/model_registry.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace animal_monitor::inference
{

namespace
{

// 模型状态常量（状态表供应用打印，见 src/inference/README.md）。
constexpr char kStatusOk[]              = "ok";
constexpr char kStatusModelMissing[]    = "model_missing";
constexpr char kStatusSha256Mismatch[]  = "sha256_mismatch";
constexpr char kStatusSha256Unknown[]   = "sha256_unknown";
constexpr char kStatusManifestInvalid[] = "manifest_invalid";
constexpr char kStatusDuplicateId[]     = "duplicate_model_id";
constexpr char kStatusSkippedNonYaml[]  = "skipped_non_yaml";

constexpr char kModelMissingMessage[] = "模型文件缺失，应用可按无 AI 模式运行";

std::string LowerExtension(const std::string& path)
{
    const std::string ext = std::filesystem::path(path).extension().string();
    std::string       out = ext;
    for (char& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool IsYamlFile(const std::string& path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".yaml" || ext == ".yml";
}

}  // namespace

void ModelRegistry::RecordModelStatus(const std::string& key, const std::string& status,
                                      const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    RecordStatusLocked(key, status, message);
}

void ModelRegistry::RecordStatusLocked(const std::string& key, const std::string& status,
                                       const std::string& message)
{
    statuses_[key] = StatusRecord{status, message};
}

Status ModelRegistry::LoadFromDir(const std::string& manifests_dir)
{
    std::error_code error;
    if (!std::filesystem::is_directory(manifests_dir, error))
    {
        return Status(StatusCode::kNotFound, "manifests 目录不存在或不可访问: " + manifests_dir);
    }

    std::vector<std::string>                  files;
    std::filesystem::directory_iterator       it(manifests_dir, error);
    const std::filesystem::directory_iterator end;
    for (; !error && it != end; it.increment(error))
    {
        if (it->is_regular_file(error))
        {
            files.push_back(it->path().string());
        }
    }
    if (error)
    {
        return Status(StatusCode::kUnavailable, "遍历 manifests 目录失败: " + error.message());
    }
    std::sort(files.begin(), files.end());  // 确定性加载顺序

    for (const std::string& file : files)
    {
        const std::string file_name = std::filesystem::path(file).filename().string();
        if (!IsYamlFile(file))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RecordStatusLocked("file:" + file_name, kStatusSkippedNonYaml, "非 yaml 文件，跳过");
            continue;
        }

        ModelManifest manifest;
        Status        status = LoadManifestFile(file, &manifest);
        if (!status)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RecordStatusLocked("file:" + file_name, kStatusManifestInvalid, status.Message());
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (manifests_.count(manifest.model_id) != 0)
            {
                RecordStatusLocked(manifest.model_id, kStatusDuplicateId,
                                   "同名 model_id 已注册（先注册者保留），跳过文件 " + file);
                continue;
            }
        }

        // 逐模型校验：模型文件存在时才校验 sha256（缺失不阻断注册）。
        std::string     record_status;
        std::string     record_message;
        std::error_code exists_error;
        if (manifest.file.empty() || !std::filesystem::exists(manifest.file, exists_error))
        {
            record_status = kStatusModelMissing;
            record_message =
                kModelMissingMessage + std::string(": ") +
                (manifest.file.empty() ? std::string("(manifest 未指定 file)") : manifest.file);
        }
        else if (manifest.sha256.empty() || manifest.sha256 == kManifestSha256Placeholder)
        {
            record_status  = kStatusSha256Unknown;
            record_message = "未填写真实 sha256（空或 " + std::string(kManifestSha256Placeholder) +
                             "），跳过完整性校验";
        }
        else
        {
            status = VerifyManifestFileSha256(manifest);
            if (status.Ok())
            {
                record_status  = kStatusOk;
                record_message = "manifest 与模型文件 sha256 校验通过";
            }
            else
            {
                record_status  = kStatusSha256Mismatch;
                record_message = status.Message();
            }
        }

        const std::string model_id = manifest.model_id;  // 先取出：移动 manifest 后不可再读
        std::lock_guard<std::mutex> lock(mutex_);
        manifests_.emplace(model_id, std::move(manifest));
        RecordStatusLocked(model_id, record_status, record_message);
    }
    return Status::MakeOk();
}

std::vector<std::string> ModelRegistry::AvailableModels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string>    result;
    for (const auto& entry : manifests_)
    {
        const auto record = statuses_.find(entry.first);
        if (record != statuses_.end() && record->second.status == kStatusOk)
        {
            result.push_back(entry.first);
        }
    }
    return result;
}

std::vector<std::string> ModelRegistry::RegisteredModels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string>    result;
    for (const auto& entry : manifests_)
    {
        result.push_back(entry.first);
    }
    return result;
}

const ModelManifest* ModelRegistry::Find(const std::string& model_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto                  it = manifests_.find(model_id);
    return it == manifests_.end() ? nullptr : &it->second;
}

std::map<std::string, ModelRegistry::StatusRecord> ModelRegistry::StatusRecords() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return statuses_;
}

std::size_t ModelRegistry::Count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return manifests_.size();
}

}  // namespace animal_monitor::inference
