// ============================================================================
// 文件：tests/unit/test_mlx90640_math.cpp
// 状态：implemented（host 可编译；MSVC /W4 已验证）
// 依据：include/animal_monitor/thermal/{mlx90640_math,thermal_frame}.h；
//       MLX90640 数据手册 + Melexis 官方 mlx90640-library 公开算法
// 说明：合成 EEPROM（1664 字节大端）驱动 ParseEeprom/换算函数；数值断言均为
//       算法自洽性验证（NOT_CALIBRATED），不代表真实器件行为。
// ============================================================================

#include <array>
#include <cmath>
#include <cstdint>

#include "test_harness.h"

#include "animal_monitor/common/status.h"
#include "animal_monitor/thermal/mlx90640_math.h"
#include "animal_monitor/thermal/thermal_frame.h"

using animal_monitor::CalcTa;
using animal_monitor::CalcTo;
using animal_monitor::CalcVdd;
using animal_monitor::HotspotPixelIndex;
using animal_monitor::InterleaveSubpages;
using animal_monitor::IsValidPixelValue;
using animal_monitor::kMlx90640EepromSizeBytes;
using animal_monitor::kMlx90640PixelCount;
using animal_monitor::MaxPixel;
using animal_monitor::MeanPixel;
using animal_monitor::MinPixel;
using animal_monitor::Mlx90640EepromData;
using animal_monitor::ParseEeprom;
using animal_monitor::ReplaceBrokenPixels;
using animal_monitor::Status;
using animal_monitor::StatusCode;
using animal_monitor::TgcCompensation;
using animal_monitor::ThermalFrame;
using animal_monitor::ValidPixelRatio;

namespace
{

constexpr int kWordCount = kMlx90640EepromSizeBytes / 2;

bool Approx(float actual, float expected, float eps = 1e-3f)
{
    return std::fabs(actual - expected) <= eps;
}

// 合成 EEPROM：全局标定取值见 mlx90640_math.cpp 官方库 Extract* 参数语义。
struct SyntheticEeprom
{
    std::array<std::uint16_t, kWordCount>              words{};
    std::array<std::uint8_t, kMlx90640EepromSizeBytes> bytes{};

    void SetWord(int index, std::uint16_t value)
    {
        words[static_cast<std::size_t>(index)]         = value;
        bytes[static_cast<std::size_t>(index * 2)]     = static_cast<std::uint8_t>(value >> 8);
        bytes[static_cast<std::size_t>(index * 2 + 1)] = static_cast<std::uint8_t>(value & 0xFF);
    }

