#pragma once

// ============================================================================
// 文件：include/animal_monitor/thermal/thermal_frame.h
// 状态：implemented（纯数学/数据结构，host 可编译，无 Linux 头）
// 依据：任务书 CODEX_MASTER_TASK.md §7.1；AGENTS.md（C++17、k 常量、尾下划线）
// 说明：
//   - ThermalFrame 为用户态热像帧容器：像素单位为摄氏（°C）、电压为伏特（V）。
//   - 温度数值来自未标定估算（NOT_CALIBRATED）：未经真实器件与参考测温设备比对标定，
//     禁止当作“准确体温”使用（AGENTS.md 规则 5）。
//   - status_bits 位定义与内核 UAPI（kernel/drivers/mlx90640/uapi/mlx90640.h）
//     保持同值，客户端原样透传并追加用户态解析状态位。
// ============================================================================

#include <cstdint>

#include "animal_monitor/common/frame_meta.h"

namespace animal_monitor {

// 热像帧状态位（值与内核 MLX90640_STATUS_* 宏一致，勿单独改动）。
namespace thermal_status_bits
{

constexpr std::uint32_t kSubpageOk        = 1u << 0;  // 两个 subpage 均有效并完成交错
constexpr std::uint32_t kEepromOk         = 1u << 1;  // EEPROM 标定解析通过
constexpr std::uint32_t kOutOfRange       = 1u << 2;  // 存在超出量程/无效像素（NaN/裁剪）
constexpr std::uint32_t kBrokenPixelMasked = 1u << 3; // 坏像素已用相邻像素替换
constexpr std::uint32_t kNotCalibrated    = 1u << 4;  // 未标定估算（本工程默认置位）
constexpr std::uint32_t kFrameOverflow    = 1u << 5;  // 帧号溢出/丢失帧
constexpr std::uint32_t kUnsupportedPattern = 1u << 6; // 非交错模式（未支持，预留）

}  // namespace thermal_status_bits

// 热像帧。像素按行优先排列：index = row * kWidth + col。
struct ThermalFrame
{
    static constexpr int kWidth      = 32;
    static constexpr int kHeight     = 24;
    static constexpr int kPixelCount = kWidth * kHeight;  // 768

    float            pixels[kPixelCount];  // 每像素目标温度估算，°C（NOT_CALIBRATED）
    FrameId          id;
    FrameTimestamps  timestamps;           // monotonic_ns 来自驱动 ktime_get_ns
    std::uint32_t    status_bits = 0;
    float            ta_c  = 0.0f;         // 器件环境温度估算，°C（未标定）
    float            vdd_v = 0.0f;         // 器件电源电压，V
};

// 判断单个像素温度是否可参与统计（有限且在器件量程内）。
bool IsValidPixelValue(float temp_c);

// 有效像素占比 [0,1]；全无效返回 0。
float ValidPixelRatio(const ThermalFrame& frame);

// 有效像素最小/平均/最大温度（°C）；无有效像素时 Min/Max 返回 0、Mean 返回 0。
float MinPixel(const ThermalFrame& frame);
float MaxPixel(const ThermalFrame& frame);
float MeanPixel(const ThermalFrame& frame);

// 最高温像素的线性索引（row*32+col）；无有效像素返回 -1。并列取最小索引。
int HotspotPixelIndex(const ThermalFrame& frame);

}  // namespace animal_monitor
