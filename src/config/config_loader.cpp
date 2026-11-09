#include "animal_monitor/config/config_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace animal_monitor::config {

namespace {

constexpr int  kMaxIndent   = 64;
constexpr char kCommentChar = '#';

std::string ErrAt(std::size_t line, const std::string& message)
{
    return "line " + std::to_string(line) + ": " + message;
}

bool IsKeyChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

// 去除行内注释（双引号内不处理）。
void StripInlineComment(std::string* line)
{
    bool in_quotes = false;
    for (std::size_t i = 0; i < line->size(); ++i)
    {
        const char c = (*line)[i];
        if (c == '"')
        {
            in_quotes = !in_quotes;
        }
        else if (c == kCommentChar && !in_quotes)
        {
            line->resize(i);
            return;
        }
    }
}

std::string Trim(const std::string& s)
{
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0)
    {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0)
    {
        --end;
    }
    return s.substr(begin, end - begin);
}

// 去除首尾双引号并解转义；引号不配对返回 false。
bool Unquote(std::string* value)
{
    std::string s = Trim(*value);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    {
        std::string body = s.substr(1, s.size() - 2);
        std::string out;
        out.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i)
        {
            if (body[i] == '\\' && i + 1 < body.size())
            {
                ++i;
                const char escaped = body[i];
                if (escaped == 'n')
                {
                    out.push_back('\n');
                }
                else if (escaped == 't')
                {
                    out.push_back('\t');
                }
                else
                {
                    out.push_back(escaped);
                }
            }
            else
            {
                out.push_back(body[i]);
            }
        }
        *value = out;
        return true;
    }
    if (s.size() >= 2 && (s.front() == '"' || s.back() == '"'))
    {
        return false;  // 引号不配对
    }
    *value = s;
    return true;
}

// 解析栈层级：node == nullptr 表示标量叶子（更深内容直接报错）。
struct Level
{
    int          indent;
    ConfigValue* node;
};

Status ParseLines(const std::vector<std::string>& lines, ConfigValue* root)
{
    *root              = ConfigValue::MakeMap();
    std::vector<Level> stack;

    for (std::size_t idx = 0; idx < lines.size(); ++idx)
    {
        const std::string raw     = lines[idx];
        const std::size_t line_no = idx + 1;
        std::string       content = raw;
        StripInlineComment(&content);

        std::string trimmed = Trim(content);
        if (trimmed.empty())
        {
            continue;
        }

        int indent = 0;
        while (indent < static_cast<int>(content.size()) && content[indent] == ' ')
        {
            ++indent;
        }
        if (indent > kMaxIndent)
        {
            return Status(StatusCode::kConfigurationError, ErrAt(line_no, "indent too deep"));
        }
        if (indent < static_cast<int>(content.size()) && content[indent] == '\t')
        {
            return Status(StatusCode::kConfigurationError,
                          ErrAt(line_no, "tab indentation is not supported"));
        }
        content = content.substr(indent);
        trimmed = Trim(content);

        const bool is_sequence_item =
            trimmed == "-" || (trimmed.size() >= 2 && trimmed[0] == '-' && trimmed[1] == ' ');
        const auto colon = trimmed.find(':');
        const bool is_mapping_line = !is_sequence_item && colon != std::string::npos;

        if (!is_sequence_item && !is_mapping_line)
        {
            return Status(StatusCode::kConfigurationError,
                          ErrAt(line_no, "expected 'key: value' or '- item'"));
        }

        // 回退到合适层级：弹出缩进 >= 当前行的层级（含标量叶子，弹出即结层）。
        while (!stack.empty() && indent <= stack.back().indent)
        {
            stack.pop_back();
        }

        if (is_sequence_item)
        {
            if (stack.empty())
            {
                return Status(StatusCode::kConfigurationError,
                              ErrAt(line_no, "sequence item at root is not supported"));
            }
            ConfigValue* parent = stack.back().node;
            if (parent == nullptr || !(parent->IsSequence() || parent->IsPending()))
            {
                return Status(StatusCode::kConfigurationError,
                              ErrAt(line_no, "sequence item under non-sequence node"));
            }

            std::string item_text = (trimmed == "-") ? "" : Trim(trimmed.substr(1));
            if (item_text.empty())
            {
                // "-" 后跟嵌套内容：追加待定节点（按内容定型 map/sequence）
                parent->Append(ConfigValue::MakePending());
                ConfigValue& item = parent->Sequence().back();
                stack.push_back(Level{indent, &item});
            }
            else
            {
                const auto item_colon = item_text.find(':');
                if (item_colon != std::string::npos)
                {
                    // "- key: value" → 序列元素为 map
                    const std::string key = Trim(item_text.substr(0, item_colon));
                    std::string       val = Trim(item_text.substr(item_colon + 1));
                    if (key.empty() || !std::all_of(key.begin(), key.end(),
                                                    [](char c) { return IsKeyChar(c); }))
                    {
                        return Status(StatusCode::kConfigurationError,
                                      ErrAt(line_no, "invalid key in sequence item"));
                    }
                    parent->Append(ConfigValue::MakeMap());
                    ConfigValue& item = parent->Sequence().back();
                    if (val.empty())
                    {
                        item.Insert(key, ConfigValue::MakePending());
                        stack.push_back(Level{indent, &item});
                    }
                    else
                    {
                        if (!Unquote(&val))
                        {
                            return Status(StatusCode::kConfigurationError,
                                          ErrAt(line_no, "unterminated quoted string"));
                        }
                        item.Insert(key, ConfigValue::MakeScalar(val));
                        stack.push_back(Level{indent, &item});
                    }
                }
                else
                {
                    // "- scalar"
                    if (!Unquote(&item_text))
                    {
                        return Status(StatusCode::kConfigurationError,
                                      ErrAt(line_no, "unterminated quoted string"));
                    }
                    parent->Append(ConfigValue::MakeScalar(item_text));
                    stack.push_back(Level{indent, nullptr});
                }
            }
            continue;
        }

        // 映射行
        const std::string key = Trim(trimmed.substr(0, colon));
        std::string       val = Trim(trimmed.substr(colon + 1));
        if (key.empty() ||
            !std::all_of(key.begin(), key.end(), [](char c) { return IsKeyChar(c); }))
        {
            return Status(StatusCode::kConfigurationError, ErrAt(line_no, "invalid key"));
        }

        ConfigValue* parent = stack.empty() ? root : stack.back().node;
        if (parent == nullptr || !(parent->IsMap() || parent->IsPending()))
        {
            return Status(StatusCode::kConfigurationError,
                          ErrAt(line_no, "mapping entry under non-map node"));
        }
        if (parent->IsMap() && parent->HasKey(key))
        {
            return Status(StatusCode::kConfigurationError,
                          ErrAt(line_no, "duplicate key '" + key + "'"));
        }

        if (val.empty())
        {
            ConfigValue& child = parent->Insert(key, ConfigValue::MakePending());
            stack.push_back(Level{indent, &child});
        }
        else
        {
            if (!Unquote(&val))
            {
                return Status(StatusCode::kConfigurationError,
                              ErrAt(line_no, "unterminated quoted string"));
            }
            parent->Insert(key, ConfigValue::MakeScalar(val));
            stack.push_back(Level{indent, nullptr});
        }
    }
    root->FinalizePending();
    return Status::MakeOk();
}

}  // namespace

