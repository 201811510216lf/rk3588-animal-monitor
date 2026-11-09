// ============================================================================
// 文件：src/thermal/mlx90640_math.cpp
// 状态：implemented（纯数学，host 可编译；MSVC /W4 已验证）
// 依据：include/animal_monitor/thermal/mlx90640_math.h；
//       MLX90640 数据手册 + Melexis 官方 mlx90640-library（Apache-2.0）公开算法
// 声明：NOT_CALIBRATED——未经真实器件验证、未经参考测温标定，输出为未标定估算，
//       禁止宣称准确体温。标记 UNVERIFIED 的细节须板卡到位后按数据手册复核。
// ============================================================================

#include "animal_monitor/thermal/mlx90640_math.h"

#include <cmath>

namespace animal_monitor {

namespace {

// —— 物理/公式命名常量 ——
constexpr float kVddNominal     = 3.3f;      // 器件标称电源电压（V）
constexpr float kRefTempC       = 25.0f;     // 标定参考温度（°C）
constexpr float kCelsiusToKelvin = 273.15f;  // °C → K 偏移
constexpr float kScaleAlpha     = 1e-6f;     // 官方库 SCALEALPHA
constexpr float kTwoPow18       = 262144.0f; // 2^18
constexpr float kTwoPow14       = 16384.0f;  // 2^14
constexpr float kKsTo4          = -0.0002f;  // 官方库硬编码 ksTo[4]（400°C 段斜率，UNVERIFIED）
constexpr float kCtMaxC         = 400.0f;    // 量程上限拐点（°C）
constexpr float kCtMinC         = -40.0f;    // 量程下限拐点（°C）

// —— 参数范围校验边界（宽限防呆，非物理标定；真实分布待板卡验证 UNVERIFIED）——
constexpr float kKt1MinAbs = 0.5f;      // |Kt1| 下限（10 位有符号 ÷8 的编码最大 63.875）
constexpr float kKt1MaxAbs = 64.0f;
constexpr float kKt2MinAbs = 0.0005f;   // |Kt2| 下限（6 位有符号 ÷4096 的编码最大 0.0078125）
constexpr float kKt2MaxAbs = 0.008f;
constexpr float kAlphaTempMin = 1e-9f;  // 每像素 alphaTemp 下限（≤0 会使 1/x 发散）
constexpr float kAlphaStoredMax = 65535.0f;  // alpha 归一化存储上限（uint16）
constexpr int   kMaxScaleShift = 32;    // 归一化放大次数守卫（防非法 EEPROM 死循环）

constexpr float kEmissivityMin = 0.05f;
constexpr float kEmissivityMax = 1.0f;

// 读取 16 位大端 word（word 索引 n → 字节 2n/2n+1）。
std::uint16_t ReadWordBe(const std::uint8_t* eeprom, int word_index)
{
    const std::uint32_t hi = eeprom[word_index * 2];
    const std::uint32_t lo = eeprom[word_index * 2 + 1];
    return static_cast<std::uint16_t>((hi << 8) | lo);
}

// 按位数做有符号扩展（value 已对齐到低 bits 位）。
int SignExtend(std::uint32_t value, int bits)
{
    const std::uint32_t mask = (1u << bits) - 1u;
    std::uint32_t       v    = value & mask;
    if ((v & (1u << (bits - 1))) != 0)
    {
        v |= ~mask;
    }
    return static_cast<int>(v);
}

// 16 位读数按有符号解释（官方库 0x7FFF 以上视为负值）。
int Sign16(std::uint16_t value)
{
    return value > 32767 ? static_cast<int>(value) - 65536 : static_cast<int>(value);
}

// 2 的整数次幂（缩放因子）。
float TwoPow(int shift)
{
    return std::ldexp(1.0f, shift);
}

// 提取 4-bit 有符号值（>7 → -16）。
int NibbleSigned(std::uint16_t word, int shift)
{
    return SignExtend((word >> shift) & 0xF, 4);
}

// —— 官方库 Extract* 的等价实现（word 索引 → 字节地址 0x2400 + 2n）——

void ExtractVddParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 51（0x2466）：kVdd 高 8 位（int8 × 32）、vdd25 低 8 位（无符号 0..255，官方库语义）。
    d->vdd_pixel = static_cast<float>(SignExtend(words[51] >> 8, 8) * 32);
    const int vdd25_byte = static_cast<int>(words[51] & 0xFF);
    d->vdd25 = static_cast<float>(((vdd25_byte - 256) << 5) - 8192);
}

void ExtractPtatParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 50（0x2464）：Kt2/KvPTAT 高 6 位 ÷4096、Kt1/KtPTAT 低 10 位 ÷8。
    d->kt2 = static_cast<float>(SignExtend((words[50] & 0xFC00) >> 10, 6)) / 4096.0f;
    d->kt1 = static_cast<float>(SignExtend(words[50] & 0x03FF, 10)) / 8.0f;
    // word 49（0x2462）：vPTAT25。word 16（0x2420）：alphaPTAT = 高 4 位 ÷2^14 + 8。
    d->v_ptat25 = static_cast<float>(words[49]);
    d->alpha_ptat = static_cast<float>(words[16] & 0xF000) / kTwoPow14 + 8.0f;
}

void ExtractGainParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 48（0x2460）：gainEE（int16，LSB）。
    d->gain = static_cast<float>(SignExtend(words[48], 16));
}

void ExtractTgcParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 60（0x2478）低 8 位：tgc（int8 ÷32，无量纲）。
    d->tgc = static_cast<float>(SignExtend(words[60] & 0xFF, 8)) / 32.0f;
}

void ExtractResolutionParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 56（0x2470）位 13:12：分辨率配置。
    d->resolution_ee = static_cast<std::uint8_t>((words[56] >> 12) & 0x3);
}

void ExtractKsTaParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 60 高 8 位：KsTa（int8 ÷8192，1/K）。
    d->ks_ta = static_cast<float>(SignExtend(words[60] >> 8, 8)) / 8192.0f;
}

void ExtractKsToParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 63（0x247E）：step 位 13:12（无符号 ×10）、ct2 位 7:4、ct3 位 11:8、KsToScale 位 3:0。
    const int step = static_cast<int>((words[63] & 0x3000) >> 12) * 10;
    d->ct[0] = kCtMinC;
    d->ct[1] = 0.0f;
    d->ct[2] = static_cast<float>(((words[63] & 0x00F0) >> 4) * step);
    d->ct[3] = d->ct[2] + static_cast<float>(((words[63] & 0x0F00) >> 8) * step);

    const float ks_to_scale = TwoPow((words[63] & 0x000F) + 8);
    d->ks_to[0] = static_cast<float>(SignExtend(words[61] & 0xFF, 8)) / ks_to_scale;
    d->ks_to[1] = static_cast<float>(SignExtend(words[61] >> 8, 8)) / ks_to_scale;
    d->ks_to[2] = static_cast<float>(SignExtend(words[62] & 0xFF, 8)) / ks_to_scale;
    d->ks_to[3] = static_cast<float>(SignExtend(words[62] >> 8, 8)) / ks_to_scale;
}

void ExtractCpParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 57（0x2472）：cpAlpha；word 58（0x2474）：cpOffset；word 59（0x2476）：cpKta/cpKv。
    const float alpha_scale_cp = TwoPow(((words[32] >> 12) & 0xF) + 27);
    d->cp_offset[0] = static_cast<float>(SignExtend(words[58] & 0x03FF, 10));
    d->cp_offset[1] = d->cp_offset[0] + static_cast<float>(SignExtend(words[58] >> 10, 6));

    d->cp_alpha[0] = static_cast<float>(SignExtend(words[57] & 0x03FF, 10)) / alpha_scale_cp;
    d->cp_alpha[1] =
        (1.0f + static_cast<float>(SignExtend(words[57] >> 10, 6)) / 128.0f) * d->cp_alpha[0];

    const float kta_scale1 = TwoPow(((words[56] & 0x00F0) >> 4) + 8);
    const float kv_scale   = TwoPow((words[56] >> 8) & 0xF);
    d->cp_kta = static_cast<float>(SignExtend(words[59] & 0xFF, 8)) / kta_scale1;
    d->cp_kv  = static_cast<float>(SignExtend(words[59] >> 8, 8)) / kv_scale;
}

void ExtractCilcParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    // word 10（0x2414）位 11：校准模式；word 53（0x246A）：棋盘模式系数。
    d->calibration_mode_ee = static_cast<std::uint8_t>(((words[10] & 0x0800) >> 4) ^ 0x80);
    d->il_chess_c[0] = static_cast<float>(SignExtend(words[53] & 0x003F, 6)) / 16.0f;
    d->il_chess_c[1] = static_cast<float>(SignExtend((words[53] & 0x07C0) >> 6, 5)) / 2.0f;
    d->il_chess_c[2] = static_cast<float>(SignExtend((words[53] & 0xF800) >> 11, 5)) / 8.0f;
}

// 每像素 alpha：word 32/33/34-47 + 每像素 word（位 9:4，int6）。
// 校验 Alpha 区间：alphaTemp 必须 > kAlphaTempMin，归一化存储值 ∈ [1, 65535]。
bool ExtractAlphaParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    const int acc_rem_scale    = words[32] & 0x000F;
    const int acc_column_scale = (words[32] & 0x00F0) >> 4;
    const int acc_row_scale    = (words[32] & 0x0F00) >> 8;
    const float alpha_scale_ee = TwoPow(((words[32] & 0xF000) >> 12) + 30);
    const float alpha_ref      = static_cast<float>(words[33]);

    int acc_row[24] = {};
    for (int i = 0; i < 6; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            acc_row[i * 4 + j] = NibbleSigned(words[34 + i], j * 4);
        }
    }
    int acc_column[32] = {};
    for (int i = 0; i < 8; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            acc_column[i * 4 + j] = NibbleSigned(words[40 + i], j * 4);
        }
    }

    float alpha_temp[kMlx90640PixelCount];
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const int row = p / kMlx90640Width;
        const int col = p % kMlx90640Width;
        float     v   = static_cast<float>(SignExtend((words[64 + p] & 0x03F0) >> 4, 6));
        v *= TwoPow(acc_rem_scale);
        v = alpha_ref + static_cast<float>(acc_row[row] << acc_row_scale) +
            static_cast<float>(acc_column[col] << acc_column_scale) + v;
        v /= alpha_scale_ee;
        v -= d->tgc * (d->cp_alpha[0] + d->cp_alpha[1]) / 2.0f;
        if (!(v > kAlphaTempMin))
        {
            return false;  // Alpha 区间校验失败
        }
        alpha_temp[p] = kScaleAlpha / v;
    }

    // 归一化：整体放大到最大值 ≥ 32767.4，存储为 uint16 范围（官方库约定）。
    float max_abs = alpha_temp[0];
    for (int p = 1; p < kMlx90640PixelCount; ++p)
    {
        if (alpha_temp[p] > max_abs)
        {
            max_abs = alpha_temp[p];
        }
    }
    int scale = 0;
    while (max_abs < 32767.4f)
    {
        if (scale >= kMaxScaleShift)
        {
            return false;  // 守卫：非法 EEPROM 导致无界放大
        }
        max_abs *= 2.0f;
        ++scale;
    }
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const float stored = std::round(alpha_temp[p] * TwoPow(scale));
        if (!(stored >= 1.0f && stored <= kAlphaStoredMax))
        {
            return false;  // Alpha 区间校验失败
        }
        d->alpha[p] = stored;
    }
    d->alpha_scale = static_cast<float>(scale);
    return true;
}

// 每像素偏置：word 16/17/18-31 + 每像素 word（位 15:10，int6）。
bool ExtractOffsetParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    const int occ_rem_scale    = words[16] & 0x000F;
    const int occ_column_scale = (words[16] & 0x00F0) >> 4;
    const int occ_row_scale    = (words[16] & 0x0F00) >> 8;
    const float offset_ref     = static_cast<float>(SignExtend(words[17], 16));

    int occ_row[24] = {};
    for (int i = 0; i < 6; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            occ_row[i * 4 + j] = NibbleSigned(words[18 + i], j * 4);
        }
    }
    int occ_column[32] = {};
    for (int i = 0; i < 8; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            occ_column[i * 4 + j] = NibbleSigned(words[24 + i], j * 4);
        }
    }

    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const int row = p / kMlx90640Width;
        const int col = p % kMlx90640Width;
        float     v   = static_cast<float>(SignExtend((words[64 + p] & 0xFC00) >> 10, 6));
        v *= TwoPow(occ_rem_scale);
        v = offset_ref + static_cast<float>(occ_row[row] << occ_row_scale) +
            static_cast<float>(occ_column[col] << occ_column_scale) + v;
        d->pix_os[p] = v;
    }
    return true;
}