    explicit SyntheticEeprom()
    {
        SetWord(0, 0x0210);   // EEPROM 版本
        SetWord(5, 0x10);     // 标定常数
        SetWord(16, 0x0213);  // CTRL1：occRowScale=2、occColumnScale=1、occRemScale=3、alphaPTAT=0
        SetWord(17, 0);       // offsetRef
        SetWord(32, 0x4000);  // alphaScale 高 4 位 = 4（+30），行列/余数尺度 = 0
        SetWord(33, 1024);    // alphaRef
        SetWord(48, 1024);    // gainEE
        SetWord(49, 32768);   // vPTAT25
        SetWord(50, static_cast<std::uint16_t>((16 << 10) | 32));  // Kt2=16/4096、Kt1=4.0
        SetWord(51, static_cast<std::uint16_t>((4 << 8) | 160));   // kVdd=128、vdd25=-11264
        SetWord(52, 0x1111);                                       // KvT 四象限 = 1
        SetWord(56, 0x3111);  // res=3、kvScale=1、ktaScale1 高 4 位=1（+8）、ktaScale2=1
        SetWord(57, 0x0080);  // cpAlpha[0] 原始 128（÷2^31）
        SetWord(58, 0x0040);  // cpOffset[0] 原始 64
        SetWord(59, 0x0101);  // cpKta=1、cpKv=1
        SetWord(60, 0x0810);  // KsTa=8（÷8192）、tgc=16（÷32=0.5）
        SetWord(61, 0x0810);  // ksTo[0]=16、ksTo[1]=8（÷2^(1+8)=512）
        SetWord(62, 0x0204);  // ksTo[2]=4、ksTo[3]=2
        SetWord(63, 0x1AA1);  // step=10、ct2=100、ct3=200、KsToScale=2^9
        for (int p = 0; p < kMlx90640PixelCount; ++p)
        {
            SetWord(64 + p, 0x0842);  // offset=2（×2^3=16）、alpha=4、kta=1
        }
        SetWord(64 + 5, 0x0843);    // 像素 5：位 0 置位 → 离群
        SetWord(64 + 100, 0x0000);  // 像素 100：整 word 为 0 → 坏像素
    }
};

// 解析合成 EEPROM 并配置帧内 RAM 值：
//   Ta=25°C（ptat=1000、PTAT_ART=0 → ptatCalc=32768=vPTAT25）；
//   Vdd=3.3V（vdd_raw=int16(-11264)=0xD400，与 vdd25 相等）；
//   修正项 (Ta-25)/(Vdd-3.3) 归零，便于精确断言。
Mlx90640EepromData MakeFrameEeprom()
{
    SyntheticEeprom    synth;
    Mlx90640EepromData data;
    const Status       status = ParseEeprom(synth.bytes.data(), &data);
    if (!status.Ok())
    {
        throw std::runtime_error("合成 EEPROM 解析失败: " + status.ToString());
    }
    data.ta_raw         = 1000;
    data.vdd_raw        = 54272;  // 0xD400
    data.pix_os_cp      = 0;
    data.gain_ram       = 512;  // gain = gainEE/gainRAM = 2
    data.cp_pixel[0]    = 100;
    data.cp_pixel[1]    = 200;
    data.resolution_ram = 3;  // 与 resolution_ee 一致 → 分辨率修正 = 1
    return data;
}

AM_TEST(parse_ok_extracts_all_fields)
{
    SyntheticEeprom    synth;
    Mlx90640EepromData data;
    const Status       status = ParseEeprom(synth.bytes.data(), &data);
    AM_EXPECT(status.Ok());

    AM_EXPECT(Approx(data.kt1, 4.0f));
    AM_EXPECT(Approx(data.kt2, 0.00390625f));
    AM_EXPECT(Approx(data.vdd_pixel, 128.0f));
    AM_EXPECT(Approx(data.vdd25, -11264.0f));
    AM_EXPECT(Approx(data.gain, 1024.0f));
    AM_EXPECT(Approx(data.tgc, 0.5f));
    AM_EXPECT(Approx(data.ks_ta, 0.0009765625f));
    AM_EXPECT(Approx(data.ks_to[0], 0.03125f));
    AM_EXPECT(Approx(data.ct[2], 100.0f));
    AM_EXPECT(Approx(data.ct[3], 200.0f));
    AM_EXPECT(Approx(data.pix_os[0], 16.0f));
    AM_EXPECT(Approx(data.kta[0], 64.0f));
    AM_EXPECT(Approx(data.kta_scale, 14.0f));
    AM_EXPECT(Approx(data.kv[0], 64.0f));
    AM_EXPECT(Approx(data.kv_scale, 7.0f));
    AM_EXPECT(Approx(data.alpha_scale, 10.0f));
    AM_EXPECT(data.alpha[0] >= 32767.0f && data.alpha[0] <= 65535.0f);  // 归一化存储契约
    AM_EXPECT_EQ(data.broken_pixels[0], 100);
    AM_EXPECT_EQ(data.outlier_pixels[0], 5);
}

AM_TEST(parse_rejects_null_arguments)
{
    Mlx90640EepromData data;
    AM_EXPECT_EQ(ParseEeprom(nullptr, &data).Code(), StatusCode::kInvalidArgument);

    std::array<std::uint8_t, kMlx90640EepromSizeBytes> bytes{};
    AM_EXPECT_EQ(ParseEeprom(bytes.data(), nullptr).Code(), StatusCode::kInvalidArgument);
}

AM_TEST(parse_rejects_all_ff_and_all_zero_images)
{
    std::array<std::uint8_t, kMlx90640EepromSizeBytes> all_ff;
    all_ff.fill(0xFF);
    Mlx90640EepromData data;
    AM_EXPECT(!ParseEeprom(all_ff.data(), &data).Ok());

    std::array<std::uint8_t, kMlx90640EepromSizeBytes> all_zero{};  // 版本字段为 0 → 拒绝
    AM_EXPECT(!ParseEeprom(all_zero.data(), &data).Ok());
}

AM_TEST(parse_rejects_kt_out_of_range)
{
    SyntheticEeprom    synth;
    Mlx90640EepromData data;

    synth.SetWord(50, 0x0001);  // Kt1 = 0.125 < 下限 0.5，Kt2 = 0
    AM_EXPECT(!ParseEeprom(synth.bytes.data(), &data).Ok());

    synth.SetWord(50, 0xFFFF);  // Kt1 = -0.125、Kt2 = -1/4096 → 均越界
    AM_EXPECT(!ParseEeprom(synth.bytes.data(), &data).Ok());
}

AM_TEST(parse_rejects_alpha_out_of_range)
{
    SyntheticEeprom    synth;
    Mlx90640EepromData data;

    synth.SetWord(33, 0);       // alphaRef = 0
    synth.SetWord(60, 0x0800);  // tgc = 0
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        synth.SetWord(64 + p, 0x0002);  // alpha 位 = 0、offset 位 = 0、kta = 1（非坏像素）
    }
    // alphaTemp = 0 → 不满足 Alpha 区间 → 拒绝。
    AM_EXPECT(!ParseEeprom(synth.bytes.data(), &data).Ok());
}