const std::string& ConfigValue::Scalar() const
{
    return scalar_;
}

bool ConfigValue::AsBool(bool* out) const
{
    if (type_ != ConfigValueType::kScalar || out == nullptr)
    {
        return false;
    }
    if (scalar_ == "true" || scalar_ == "True" || scalar_ == "TRUE" || scalar_ == "1")
    {
        *out = true;
        return true;
    }
    if (scalar_ == "false" || scalar_ == "False" || scalar_ == "FALSE" || scalar_ == "0")
    {
        *out = false;
        return true;
    }
    return false;
}

bool ConfigValue::AsInt64(std::int64_t* out) const
{
    if (type_ != ConfigValueType::kScalar || out == nullptr)
    {
        return false;
    }
    char*             end   = nullptr;
    const long long   value = std::strtoll(scalar_.c_str(), &end, 10);
    if (end == scalar_.c_str() || *end != '\0')
    {
        return false;
    }
    *out = static_cast<std::int64_t>(value);
    return true;
}

bool ConfigValue::AsDouble(double* out) const
{
    if (type_ != ConfigValueType::kScalar || out == nullptr)
    {
        return false;
    }
    char*  end   = nullptr;
    double value = std::strtod(scalar_.c_str(), &end);
    if (end == scalar_.c_str() || *end != '\0')
    {
        return false;
    }
    *out = value;
    return true;
}

bool ConfigValue::HasKey(const std::string& key) const
{
    return type_ == ConfigValueType::kMap && map_.find(key) != map_.end();
}

const ConfigValue* ConfigValue::Find(const std::string& key) const
{
    if (type_ != ConfigValueType::kMap)
    {
        return nullptr;
    }
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

const std::map<std::string, ConfigValue>& ConfigValue::Map() const
{
    return map_;
}

const std::vector<ConfigValue>& ConfigValue::Sequence() const
{
    return sequence_;
}

std::vector<ConfigValue>& ConfigValue::Sequence()
{
    return sequence_;
}

ConfigValue ConfigValue::MakeScalar(std::string value)
{
    ConfigValue result;
    result.type_   = ConfigValueType::kScalar;
    result.scalar_ = std::move(value);
    return result;
}

ConfigValue ConfigValue::MakeMap()
{
    ConfigValue result;
    result.type_ = ConfigValueType::kMap;
    return result;
}

ConfigValue ConfigValue::MakeSequence()
{
    ConfigValue result;
    result.type_ = ConfigValueType::kSequence;
    return result;
}

ConfigValue ConfigValue::MakePending()
{
    return ConfigValue();
}

ConfigValue& ConfigValue::Insert(const std::string& key, ConfigValue value)
{
    if (type_ == ConfigValueType::kPending)
    {
        type_ = ConfigValueType::kMap;
    }
    map_[key] = std::move(value);
    return map_[key];
}

ConfigValue& ConfigValue::Append(ConfigValue value)
{
    if (type_ == ConfigValueType::kPending)
    {
        type_ = ConfigValueType::kSequence;
    }
    sequence_.push_back(std::move(value));
    return sequence_.back();
}

void ConfigValue::FinalizePending()
{
    if (type_ == ConfigValueType::kPending)
    {
        type_ = ConfigValueType::kMap;
    }
    if (type_ == ConfigValueType::kMap)
    {
        for (auto& entry : map_)
        {
            entry.second.FinalizePending();
        }
    }
    else if (type_ == ConfigValueType::kSequence)
    {
        for (auto& item : sequence_)
        {
            item.FinalizePending();
        }
    }
}

Status ConfigLoader::LoadFromFile(const std::string& path, ConfigValue* root)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        return Status(StatusCode::kNotFound, "cannot open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (file.bad())
    {
        return Status(StatusCode::kUnavailable, "read error on config file: " + path);
    }
    return ParseString(buffer.str(), root);
}

Status ConfigLoader::ParseString(const std::string& text, ConfigValue* root)
{
    if (root == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "null config root");
    }
    std::vector<std::string> lines;
    std::istringstream       stream(text);
    std::string              line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return ParseLines(lines, root);
}