// 每像素 kta：word 54/55 四象限基准 + 每像素 word（位 3:1，int3），归一化存储。
bool ExtractKtaPixelParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    int kta_rc[4];
    kta_rc[0] = SignExtend(words[54] >> 8, 8);  // 奇行奇列
    kta_rc[2] = SignExtend(words[54] & 0xFF, 8); // 偶行奇列
    kta_rc[1] = SignExtend(words[55] >> 8, 8);  // 奇行偶列
    kta_rc[3] = SignExtend(words[55] & 0xFF, 8); // 偶行偶列

    const float kta_scale1 = TwoPow(((words[56] & 0x00F0) >> 4) + 8);
    const float kta_scale2 = TwoPow(words[56] & 0x000F);

    float kta_temp[kMlx90640PixelCount];
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const int row   = p / kMlx90640Width;
        const int col   = p % kMlx90640Width;
        const int split = 2 * (row % 2) + col % 2;  // 官方库 2*(p/32 - (p/64)*2) + p%2
        float     v     = static_cast<float>(SignExtend((words[64 + p] & 0x000E) >> 1, 3));
        v = v * kta_scale2 + static_cast<float>(kta_rc[split]);
        kta_temp[p] = v / kta_scale1;
    }

    float max_abs = 0.0f;
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const float a = std::fabs(kta_temp[p]);
        if (a > max_abs)
        {
            max_abs = a;
        }
    }
    int scale = 0;
    while (max_abs < 63.4f)
    {
        if (scale >= kMaxScaleShift)
        {
            return false;  // 守卫：非法 EEPROM（如全零 kta）导致无界放大
        }
        max_abs *= 2.0f;
        ++scale;
    }
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        d->kta[p] = std::round(kta_temp[p] * TwoPow(scale));
    }
    d->kta_scale = static_cast<float>(scale);
    return true;
}

// 每像素 kv：word 52 四象限基准 ÷2^kvScaleEE，归一化存储。
bool ExtractKvPixelParameters(const std::uint16_t* words, Mlx90640EepromData* d)
{
    int kv_t[4];
    kv_t[0] = NibbleSigned(words[52], 12);  // 奇行奇列
    kv_t[2] = NibbleSigned(words[52], 8);   // 偶行奇列
    kv_t[1] = NibbleSigned(words[52], 4);   // 奇行偶列
    kv_t[3] = NibbleSigned(words[52], 0);   // 偶行偶列

    const float kv_scale_ee = TwoPow((words[56] >> 8) & 0xF);

    float kv_temp[kMlx90640PixelCount];
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const int row   = p / kMlx90640Width;
        const int col   = p % kMlx90640Width;
        const int split = 2 * (row % 2) + col % 2;  // 官方库 2*(p/32 - (p/64)*2) + p%2
        kv_temp[p]      = static_cast<float>(kv_t[split]) / kv_scale_ee;
    }

    float max_abs = 0.0f;
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        const float a = std::fabs(kv_temp[p]);
        if (a > max_abs)
        {
            max_abs = a;
        }
    }
    int scale = 0;
    while (max_abs < 63.4f)
    {
        if (scale >= kMaxScaleShift)
        {
            return false;  // 守卫：非法 EEPROM（如全零 kv）导致无界放大
        }
        max_abs *= 2.0f;
        ++scale;
    }
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        d->kv[p] = std::round(kv_temp[p] * TwoPow(scale));
    }
    d->kv_scale = static_cast<float>(scale);
    return true;
}

