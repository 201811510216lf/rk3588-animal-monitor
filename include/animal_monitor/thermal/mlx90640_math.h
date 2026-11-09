#pragma once

// ============================================================================
// 文件：include/animal_monitor/thermal/mlx90640_math.h
// 状态：implemented（纯数学，host 可编译，无 Linux 头；MSVC /W4 已验证）
// 依据：任务书 CODEX_MASTER_TASK.md §7.1；MLX90640 数据手册与 Melexis 官方
//       mlx90640-library（Apache-2.0）公开算法；README 接口决策（cdev + 用户态算温）
// 重要声明（NOT_CALIBRATED）：
//   - 本模块未使用真实器件验证，也未与参考测温设备比对标定。
//   - 全部输出均为“未标定估算”，禁止宣称准确体温/绝对温度。
//   - EEPROM 字段地址区间依据数据手册/官方库整理，标注 UNVERIFIED 的条目
//     须在板卡到位后用真实 EEPROM 镜像复核（scripts/test_i2c.sh 扩展）。
// 命名说明：任务书 API（ParseEeprom/InterleaveSubpages/TgcCompensation/CalcTa/CalcTo）
//   采用 PascalCase；字段名保留任务书命名（Kt1/Kt2/VddPixel/...），
//   括号注释对应 Melexis 官方库命名。
// ============================================================================

#include <cstdint>

#include "animal_monitor/common/status.h"

namespace animal_monitor {

// 器件几何与 EEPROM 布局常量。
constexpr int          kMlx90640PixelCount = 768;    // 32 × 24
constexpr int          kMlx90640Width      = 32;
constexpr int          kMlx90640Height     = 24;
constexpr int          kMlx90640EepromSizeBytes = 1664;  // 0x2400 起 832 个 16 位大端 word
constexpr int          kMlx90640EepromWordCount = 832;
constexpr std::uint16_t kMlx90640EepromBaseAddr = 0x2400;
constexpr int          kMlx90640SubpageCount = 2;
constexpr int          kMlx90640MaxBadPixels = 4;  // 坏/离群像素最大数（官方库阈值）

// 标定与帧状态合并结构。EEPROM 字段由 ParseEeprom 填充；
// “帧内 RAM 值”字段每帧由客户端/驱动从 RAM 尾随区刷新（非 EEPROM）。
struct Mlx90640EepromData
{
    // ================= EEPROM 全局标定 =================
    // word 索引 n 对应字节地址 0x2400 + 2n；全部 16 位大端。
    std::uint16_t eeprom_version = 0;  // word 0（0x2400）：EEPROM 版本，非法值 0x0000/0xFFFF
    std::uint16_t cal_const      = 0;  // word 5（0x2405）：标定常数，手册为 0x10（不强制校验）

    // 任务书 Kt1 = Melexis KtPTAT：PTAT 温度斜率（word 50 低 10 位，÷8），单位 1/K。
    float kt1 = 0.0f;
    // 任务书 Kt2 = Melexis KvPTAT：PTAT 电源斜率（word 50 高 6 位，÷4096），单位 1/V。
    float kt2 = 0.0f;
    // 任务书 VddPixel = Melexis kVdd：Vdd 灵敏度（word 51 高 8 位，int8 × 32），单位 LSB/V。
    float vdd_pixel = 0.0f;
    float vdd25      = 0.0f;  // 25°C 基准 Vdd 偏移（word 51 低 8 位，(v-256)×32-8192），LSB
    float v_ptat25   = 0.0f;  // 25°C PTAT 基准（word 49），LSB
    float alpha_ptat = 0.0f;  // PTAT 系数（word 16 高 4 位 ÷2^14 + 8），无量纲
    // 任务书 Gain = Melexis gainEE：增益基准（word 48，int16），LSB。
    float gain = 0.0f;
    // 任务书 Tgc = Melexis tgc：热梯度补偿系数（word 60 低 8 位，int8 ÷32），无量纲。
    float tgc = 0.0f;
    // 任务书 KsTa（word 60 高 8 位，int8 ÷8192），单位 1/K。
    float ks_ta = 0.0f;
    // 任务书 KsTo[0..3]（word 61/62 各两字节 int8，÷2^(word 63 低 4 位 + 8)），单位 1/K。
    float ks_to[4] = {};
    float ct[4]    = {};  // 分段拐点（°C）：ct0=-40、ct1=0、ct2/ct3 由 word 63 解算

