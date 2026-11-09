#pragma once

// ============================================================================
// 文件：include/animal_monitor/thermal/mlx90640_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证）
// 依据：任务书 CODEX_MASTER_TASK.md §7.1；kernel/drivers/mlx90640/README.md 接口决策
//       （cdev + 固定 UAPI + 每 read 主动采集）
// 编译条件：仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建编译（依赖 Linux 头）；
//          本机 Windows host 构建不编译本文件。目标构建需把 kernel/drivers 加入
//          include 路径（引用 kernel/drivers/mlx90640/uapi/mlx90640.h，CMake 后续接线）。
// 线程模型：客户端不内置线程/后台采集循环；读帧循环由 thermal_service 驱动
//          （外部以目标帧率调用 ReadFrame，阻塞至多 timeout_ms）。
// 声明：像素温度输出为未标定估算（NOT_CALIBRATED），禁止宣称准确体温。
// ============================================================================

#include <string>

#include "animal_monitor/common/status.h"
#include "animal_monitor/thermal/mlx90640_math.h"
#include "animal_monitor/thermal/thermal_frame.h"

namespace animal_monitor
{

class Mlx90640Client
{
   public:
    // device_path：cdev 节点路径（如 /dev/mlx90640_0），由板卡配置决定，不硬编码默认值。
    explicit Mlx90640Client(std::string device_path);

    // RAII：析构时关闭文件描述符。
    ~Mlx90640Client();

    Mlx90640Client(const Mlx90640Client&)            = delete;
    Mlx90640Client& operator=(const Mlx90640Client&) = delete;

    // 打开 cdev（O_RDWR | O_CLOEXEC）并校验 ABI 版本（ioctl MLX_IOC_GET_ABI），
    // 随后拉取标定参数（MLX_IOC_GET_PARAMS）并 ParseEeprom。
    // EEPROM 解析失败不视为 Open 失败：状态记录在 ReadFrame 的 status_bits 中
    // （kEepromOk 清零），此时像素填 NaN 便于诊断。
    Status Open();

    // 阻塞读取一帧（poll 等待至多 timeout_ms，timeout_ms < 0 表示无限等待）。
    // 内核每 read 主动采集两个 subpage 并交错合并；本函数负责 raw → 温度换算。
    Status ReadFrame(ThermalFrame* frame, int timeout_ms);

    // 幂等关闭（可重复调用）。
    void Close();

    bool                      IsOpen() const { return fd_ >= 0; }
    const Mlx90640EepromData& Eeprom() const { return eeprom_; }

   private:
    std::string        device_path_;
    int                fd_ = -1;
    Mlx90640EepromData eeprom_;
    bool               eeprom_ok_ = false;
};

}  // namespace animal_monitor