// 坏/离群像素标记：word 64+p == 0 → 坏；位 0 → 离群。各 ≤ 4 且总数 ≤ 4。
bool ExtractDeviatingPixels(const std::uint16_t* words, Mlx90640EepromData* d)
{
    int broken  = 0;
    int outlier = 0;
    for (int p = 0; p < kMlx90640PixelCount && broken <= kMlx90640MaxBadPixels &&
                    outlier <= kMlx90640MaxBadPixels;
         ++p)
    {
        if (words[64 + p] == 0)
        {
            if (broken < kMlx90640MaxBadPixels)
            {
                d->broken_pixels[broken] = static_cast<std::uint16_t>(p);
            }
            ++broken;
        }
        else if ((words[64 + p] & 0x0001) != 0)
        {
            if (outlier < kMlx90640MaxBadPixels)
            {
                d->outlier_pixels[outlier] = static_cast<std::uint16_t>(p);
            }
            ++outlier;
        }
    }
    return broken <= kMlx90640MaxBadPixels && outlier <= kMlx90640MaxBadPixels &&
           broken + outlier <= kMlx90640MaxBadPixels;
}

}  // namespace

Status ParseEeprom(const std::uint8_t eeprom[kMlx90640EepromSizeBytes], Mlx90640EepromData* out)
{
    if (eeprom == nullptr || out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: eeprom/output 不能为空指针");
    }

    std::uint16_t words[kMlx90640EepromWordCount];
    for (int i = 0; i < kMlx90640EepromWordCount; ++i)
    {
        words[i] = ReadWordBe(eeprom, i);
    }

    Mlx90640EepromData data;
    data.eeprom_version = words[0];
    data.cal_const      = words[5];

    // 版本字段防全 0/全 0xFF 镜像。
    if (data.eeprom_version == 0x0000 || data.eeprom_version == 0xFFFF)
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: EEPROM 版本字段非法");
    }

    ExtractVddParameters(words, &data);
    ExtractPtatParameters(words, &data);
    ExtractGainParameters(words, &data);
    ExtractTgcParameters(words, &data);
    ExtractResolutionParameters(words, &data);
    ExtractKsTaParameters(words, &data);
    ExtractKsToParameters(words, &data);
    ExtractCpParameters(words, &data);
    ExtractCilcParameters(words, &data);

    // 任务书要求的范围校验：Kt1/Kt2 合理区间。
    const float abs_kt1 = std::fabs(data.kt1);
    const float abs_kt2 = std::fabs(data.kt2);
    if (!(abs_kt1 >= kKt1MinAbs && abs_kt1 <= kKt1MaxAbs))
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: Kt1 超出合理区间");
    }
    if (!(abs_kt2 >= kKt2MinAbs && abs_kt2 <= kKt2MaxAbs))
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: Kt2 超出合理区间");
    }

    // 任务书要求的范围校验：Alpha 区间（内含归一化守卫）。
    if (!ExtractAlphaParameters(words, &data))
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: Alpha 超出合理区间");
    }
    ExtractOffsetParameters(words, &data);
    if (!ExtractKtaPixelParameters(words, &data))
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: 每像素 kta 缩放非法");
    }
    if (!ExtractKvPixelParameters(words, &data))
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: 每像素 kv 缩放非法");
    }

    // 任务书要求的范围校验：坏像素标记位。
    if (!ExtractDeviatingPixels(words, &data))
    {
        return Status(StatusCode::kInvalidArgument, "ParseEeprom: 坏/离群像素数量超限");
    }

    *out = data;
    return Status::MakeOk();
}

Status InterleaveSubpages(const std::uint16_t subpage0[kMlx90640PixelCount],
                          const std::uint16_t subpage1[kMlx90640PixelCount],
                          std::uint16_t       out[kMlx90640PixelCount])
{
    if (subpage0 == nullptr || subpage1 == nullptr || out == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "InterleaveSubpages: 输入/输出不能为空指针");
    }
    // 交错模式：subpage 0 = 偶数行、subpage 1 = 奇数行（官方库 ilPattern 规则）。
    for (int row = 0; row < kMlx90640Height; ++row)
    {
        for (int col = 0; col < kMlx90640Width; ++col)
        {
            const int p  = row * kMlx90640Width + col;
            out[p]       = (row % 2 == 0) ? subpage0[p] : subpage1[p];
        }
    }
    return Status::MakeOk();
}

