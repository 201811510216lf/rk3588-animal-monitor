// ============================================================================
// 文件：src/device/peripheral_manager.cpp
// 状态：implemented（host 可编译；已由 tests/unit/test_peripheral_manager.cpp 覆盖）
// 依据：include/animal_monitor/device/peripheral_manager.h；
//       任务书 CODEX_MASTER_TASK.md §8（外设状态与诊断）。
// ============================================================================

#include "animal_monitor/device/peripheral_manager.h"

#include <map>
#include <mutex>
#include <string>

namespace animal_monitor {

const char* PeripheralStateName(PeripheralState state)
{
    switch (state)
    {
        case PeripheralState::kUnknown:
            return "unknown";
        case PeripheralState::kEnabled:
            return "enabled";
        case PeripheralState::kDisabled:
            return "disabled";
        case PeripheralState::kError:
            return "error";
        case PeripheralState::kHardwareBlocked:
            return "hardware_blocked";
    }
    return "unknown";
}

void PeripheralManager::Register(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    registry_.emplace(name, PeripheralStatus{name, PeripheralState::kUnknown, std::string()});
}

void PeripheralManager::SetState(const std::string& name, PeripheralState state,
                                 const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(name);
    if (it == registry_.end())
    {
        // 未注册名称隐式注册（文档化行为），避免状态更新被静默丢弃。
        it = registry_.emplace(name, PeripheralStatus{name, PeripheralState::kUnknown,
                                                      std::string()})
                 .first;
    }
    it->second.state   = state;
    it->second.message = message;
}

const PeripheralStatus* PeripheralManager::Find(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = registry_.find(name);
    return it == registry_.end() ? nullptr : &it->second;
}

std::string PeripheralManager::Dump() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 表头与每行：name \t state_text \t message。std::map 按键排序，输出稳定。
    std::string text = "name\tstate\tmessage\n";
    for (const auto& entry : registry_)
    {
        text += entry.first;
        text += '\t';
        text += PeripheralStateName(entry.second.state);
        text += '\t';
        text += entry.second.message;
        text += '\n';
    }
    return text;
}

std::size_t PeripheralManager::Count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.size();
}

}  // namespace animal_monitor
