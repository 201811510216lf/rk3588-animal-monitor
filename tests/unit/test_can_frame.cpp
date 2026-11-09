// ============================================================================
// 文件：tests/unit/test_can_frame.cpp
// 状态：implemented（host 单测，MSVC /W4 /permissive- 零告警）
// 依据：include/animal_monitor/communication/can_frame.h。
// ============================================================================

#include <cstdint>
#include <string>

#include "test_harness.h"

#include "animal_monitor/communication/can_frame.h"

using animal_monitor::CanFrame;
using animal_monitor::kMaxCanDlc;
using animal_monitor::ToString;
using animal_monitor::ValidateDlc;

namespace {

AM_TEST(dlc_validation_boundaries)
{
    AM_EXPECT(ValidateDlc(0));
    AM_EXPECT(ValidateDlc(1));
    AM_EXPECT(ValidateDlc(kMaxCanDlc));
    AM_EXPECT(!ValidateDlc(9));
    AM_EXPECT(!ValidateDlc(255));
}

AM_TEST(default_values)
{
    const CanFrame frame;
    AM_EXPECT_EQ(frame.can_id, 0u);
    AM_EXPECT(!frame.extended);
    AM_EXPECT(!frame.rtr);
    AM_EXPECT_EQ(frame.dlc, 0u);
    for (std::uint8_t i = 0; i < kMaxCanDlc; ++i)
    {
        AM_EXPECT_EQ(frame.data[i], 0u);
    }
}

AM_TEST(to_string_standard_frame)
{
    const CanFrame frame{0x123, false, false, 2, {0x0A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    AM_EXPECT_EQ(ToString(frame), std::string("123#0a01"));
}

AM_TEST(to_string_empty_frame)
{
    const CanFrame frame;
    AM_EXPECT_EQ(ToString(frame), std::string("0#"));
}

AM_TEST(to_string_rtr_frame_omits_data)
{
    const CanFrame frame{0x123, false, true, 4, {}};
    AM_EXPECT_EQ(ToString(frame), std::string("123#R"));
}

AM_TEST(to_string_extended_frame)
{
    const CanFrame frame{0x1FFFFFFF, true, false, 1, {0xFF, 0, 0, 0, 0, 0, 0, 0}};
    AM_EXPECT_EQ(ToString(frame), std::string("1fffffff#ff"));
}

AM_TEST(to_string_defensive_clamp_on_invalid_dlc)
{
    // 未校验的非法 dlc（9）不得越界读 data，输出按 8 字节截断。
    const CanFrame frame{0x123, false, false, 9, {0x0A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}};
    AM_EXPECT_EQ(ToString(frame), std::string("123#0a01020304050607"));
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