bool IsBrokenPixel(const Mlx90640EepromData& eeprom, std::uint32_t pixel)
{
    for (int i = 0; i < 5; ++i)
    {
        if (eeprom.broken_pixels[i] == pixel || eeprom.outlier_pixels[i] == pixel)
        {
            return true;
        }
    }
    return false;
}

int ReplaceBrokenPixels(const Mlx90640EepromData& eeprom,
                        std::uint16_t            frame[kMlx90640PixelCount])
{
    if (frame == nullptr)
    {
        return -1;
    }
    int replaced = 0;
    for (int p = 0; p < kMlx90640PixelCount; ++p)
    {
        if (!IsBrokenPixel(eeprom, static_cast<std::uint32_t>(p)))
        {
            continue;
        }
        // 简化自官方库 BadPixelsCorrection 模式 0（同列相邻像素；UNVERIFIED 简化）。
        const int col = p % kMlx90640Width;
        if (col == 0)
        {
            frame[p] = frame[p + 1];
        }
        else if (col == kMlx90640Width - 1)
        {
            frame[p] = frame[p - 1];
        }
        else
        {
            const std::uint32_t sum =
                static_cast<std::uint32_t>(frame[p - 1]) + static_cast<std::uint32_t>(frame[p + 1]);
            frame[p] = static_cast<std::uint16_t>(sum / 2u);
        }
        ++replaced;
    }
    return replaced;
}

float CalcVdd(const Mlx90640EepromData& eeprom, std::uint16_t vdd_raw)
{
    if (eeprom.vdd_pixel == 0.0f)
    {
        return std::nanf("");
    }
    const float raw       = static_cast<float>(Sign16(vdd_raw));
    const float res_ee    = TwoPow(eeprom.resolution_ee);
    const float res_ram   = TwoPow(eeprom.resolution_ram == 0 ? eeprom.resolution_ee
                                                              : eeprom.resolution_ram);
    const float res_corr  = res_ee / res_ram;
    return (res_corr * raw - eeprom.vdd25) / eeprom.vdd_pixel + kVddNominal;
}

float CalcTa(const Mlx90640EepromData& eeprom, std::uint16_t ta_raw)
{
    if (eeprom.kt1 == 0.0f)
    {
        return std::nanf("");
    }
    const float ptat     = static_cast<float>(Sign16(ta_raw));
    const float ptat_art = static_cast<float>(Sign16(eeprom.pix_os_cp));
    const float denom    = ptat * eeprom.alpha_ptat + ptat_art;
    if (denom == 0.0f)
    {
        return std::nanf("");  // 分母为零守卫
    }
    const float vdd      = CalcVdd(eeprom, eeprom.vdd_raw);
    const float ptat_calc = (ptat / denom) * kTwoPow18;
    const float ta =
        (ptat_calc / (1.0f + eeprom.kt2 * (vdd - kVddNominal)) - eeprom.v_ptat25) / eeprom.kt1 +
        kRefTempC;
    return ta;
}

float TgcCompensation(const Mlx90640EepromData& eeprom, std::uint32_t pixel, std::uint16_t raw,
                      std::uint16_t subpage)
{
    if (pixel >= static_cast<std::uint32_t>(kMlx90640PixelCount) ||
        subpage >= static_cast<std::uint16_t>(kMlx90640SubpageCount))
    {
        return 0.0f;
    }
    if (IsBrokenPixel(eeprom, pixel))
    {
        return 0.0f;  // 坏像素丢弃；调用方可先 ReplaceBrokenPixels 替换
    }

    const int    gain_ram_s = Sign16(eeprom.gain_ram);
    const float gain_ratio =
        gain_ram_s > 0 ? eeprom.gain / static_cast<float>(gain_ram_s) : 1.0f;

    const float ta  = CalcTa(eeprom, eeprom.ta_raw);
    const float vdd = CalcVdd(eeprom, eeprom.vdd_raw);

    float ir = static_cast<float>(Sign16(raw)) * gain_ratio;
    const float kta = eeprom.kta[pixel] / TwoPow(static_cast<int>(eeprom.kta_scale));
    const float kv  = eeprom.kv[pixel] / TwoPow(static_cast<int>(eeprom.kv_scale));
    ir -= eeprom.pix_os[pixel] * (1.0f + kta * (ta - kRefTempC)) *
          (1.0f + kv * (vdd - kVddNominal));

    // TGC 补偿：减去 tgc × 补偿像素信号（cp_pixel 经增益/偏置修正）。
    float cp = static_cast<float>(Sign16(eeprom.cp_pixel[subpage])) * gain_ratio;
    cp -= eeprom.cp_offset[subpage] * (1.0f + eeprom.cp_kta * (ta - kRefTempC)) *
          (1.0f + eeprom.cp_kv * (vdd - kVddNominal));
    ir -= eeprom.tgc * cp;

    return ir;
}

