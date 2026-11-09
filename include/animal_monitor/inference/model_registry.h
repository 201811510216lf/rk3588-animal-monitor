#pragma once

// animal_monitor/inference/model_registry.h
// 状态：implemented（host 可编译）
// 说明：模型注册表：扫描 manifests 目录，逐个解析并校验。
//   - 非 .yaml/.yml 文件跳过（记录 skipped_non_yaml）；
//   - 缺失模型文件不阻断注册（status=model_missing），应用可按无 AI 模式运行；
//   - sha256 仅在模型文件存在时校验；未填写真实 sha256 记为 sha256_unknown；
//   - 解析失败/重复 model_id 均记录状态，不中断整体加载。

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "animal_monitor/common/status.h"
#include "animal_monitor/inference/model_manifest.h"

namespace animal_monitor::inference {

class ModelRegistry
{
  public:
    // 模型状态记录（供应用打印/监控）。
    struct StatusRecord
    {
        std::string status;  // ok / model_missing / sha256_mismatch / sha256_unknown /
                             // manifest_invalid / duplicate_model_id / skipped_non_yaml
        std::string message;
    };

    // 扫描目录加载所有 .yaml/.yml 并逐个校验。
    // 目录不存在/不可访问返回 kNotFound；单个文件的问题记录于状态表，不中断整体。
    Status LoadFromDir(const std::string& manifests_dir);

    // 记录/覆盖某键的状态（应用运行时补充用，键通常为 model_id 或 "file:..."）。
    void RecordModelStatus(const std::string& key, const std::string& status,
                           const std::string& message);

    // 已注册且状态为 ok 的模型（可直接用于推理）。
    std::vector<std::string> AvailableModels() const;

    // 全部已注册 model_id（含 model_missing 等状态）。
    std::vector<std::string> RegisteredModels() const;

    // 查找 manifest；未注册返回 nullptr。指针指向注册表内部存储，随注册表生命周期
    // 有效（std::map 节点稳定，插入不失效）。
    const ModelManifest* Find(const std::string& model_id) const;

    // 全部状态记录快照（含按文件名记录的加载失败条目）。
    std::map<std::string, StatusRecord> StatusRecords() const;

    // 已注册模型数量。
    std::size_t Count() const;

  private:
    void RecordStatusLocked(const std::string& key, const std::string& status,
                            const std::string& message);

    mutable std::mutex                   mutex_;
    std::map<std::string, ModelManifest> manifests_;
    std::map<std::string, StatusRecord>  statuses_;
};

}  // namespace animal_monitor::inference
