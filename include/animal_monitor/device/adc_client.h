#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/adc_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译）
// 依据：任务书 CODEX_MASTER_TASK.md §8（ADC 用 IIO；电压/电流等模拟量，可选）。
// 说明：IIO sysfs 路径 /sys/bus/iio/devices/<name>（IIO 标准路径），
//       iio_device_name（如 "iio:device0"）由配置传入；通道名固定为
//       "in_voltage<channel>"（单端电压语义），差分/其他通道类型需扩展
//       （板卡 ADC 拓扑 UNVERIFIED，不假设通道号与物理引脚映射）。
// 线程模型：客户端不内置线程；sysfs 读写原子性由内核保证，并发读安全。
// ============================================================================

#include <cstdint>
#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor
{

// IIO sysfs ADC 客户端：读取 in_voltageX_raw / in_voltageX_scale。
class AdcClient
{
   public:
    // iio_device_name：IIO 设备名（如 "iio:device0"），sysfs 基路径为
    // /sys/bus/iio/devices/<name>，由配置传入。
    explicit AdcClient(std::string iio_device_name);

    AdcClient(const AdcClient&)            = delete;
    AdcClient& operator=(const AdcClient&) = delete;

    // 校验 IIO 设备目录存在（stat 检查目录）。
    Status Open();

    // 读 in_voltage<channel>_raw 原始值（ADC code，整数）。
    Status ReadRaw(int channel, std::int32_t* raw);

    // 读原始值并按 scale 换算：先尝试通道级 in_voltage<channel>_scale，
    // 不存在则回退共享 in_voltage_scale。value = raw × scale（单精度浮点）。
    Status ReadScaled(int channel, float* value);

    // 幂等关闭（当前无资源持有，仅复位状态；保留以匹配生命周期契约）。
    void Close();

    bool IsOpen() const { return open_; }

   private:
    // 读取 sysfs 属性文件内容（去除尾部空白）；文件不存在返回 kHardwareUnavailable。
    Status ReadAttr(const std::string& attr_name, std::string* out);

    std::string device_name_;
    std::string base_path_;  // /sys/bus/iio/devices/<name>（Open 时确定）
    bool        open_ = false;
};

}  // namespace animal_monitor
