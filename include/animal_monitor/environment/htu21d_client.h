#pragma once

// ============================================================================
// 文件：include/animal_monitor/environment/htu21d_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证）
// 依据：任务书 CODEX_MASTER_TASK.md §7.2；kernel/drivers/htu21d/README.md（IIO 路线）
// 编译条件：仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建编译（依赖 Linux sysfs）。
// 读取契约（与 kernel/drivers/htu21d/htu21d.c 配套；UNVERIFIED：sysfs 路径以
// 目标内核实际导出为准）：
//   - 通道属性：in_temp_raw / in_temp_scale、in_humidityrelative_raw /
//     in_humidityrelative_scale；SCALE 仅为斜率（175.72/65536、125/65536），
//     常数项（-46.85 / -6）并入换算公式（htu21d_math.h 的 Htu21dTempC/Htu21dRhPercent）。
//   - 本驱动同时注册了 IIO_INFO_PROCESSED：in_*_input 会呈现处理后毫单位值
//     （毫摄氏度/毫 %RH）。客户端优先读 *_raw 并用公式换算；*_raw 不存在时
//     回退读 *_input 按毫单位 ÷1000 处理。
// 线程模型：无内置线程；采样由 EnvironmentService 按周期调用 ReadSample
//          （单次耗时约百毫秒级：器件转换时间 + sysfs I/O）。
// ============================================================================

#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor
{

class Htu21dClient
{
   public:
    // iio_device_name：IIO 设备名（如 "iio:device0"），实际编号由内核导出顺序决定，
    // 不硬编码默认值（板卡配置指定）。
    explicit Htu21dClient(std::string iio_device_name);

    Htu21dClient(const Htu21dClient&)            = delete;
    Htu21dClient& operator=(const Htu21dClient&) = delete;

    // 读取一次温湿度样本：temp_c（°C）、rh_pct（%RH）。
    Status ReadSample(float* temp_c, float* rh_pct);

    const std::string& DeviceName() const { return iio_device_name_; }

   private:
    std::string iio_device_name_;
    std::string base_path_;  // /sys/bus/iio/devices/<name>
};

}  // namespace animal_monitor