    std::uint8_t resolution_ee        = 0;  // 分辨率配置（word 56 位 13:12）
    std::uint8_t calibration_mode_ee  = 0;  // 校准模式（word 10 位 11 取反 0x80；UNVERIFIED）
    float cp_kta     = 0.0f;  // 补偿像素 kta（word 59 低 8 位 ÷2^kta_scale1），1/K
    float cp_kv      = 0.0f;  // 补偿像素 kv（word 59 高 8 位 ÷2^kv_scale），1/V
    float cp_alpha[2] = {};   // 补偿像素灵敏度（word 57），无量纲
    float cp_offset[2] = {};  // 补偿像素偏移（word 58），LSB
    float il_chess_c[3] = {}; // 棋盘模式系数（word 53），无量纲（仅棋盘模式用）

    // ================= 每像素 OTP（word 64+p，p = row*32+col）================
    // 每个 word 打包：位 15:10 = 偏置（int6）、位 9:4 = alpha（int6）、
    // 位 3:1 = kta（int3）、位 0 = 离群标记；整 word == 0 视为坏像素。
    // 任务书 PixOs = 像素偏置（LSB，含 offsetRef + 行列增量 × 2^scale）。
    float pix_os[kMlx90640PixelCount] = {};
    // 任务书 Alpha = 像素灵敏度倒数（1e-6 缩放后归一化存储，取值 [1, 65535]）。
    float alpha[kMlx90640PixelCount] = {};
    float alpha_scale = 0.0f;  // alpha 归一化 2 的幂（word 32 高 4 位 + 30，再按需放大）
    float kta[kMlx90640PixelCount] = {};  // 像素 kta（归一化存储），使用前 ÷2^kta_scale
    float kta_scale   = 0.0f;  // kta 归一化 2 的幂（word 56 低 8 位 + 再归一化）
    float kv[kMlx90640PixelCount] = {};   // 像素 kv（归一化存储），使用前 ÷2^kv_scale
    float kv_scale    = 0.0f;  // kv 归一化 2 的幂（word 56 位 11:8 + 再归一化）

    // 坏/离群像素列表，0xFFFF 为结束哨兵（官方库约定；最多 kMlx90640MaxBadPixels 个）。
    std::uint16_t broken_pixels[5] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    std::uint16_t outlier_pixels[5] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};

    // ================= 帧内 RAM 值（每帧刷新，非 EEPROM）================
    // RAM 基址 0x0400：像素区 768 word，尾随区（0x0700 起 64 word）含换算因子。
    // 字偏移与官方库一致：word 768 = PTAT_ART、776/808 = CP 像素（subpage 0/1）、
    // 778 = Gain、800 = PTAT（Ta）、810 = VDD。
    std::uint16_t ta_raw   = 0;  // 任务书 Ta：帧内 PTAT 原始值（RAM word 800，0x0A40）
    std::uint16_t vdd_raw  = 0;  // 任务书 Vdd：帧内 VDD 原始值（RAM word 810，0x0A54）
    std::uint16_t gain_ram = 0;  // 帧内增益（RAM word 778，0x0A14）
    std::uint16_t pix_os_cp = 0; // 帧内补偿像素参考 PTAT_ART（RAM word 768，0x0A00）
    std::uint16_t cp_pixel[2] = {};  // 帧内 CP 像素原始值（RAM word 776/808，0x0A10/0x0A50）
    std::uint8_t  resolution_ram = 0;  // 帧内 CTRL1 分辨率快照（位 11:10）；
                                        // 初稿默认与 resolution_ee 一致（UNVERIFIED）
    // 任务书 Kt1/Kt2（RAM 尾随区另一组斜率字）与 Vth[2]（RAM 尾随区阈值字）：
    // 具体字偏移以数据手册 §10.2.2 为准，未在官方库中使用，暂预留为 0（UNVERIFIED）。
    float kt1_ram = 0.0f;
    float kt2_ram = 0.0f;
    float vth[2]  = {};

