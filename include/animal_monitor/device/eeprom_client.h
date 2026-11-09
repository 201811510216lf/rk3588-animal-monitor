#pragma once

// ============================================================================
// 文件：include/animal_monitor/device/eeprom_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译）
// 依据：任务书 CODEX_MASTER_TASK.md §8（EEPROM 用 at24 驱动；校准/设备身份用途）。
// 说明：sysfs_path 为 at24 暴露的 eeprom 文件（如
//       "/sys/bus/i2c/devices/2-0050/eeprom"），I2C 总线号/地址由板卡配置传入
//       （UNVERIFIED）。写入是否被允许由 at24 驱动与设备树只读标志决定。
// 线程模型：无状态句柄，每次读写独立 open/pread/pwrite，并发安全；
//          同一区域并发写仍由上层负责互斥。
// 寿命策略：EEPROM 写入次数有限且 at24 无磨损均衡，写合并/日志式存储等
//           耐写策略由上层控制（本客户端只提供裸读写）。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor
{

// at24 EEPROM sysfs 客户端。
class EepromClient
{
   public:
    // sysfs_path：at24 eeprom 文件路径（配置传入）。
    explicit EepromClient(std::string sysfs_path);

    // 从 offset 读取 len 字节到 buf（open O_RDONLY + pread，支持短读聚合）。
    Status Read(std::uint64_t offset, std::uint8_t* buf, std::size_t len);

    // 从 offset 写入 len 字节（open O_WRONLY + pwrite，支持短写聚合）。
    Status Write(std::uint64_t offset, const std::uint8_t* buf, std::size_t len);

    const std::string& SysfsPath() const { return sysfs_path_; }

   private:
    std::string sysfs_path_;
};

}  // namespace animal_monitor
