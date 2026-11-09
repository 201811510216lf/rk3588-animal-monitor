#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "animal_monitor/common/status.h"

namespace animal_monitor::config
{

enum class ConfigValueType
{
    kScalar,
    kMap,
    kSequence,
    kPending,  // 解析中间态：待定节点（首个 Insert → map；首个 Append → sequence）
};

// 配置树节点。Phase 1 使用自有 YAML 子集解析器：
//   - 缩进嵌套 map（仅空格，Tab 不支持）；
//   - "- item" 序列；"- key: value" 序列内 map；
//   - 标量、双引号字符串（支持 \" \\ \n \t 转义）、行内 # 注释；
//   - 不支持：flow 集合、多行标量、锚点/别名、含冒号的裸标量（用引号包裹）。
// 后续替换为 yaml-cpp 时保持本接口不变。
class ConfigValue
{
   public:
    ConfigValueType Type() const { return type_; }
    bool            IsPending() const { return type_ == ConfigValueType::kPending; }
    bool            IsScalar() const { return type_ == ConfigValueType::kScalar; }
    bool            IsMap() const { return type_ == ConfigValueType::kMap; }
    bool            IsSequence() const { return type_ == ConfigValueType::kSequence; }

    // ---- 标量 ----
    const std::string& Scalar() const;
    bool               AsBool(bool* out) const;
    bool               AsInt64(std::int64_t* out) const;
    bool               AsDouble(double* out) const;

    // ---- map ----
    bool                                      HasKey(const std::string& key) const;
    const ConfigValue*                        Find(const std::string& key) const;
    const std::map<std::string, ConfigValue>& Map() const;

    // ---- sequence ----
    const std::vector<ConfigValue>& Sequence() const;
    std::vector<ConfigValue>&       Sequence();  // 非 const 访问（解析器/测试使用）

    // ---- 构造（解析器与测试使用） ----
    static ConfigValue MakeScalar(std::string value);
    static ConfigValue MakeMap();
    static ConfigValue MakeSequence();
    static ConfigValue MakePending();

    // 插入子键；返回插入节点的引用。仅 map/pending 节点可用（pending 转 map）。
    ConfigValue& Insert(const std::string& key, ConfigValue value);
    // 追加序列元素；返回追加元素的引用。仅 sequence/pending 节点可用（pending 转 sequence）。
    ConfigValue& Append(ConfigValue value);

    // 解析收尾：递归把无内容的 pending 节点定型为空 map
    // （子集语义："key:" 无子项 = 空映射）。
    void FinalizePending();

   private:
    ConfigValueType                    type_ = ConfigValueType::kPending;
    std::string                        scalar_;
    std::vector<ConfigValue>           sequence_;
    std::map<std::string, ConfigValue> map_;
};

// YAML 子集解析。错误以 Status 返回，消息包含行号。
class ConfigLoader
{
   public:
    static Status LoadFromFile(const std::string& path, ConfigValue* root);
    static Status ParseString(const std::string& text, ConfigValue* root);
};

// 配置校验：检查必需键与关键类型。错误明细通过 problems 返回。
class ConfigValidator
{
   public:
    static Status Validate(const ConfigValue& root, std::string* problems);
};

}  // namespace animal_monitor::config