    // 任务书 Emissivity：目标物发射率，用户设定（默认 0.95，非 EEPROM；仅影响 CalcTo）。
    float emissivity = 0.95f;
};

// 解析 1664 字节大端 EEPROM 镜像（0x2400 起 832 word）并填充标定结构。
// 校验（任务书要求）：
//   - Kt1/Kt2 合理区间（编码最大值内的非零绝对值下限防全 0/全 0xFF 镜像）；
//   - Alpha 区间（每像素 alphaTemp 必须为正有限、归一化存储值 ∈ [1, 65535]）；
//   - 坏像素标记位（坏/离群像素各自 ≤ 4 且总数 ≤ 4，列表以 0xFFFF 结束）。
Status ParseEeprom(const std::uint8_t eeprom[kMlx90640EepromSizeBytes],
                   Mlx90640EepromData*    out);

// 交错模式子页合并：subpage 0 = 偶数行（0,2,4,...），subpage 1 = 奇数行（1,3,5,...），
// out[row*32+col] 取对应子页同位置像素（数据手册交错模式，官方库 ilPattern 规则）。
// 仅适用于交错模式（CTRL1 位 12 = 0）；棋盘模式不支持（本函数无从判断模式，调用方保证）。
Status InterleaveSubpages(const std::uint16_t subpage0[kMlx90640PixelCount],
                          const std::uint16_t subpage1[kMlx90640PixelCount],
                          std::uint16_t       out[kMlx90640PixelCount]);

// 单像素 TGC 补偿流水线（官方库 GetImage/CalculateTo 的逐像素部分）：
//   1) 16 位读数按有符号解释；2) 增益归一（gain_ee / gain_ram，gain_ram ≤ 0 时取 1）；
//   3) 偏置补偿：pix_os[p] × (1 + kta×(Ta-25)) × (1 + kv×(Vdd-3.3))；
//   4) TGC 补偿：减去 tgc × 补偿像素信号（cp_pixel[subpage] 经增益/偏置修正）。
// 内部调用 CalcTa/CalcVdd（读取 eeprom 的帧内 RAM 字段，须先刷新）。
// 坏/离群像素返回 0.0f（丢弃）；调用方可先用 ReplaceBrokenPixels 做相邻像素替换。
float TgcCompensation(const Mlx90640EepromData& eeprom, std::uint32_t pixel,
                      std::uint16_t raw, std::uint16_t subpage);

// 用相邻同列像素替换坏/离群像素（简化自官方库 BadPixelsCorrection 模式 0）：
// 列 0 → 取右邻；列 31 → 取左邻；其余 → 左右邻均值。原地修改 frame。
// 返回值：替换个数；frame 为 nullptr 返回 -1。
int ReplaceBrokenPixels(const Mlx90640EepromData& eeprom,
                        std::uint16_t frame[kMlx90640PixelCount]);

// 判断像素是否为坏/离群像素。
bool IsBrokenPixel(const Mlx90640EepromData& eeprom, std::uint32_t pixel);

// 电源电压估算（V）：Vdd = (2^resEE/2^resRAM × vdd_raw - vdd25) / VddPixel + 3.3。
// resolution_ram 与 resolution_ee 不一致时用 eeprom.resolution_ram 修正（初稿默认相等）。
float CalcVdd(const Mlx90640EepromData& eeprom, std::uint16_t vdd_raw);

// 环境温度估算（°C，未标定）：官方库 GetTa 公式。
// 需要 eeprom 帧内字段：vdd_raw（算 Vdd）、pix_os_cp（PTAT_ART）。
// 分母为零等非法输入返回 NaN（调用方置 kOutOfRange）。
float CalcTa(const Mlx90640EepromData& eeprom, std::uint16_t ta_raw);

// 目标物温度估算（°C，未标定）：官方库 CalculateTo 的逐像素公式。
// v_ir_comp 为 TgcCompensation 输出（含增益/偏置/TGC 补偿，未除发射率）；
// ta 为 CalcTa 输出（°C）。发射率取 eeprom.emissivity（截断到 [0.05, 1.0]）。
// 非法输入（坏像素 v_ir_comp = 0、负信号）返回 NaN。
float CalcTo(const Mlx90640EepromData& eeprom, std::uint32_t pixel, float v_ir_comp, float ta);

}  // namespace animal_monitor
