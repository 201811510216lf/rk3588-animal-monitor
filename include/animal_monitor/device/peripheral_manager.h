#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/peripheral_manager.h
// 状态：implemented（host 可编译；已由 tests/unit/test_peripheral_manager.cpp 覆盖）
// 依据：任务书 CODEX_MASTER_TASK.md §8（每个外设必须有配置、设备状态、诊断）。
// 线程模型：所有方法互斥保护，可跨线程调用。
// ============================================================================

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

namespace animal_monitor {

// 外设状态枚举。kHardwareBlocked 表示代码就绪但缺少板卡/驱动验证。
enum class PeripheralState
{
    kUnknown,
    kEnabled,
    kDisabled,
    kError,
    kHardwareBlocked,
};

// 状态文本（"unknown"/"enabled"/"disabled"/"error"/"hardware_blocked"），供 Dump/日志使用。
const char* PeripheralStateName(PeripheralState state);

// 单个外设的状态快照。
struct PeripheralStatus
{
    std::string     name;
    PeripheralState state = PeripheralState::kUnknown;
    std::string     message;
};

// 外设状态注册表（可移植纯逻辑，无硬件依赖）。
// 用法：先 Register(name) 声明外设（初始 kUnknown），再 SetState 更新；
// SetState 对未注册名称自动隐式注册（便于按状态直接上报，文档化行为）。
class PeripheralManager
{
  public:
    PeripheralManager() = default;

    PeripheralManager(const PeripheralManager&) = delete;
    PeripheralManager& operator=(const PeripheralManager&) = delete;

    // 注册外设（已存在则幂等，不覆盖当前状态）。
    void Register(const std::string& name);

    // 更新状态与消息；名称未注册时隐式注册。
    void SetState(const std::string& name, PeripheralState state, const std::string& message);

    // 查找状态快照；未注册返回 nullptr。
    // 指针有效性：内部使用 std::map，插入/删除其他条目不失效既有节点；
    // 但同一名称再次 SetState 会原地更新所指对象，跨线程读该指针需自行同步。
    const PeripheralStatus* Find(const std::string& name) const;

    // 按名称排序的稳定文本表（表头 + 每行 "<name>\t<state_text>\t<message>"）。
    std::string Dump() const;

    // 已注册外设数量。
    std::size_t Count() const;

  private:
    mutable std::mutex                  mutex_;
    std::map<std::string, PeripheralStatus> registry_;
};

}  // namespace animal_monitor