AM_TEST(parse_rejects_too_many_broken_pixels)
{
    SyntheticEeprom    synth;
    Mlx90640EepromData data;

    for (int p = 0; p < 5; ++p)
    {
        synth.SetWord(64 + p, 0x0000);  // 5 个坏像素（上限 4）
    }
    AM_EXPECT(!ParseEeprom(synth.bytes.data(), &data).Ok());
}

AM_TEST(interleave_merges_even_and_odd_rows)
{
    std::array<std::uint16_t, kMlx90640PixelCount> sp0;
    std::array<std::uint16_t, kMlx90640PixelCount> sp1;
    std::array<std::uint16_t, kMlx90640PixelCount> out;
    for (int i = 0; i < kMlx90640PixelCount; ++i)
    {
        sp0[i] = static_cast<std::uint16_t>(1000 + i);
        sp1[i] = static_cast<std::uint16_t>(2000 + i);
    }
    AM_EXPECT(InterleaveSubpages(sp0.data(), sp1.data(), out.data()).Ok());

    // 偶数行取 subpage0、奇数行取 subpage1。
    AM_EXPECT_EQ(out[0], 1000);
    AM_EXPECT_EQ(out[31], 1031);
    AM_EXPECT_EQ(out[32], 2032);
    AM_EXPECT_EQ(out[63], 2063);
    AM_EXPECT_EQ(out[767], 2767);
}

AM_TEST(interleave_rejects_null_arguments)
{
    std::array<std::uint16_t, kMlx90640PixelCount> buf;
    AM_EXPECT(!InterleaveSubpages(nullptr, buf.data(), buf.data()).Ok());
    AM_EXPECT(!InterleaveSubpages(buf.data(), nullptr, buf.data()).Ok());
    AM_EXPECT(!InterleaveSubpages(buf.data(), buf.data(), nullptr).Ok());
}

AM_TEST(tgc_compensation_known_value)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    // ir = 3000×2 - 16×(1+0)×(1+0) = 5984；
    // cp = 100×2 - 64×(1+0)×(1+0) = 136；tgc=0.5 → 5984 - 68 = 5916。
    AM_EXPECT(Approx(TgcCompensation(data, 0, 3000, 0), 5916.0f));
}

AM_TEST(tgc_broken_and_outlier_pixels_return_zero)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    AM_EXPECT_EQ(TgcCompensation(data, 100, 3000, 0), 0.0f);  // 坏像素
    AM_EXPECT_EQ(TgcCompensation(data, 5, 3000, 0), 0.0f);    // 离群像素
}

AM_TEST(tgc_rejects_out_of_range_arguments)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    AM_EXPECT_EQ(TgcCompensation(data, 768, 3000, 0), 0.0f);  // 像素越界
    AM_EXPECT_EQ(TgcCompensation(data, 0, 3000, 2), 0.0f);    // subpage 越界
}

AM_TEST(replace_broken_pixels_interior_and_edges)
{
    std::array<std::uint16_t, kMlx90640PixelCount> frame;
    for (int i = 0; i < kMlx90640PixelCount; ++i)
    {
        frame[i] = static_cast<std::uint16_t>(i);
    }
    Mlx90640EepromData data = MakeFrameEeprom();  // broken = {100}、outlier = {5}
    // 坏像素 100 与离群像素 5 均被替换：frame[5] = (4+6)/2 = 5、frame[100] = (99+101)/2 = 100。
    AM_EXPECT_EQ(ReplaceBrokenPixels(data, frame.data()), 2);
    AM_EXPECT_EQ(frame[5], 5);
    AM_EXPECT_EQ(frame[100], 100);

    Mlx90640EepromData edges;
    edges.broken_pixels[0] = 32;  // 行首（列 0）→ 取右邻
    edges.broken_pixels[1] = 63;  // 行尾（列 31）→ 取左邻
    AM_EXPECT_EQ(ReplaceBrokenPixels(edges, frame.data()), 2);
    AM_EXPECT_EQ(frame[32], 33);
    AM_EXPECT_EQ(frame[63], 62);
    AM_EXPECT_EQ(ReplaceBrokenPixels(edges, nullptr), -1);
}