Status ConfigValidator::Validate(const ConfigValue& root, std::string* problems)
{
    std::ostringstream issues;

    if (!root.IsMap())
    {
        return Status(StatusCode::kConfigurationError, "config root must be a mapping");
    }

    // 必需顶层键。board_profile 允许标量（boards/<name>.yaml 的配置名）
    // 或内联 map；其余一律要求为 map。
    static const std::vector<const char*> kRequiredTopLevel = {
        "board_profile", "cameras", "composition", "encoder", "network",
        "models",        "sensors", "peripherals", "storage", "ota",
        "logging",       "health",  "queues",      "app",
    };
    for (const char* key : kRequiredTopLevel)
    {
        const ConfigValue* entry = root.Find(key);
        if (entry == nullptr)
        {
            issues << "missing top-level key '" << key << "'; ";
        }
        else if (std::string(key) == "board_profile" && entry->IsScalar())
        {
            continue;  // 允许：boards/<name>.yaml 的配置名
        }
        else if (!entry->IsMap())
        {
            issues << "top-level key '" << key << "' must be a mapping; ";
        }
    }

    // 关键嵌套项
    const ConfigValue* cameras = root.Find("cameras");
    if (cameras != nullptr && cameras->IsMap())
    {
        for (const char* cam : {"camera0", "camera1"})
        {
            const ConfigValue* entry = cameras->Find(cam);
            if (entry == nullptr || !entry->IsMap())
            {
                issues << "cameras." << cam << " must be a mapping; ";
                continue;
            }
            const ConfigValue* source = entry->Find("source");
            if (source == nullptr || !source->IsScalar())
            {
                issues << "cameras." << cam << ".source must be a scalar (mock|v4l2); ";
            }
            else if (source->Scalar() != "mock" && source->Scalar() != "v4l2")
            {
                issues << "cameras." << cam
                       << ".source must be 'mock' or 'v4l2' (got '" << source->Scalar()
                       << "'); ";
            }
        }
    }
    else if (cameras != nullptr)
    {
        issues << "cameras.camera0 must be a mapping; ";
        issues << "cameras.camera1 must be a mapping; ";
    }

    const ConfigValue* logging = root.Find("logging");
    if (logging != nullptr && logging->IsMap())
    {
        const ConfigValue* level = logging->Find("level");
        if (level == nullptr || !level->IsScalar())
        {
            issues << "logging.level must be a scalar; ";
        }
    }

    const ConfigValue* app = root.Find("app");
    if (app != nullptr && app->IsMap())
    {
        // 必需：video_pipeline（Phase 2 composite 链路开关）
        const ConfigValue* pipeline = app->Find("video_pipeline");
        if (pipeline == nullptr || !pipeline->IsMap())
        {
            issues << "app.video_pipeline must be a mapping; ";
        }
        else
        {
            const ConfigValue* enabled = pipeline->Find("enabled");
            bool               value   = false;
            if (enabled == nullptr || !enabled->AsBool(&value))
            {
                issues << "app.video_pipeline.enabled must be true/false; ";
            }
        }

        // 可选：mock_pipeline（存在则校验）
        const ConfigValue* mock = app->Find("mock_pipeline");
        if (mock != nullptr)
        {
            if (!mock->IsMap())
            {
                issues << "app.mock_pipeline must be a mapping; ";
            }
            else
            {
                const ConfigValue* enabled = mock->Find("enabled");
                bool               value   = false;
                if (enabled == nullptr || !enabled->AsBool(&value))
                {
                    issues << "app.mock_pipeline.enabled must be true/false; ";
                }
            }
        }
    }

    if (problems != nullptr)
    {
        *problems = issues.str();
    }
    if (issues.tellp() > 0)
    {
        return Status(StatusCode::kConfigurationError, "config validation failed");
    }
    return Status::MakeOk();
}

}  // namespace animal_monitor::config
