// ============================================================================
// 文件：tests/unit/test_peripheral_manager.cpp
// 状态：implemented（host 单测，MSVC /W4 /permissive- 零告警）
// 依据：include/animal_monitor/device/peripheral_manager.h。
// ============================================================================

#include <string>

#include "test_harness.h"

#include "animal_monitor/device/peripheral_manager.h"

using animal_monitor::PeripheralManager;
using animal_monitor::PeripheralState;
using animal_monitor::PeripheralStateName;

namespace {

AM_TEST(register_find_and_state_update)
{
    PeripheralManager manager;
    manager.Register("modbus");

    const animal_monitor::PeripheralStatus* found = manager.Find("modbus");
    AM_EXPECT(found != nullptr);
    AM_EXPECT(found->state == PeripheralState::kUnknown);
    AM_EXPECT_EQ(found->name, std::string("modbus"));

    manager.SetState("modbus", PeripheralState::kEnabled, "ttyS1 打开成功");
    found = manager.Find("modbus");
    AM_EXPECT(found != nullptr);
    AM_EXPECT(found->state == PeripheralState::kEnabled);
    AM_EXPECT_EQ(found->message, std::string("ttyS1 打开成功"));
}

AM_TEST(register_is_idempotent)
{
    PeripheralManager manager;
    manager.Register("gpio");
    manager.SetState("gpio", PeripheralState::kDisabled, "已关闭");
    manager.Register("gpio");  // 重复注册不得覆盖状态
    const animal_monitor::PeripheralStatus* found = manager.Find("gpio");
    AM_EXPECT(found != nullptr);
    AM_EXPECT(found->state == PeripheralState::kDisabled);
}

AM_TEST(find_unregistered_returns_null)
{
    PeripheralManager manager;
    AM_EXPECT(manager.Find("not_registered") == nullptr);
}

AM_TEST(set_state_implicitly_registers)
{
    PeripheralManager manager;
    manager.SetState("watchdog", PeripheralState::kHardwareBlocked, "无板卡");
    AM_EXPECT_EQ(manager.Count(), 1u);
    const animal_monitor::PeripheralStatus* found = manager.Find("watchdog");
    AM_EXPECT(found != nullptr);
    AM_EXPECT(found->state == PeripheralState::kHardwareBlocked);
}

AM_TEST(dump_contains_state_text_and_is_sorted)
{
    PeripheralManager manager;
    manager.Register("bbb");
    manager.SetState("bbb", PeripheralState::kUnknown, "");
    manager.Register("aaa");
    manager.SetState("aaa", PeripheralState::kError, "打开失败");
    manager.SetState("ccc", PeripheralState::kEnabled, "正常");

    const std::string dump = manager.Dump();
    // 表头 + 状态文本存在。
    AM_EXPECT(dump.find("name\tstate\tmessage") == 0);
    AM_EXPECT(dump.find("error") != std::string::npos);
    AM_EXPECT(dump.find("enabled") != std::string::npos);
    // 按名称排序稳定：aaa 在 bbb 之前。
    AM_EXPECT(dump.find("aaa\t") < dump.find("bbb\t"));
    AM_EXPECT(dump.find("bbb\t") < dump.find("ccc\t"));
}

AM_TEST(state_name_mapping)
{
    AM_EXPECT_EQ(std::string(PeripheralStateName(PeripheralState::kUnknown)),
                 std::string("unknown"));
    AM_EXPECT_EQ(std::string(PeripheralStateName(PeripheralState::kEnabled)),
                 std::string("enabled"));
    AM_EXPECT_EQ(std::string(PeripheralStateName(PeripheralState::kDisabled)),
                 std::string("disabled"));
    AM_EXPECT_EQ(std::string(PeripheralStateName(PeripheralState::kError)), std::string("error"));
    AM_EXPECT_EQ(std::string(PeripheralStateName(PeripheralState::kHardwareBlocked)),
                 std::string("hardware_blocked"));
}

AM_TEST(count_tracks_registered_peripherals)
{
    PeripheralManager manager;
    AM_EXPECT_EQ(manager.Count(), 0u);
    manager.Register("rtc");
    manager.Register("eeprom");
    manager.Register("rtc");  // 幂等
    AM_EXPECT_EQ(manager.Count(), 2u);
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
