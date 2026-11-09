// ============================================================================
// 文件：tests/unit/test_htu21d_math.cpp
// 状态：implemented（host 可编译，MSVC /W4 已验证）
// 依据：include/animal_monitor/environment/htu21d_math.h；HTU21D 数据手册公开公式
// 测试锚点说明：
//   - (0x00,0x00) → 0x00：任务书指定锚点（初值 0、异或 0、无置位位）。
//   - (0x68,0x3A) → 0x7C 与 (0x4E,0x85) → 0x6B：Sensirion 数据手册 CRC 典型示例值。
//   - 端点换算：srh=0 → -6%；srh=0xFFFF → 125×65535/65536-6 ≈ 118.9981%；
//     st=0 → -46.85°C；st=0xFFFF → 175.72×65535/65536-46.85 ≈ 128.8673°C。
// ============================================================================

#include <cmath>
#include <cstdint>

#include "test_harness.h"

#include "animal_monitor/environment/htu21d_math.h"

using animal_monitor::Htu21dCrc8;
using animal_monitor::Htu21dCrcOk;
using animal_monitor::Htu21dRhPercent;
using animal_monitor::Htu21dTempC;

namespace
{

bool Approx(float actual, float expected, float eps = 1e-3f)
{
    return std::fabs(actual - expected) <= eps;
}

AM_TEST(crc8_zero_vector)
{
    // 任务书锚点：初值 0、数据全 0 → CRC 恒为 0。
    AM_EXPECT_EQ(Htu21dCrc8(0x00, 0x00), 0x00);
}

AM_TEST(crc8_datasheet_example_vectors)
{
    // Sensirion 数据手册 CRC 典型示例值（与 (0,0)→0 锚点共同自证实现正确）。
    AM_EXPECT_EQ(Htu21dCrc8(0x68, 0x3A), 0x7C);
    AM_EXPECT_EQ(Htu21dCrc8(0x4E, 0x85), 0x6B);
}

AM_TEST(crc8_round_trip_for_typical_values)
{
    const std::uint16_t values[] = {0x0000, 0x683A, 0x4E85, 0xFFFF, 0x1234, 0x8000};
    for (std::uint16_t v : values)
    {
        const std::uint8_t crc =
            Htu21dCrc8(static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v & 0xFF));
        AM_EXPECT(Htu21dCrcOk(v, crc));
    }
}

AM_TEST(crc8_rejects_corrupted_byte)
{
    AM_EXPECT(!Htu21dCrcOk(0x683A, static_cast<std::uint8_t>(0x7C ^ 0x01)));
    AM_EXPECT(!Htu21dCrcOk(0x683A, 0x00));
    AM_EXPECT(!Htu21dCrcOk(0x0001, 0x00));
}

AM_TEST(rh_endpoints)
{
    AM_EXPECT(Approx(Htu21dRhPercent(0x0000), -6.0f, 1e-4f));  // 任务书端点
    AM_EXPECT(Approx(Htu21dRhPercent(0xFFFF), 118.9981f));     // 逼近 125，不到
    AM_EXPECT(Approx(Htu21dRhPercent(0x8000), 56.5f));         // 中点
}

AM_TEST(temp_endpoints)
{
    AM_EXPECT(Approx(Htu21dTempC(0x0000), -46.85f, 1e-4f));  // 任务书端点
    AM_EXPECT(Approx(Htu21dTempC(0xFFFF), 128.8673f));       // 量程上界附近
    AM_EXPECT(Approx(Htu21dTempC(0x8000), 41.01f));          // 中点
}

AM_TEST(conversions_are_strictly_monotonic)
{
    AM_EXPECT(Htu21dRhPercent(1000) < Htu21dRhPercent(2000));
    AM_EXPECT(Htu21dTempC(1000) < Htu21dTempC(2000));
}

}  // namespace

int main() { return am_test::RunAll(); }