AM_TEST(calc_vdd_known_value)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    AM_EXPECT(Approx(CalcVdd(data, data.vdd_raw), 3.3f));
}

AM_TEST(calc_vdd_zero_sensitivity_returns_nan)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    data.vdd_pixel          = 0.0f;
    AM_EXPECT(std::isnan(CalcVdd(data, data.vdd_raw)));
}

AM_TEST(calc_ta_known_value)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    AM_EXPECT(Approx(CalcTa(data, 1000), 25.0f));
    AM_EXPECT(Approx(CalcTa(data, 2000), 25.0f));  // PTAT_ART=0 时与 ptat 取值无关
}

AM_TEST(calc_ta_zero_denominator_returns_nan)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    AM_EXPECT(std::isnan(CalcTa(data, 0)));  // ptat=0、PTAT_ART=0 → 分母为零
}

AM_TEST(calc_to_sanity_and_monotonicity)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    const float        to   = CalcTo(data, 0, 1000.0f, 25.0f);
    AM_EXPECT(std::isfinite(to));
    AM_EXPECT(to > 25.0f);
    AM_EXPECT(to < 300.0f);
    AM_EXPECT(CalcTo(data, 0, 1500.0f, 25.0f) > to);  // 信号越强温度越高
}

AM_TEST(calc_to_emissivity_ordering)
{
    Mlx90640EepromData data       = MakeFrameEeprom();
    const float        to_default = CalcTo(data, 0, 1000.0f, 25.0f);  // 默认发射率 0.95
    data.emissivity               = 1.0f;  // 发射率越高 → 有效信号越小 → 温度越低
    AM_EXPECT(CalcTo(data, 0, 1000.0f, 25.0f) < to_default);
}

AM_TEST(calc_to_rejects_non_positive_signal)
{
    Mlx90640EepromData data = MakeFrameEeprom();
    AM_EXPECT(std::isnan(CalcTo(data, 0, 0.0f, 25.0f)));
    AM_EXPECT(std::isnan(CalcTo(data, 0, -5.0f, 25.0f)));
    AM_EXPECT(std::isnan(CalcTo(data, 999, 1000.0f, 25.0f)));  // 像素越界
}

AM_TEST(thermal_frame_stats_with_invalid_pixels)
{
    ThermalFrame frame;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        frame.pixels[i] = static_cast<float>(20 + i % 10);
    }
    frame.pixels[5] = std::nanf("");  // NaN → 无效
    frame.pixels[9] = 999.0f;         // 超量程 → 无效

    AM_EXPECT(IsValidPixelValue(20.0f));
    AM_EXPECT(!IsValidPixelValue(std::nanf("")));
    AM_EXPECT(!IsValidPixelValue(999.0f));

    AM_EXPECT(Approx(ValidPixelRatio(frame), 766.0f / 768.0f, 1e-4f));
    AM_EXPECT(Approx(MinPixel(frame), 20.0f, 1e-4f));
    AM_EXPECT(Approx(MaxPixel(frame), 29.0f, 1e-4f));
    AM_EXPECT(MeanPixel(frame) > 24.0f && MeanPixel(frame) < 25.0f);
    AM_EXPECT_EQ(HotspotPixelIndex(frame), 19);  // 首个 29（索引 9 已无效）
}

AM_TEST(thermal_frame_all_invalid)
{
    ThermalFrame frame;
    for (int i = 0; i < ThermalFrame::kPixelCount; ++i)
    {
        frame.pixels[i] = std::nanf("");
    }
    AM_EXPECT_EQ(ValidPixelRatio(frame), 0.0f);
    AM_EXPECT_EQ(MinPixel(frame), 0.0f);
    AM_EXPECT_EQ(MaxPixel(frame), 0.0f);
    AM_EXPECT_EQ(MeanPixel(frame), 0.0f);
    AM_EXPECT_EQ(HotspotPixelIndex(frame), -1);
}

}  // namespace

int main() { return am_test::RunAll(); }