float CalcTo(const Mlx90640EepromData& eeprom, std::uint32_t pixel, float v_ir_comp, float ta)
{
    if (pixel >= static_cast<std::uint32_t>(kMlx90640PixelCount))
    {
        return std::nanf("");
    }
    if (!(v_ir_comp > 0.0f) || !std::isfinite(ta))
    {
        return std::nanf("");  // 非正信号/非法温度无物理意义
    }

    // 发射率截断到 [kEmissivityMin, kEmissivityMax]。
    const float emissivity =
        eeprom.emissivity < kEmissivityMin ? kEmissivityMin
        : eeprom.emissivity > kEmissivityMax ? kEmissivityMax
                                             : eeprom.emissivity;
    const float ir_data = v_ir_comp / emissivity;

    // 反射温度 Tr 初稿取 Ta（任务书 CalcTo 无 Tr 入参；UNVERIFIED 假设）。
    float ta4 = ta + kCelsiusToKelvin;
    ta4       = ta4 * ta4;
    ta4       = ta4 * ta4;
    const float ta_tr = ta4;  // Tr == Ta 时 tr4 - (tr4 - ta4)/e == ta4

    if (eeprom.alpha[pixel] <= 0.0f)
    {
        return std::nanf("");
    }
    float alpha_comp = kScaleAlpha * TwoPow(static_cast<int>(eeprom.alpha_scale)) /
                       eeprom.alpha[pixel];
    alpha_comp *= (1.0f + eeprom.ks_ta * (ta - kRefTempC));

    // 一次解算 + 分段校正（官方库 CalculateTo）。
    float sx = alpha_comp * alpha_comp * alpha_comp * (ir_data + alpha_comp * ta_tr);
    sx       = std::sqrt(std::sqrt(sx)) * eeprom.ks_to[1];
    const float first_denom =
        alpha_comp * (1.0f - eeprom.ks_to[1] * kCelsiusToKelvin) + sx;
    float to = std::sqrt(std::sqrt(ir_data / first_denom + ta_tr)) - kCelsiusToKelvin;

    int range = 3;
    if (to < eeprom.ct[1])
    {
        range = 0;
    }
    else if (to < eeprom.ct[2])
    {
        range = 1;
    }
    else if (to < eeprom.ct[3])
    {
        range = 2;
    }

    float alpha_corr = 1.0f;
    if (range == 0)
    {
        alpha_corr = 1.0f / (1.0f + eeprom.ks_to[0] * 40.0f);
    }
    else if (range == 2)
    {
        alpha_corr = 1.0f + eeprom.ks_to[1] * eeprom.ct[2];
    }
    else if (range == 3)
    {
        alpha_corr = (1.0f + eeprom.ks_to[1] * eeprom.ct[2]) *
                     (1.0f + eeprom.ks_to[2] * (eeprom.ct[3] - eeprom.ct[2]));
    }

    const float denom =
        alpha_comp * alpha_corr * (1.0f + eeprom.ks_to[range] * (to - eeprom.ct[range]));
    if (denom <= 0.0f)
    {
        return std::nanf("");
    }
    return std::sqrt(std::sqrt(ir_data / denom + ta_tr)) - kCelsiusToKelvin;
}

}  // namespace animal_monitor
