#pragma once

// ============================================================================
// 文件：include/animal_monitor/communication/rs485_modbus_client.h
// 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未在目标板编译验证；
//       仅 ANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON 的目标构建参与编译，依赖 Linux 头）
// 依据：任务书 CODEX_MASTER_TASK.md §8（UART RS485 mode + TIOCSRS485 + Modbus RTU）。
// 线程模型：客户端不内置线程；同一实例的 Transact 并发调用需上层串行化
//          （Modbus RTU 总线上同一时刻只允许一个事务）。
// 说明：tty 路径、波特率、校验位全部由配置传入，不硬编码板卡串口编号（UNVERIFIED）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "animal_monitor/common/status.h"
#include "animal_monitor/communication/modbus_rtu.h"

namespace animal_monitor
{

// Modbus RTU 校验位（termios 支持范围内）。
enum class Rs485Parity
{
    kNone,
    kEven,
    kOdd,
};

// RS485 半双工 Modbus RTU 主站客户端。
// Open 流程：open O_RDWR|O_NOCTTY → termios 8N1/raw（校验位可配）→
// ioctl TIOCSRS485 启用半双工（SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND）；
// TIOCSRS485 失败视为非致命（部分板卡收发器自动方向控制，无需内核 RS485 模式），
// 记录到 LastRs485Warning()。
class Rs485ModbusClient
{
   public:
    // tty_path：串口节点（如 "/dev/ttyS1"）；baud：标准波特率（1200..230400）；
    // parity：校验位；timeout_ms：每段读取超时（毫秒，>0）。
    Rs485ModbusClient(std::string tty_path, int baud, Rs485Parity parity, int timeout_ms);

    // RAII：析构时关闭串口。
    ~Rs485ModbusClient();

    Rs485ModbusClient(const Rs485ModbusClient&)            = delete;
    Rs485ModbusClient& operator=(const Rs485ModbusClient&) = delete;

    Status Open();

    // 一次 Modbus RTU 事务：请求帧自动补 CRC 后写出，按 Modbus 响应最小长度
    // 分段读取（从站+功能码 → 功能码相关的剩余长度），ParseFrame 校验后返回。
    // 超时或 CRC/长度错误时整个事务自动重试一次；从站/功能码不匹配不重试
    // （对端应答异常，重试无意义）。异常响应（function&0x80）是合法结果，
    // 调用方用 IsException 判别。
    Status Transact(const ModbusFrame& request, ModbusFrame* response);

    // 幂等关闭（可重复调用）。
    void Close();

    bool               IsOpen() const { return fd_ >= 0; }
    const std::string& LastRs485Warning() const { return rs485_warning_; }

   private:
    // 从 fd_ 读满 count 字节（逐段 poll + read），返回 kTimeout 表示超时。
    Status ReadExactly(std::vector<std::uint8_t>* out, std::size_t count);

    std::string tty_path_;
    int         baud_;
    Rs485Parity parity_;
    int         timeout_ms_;
    int         fd_ = -1;
    std::string rs485_warning_;  // TIOCSRS485 失败等非致命告警（诊断用）
};

}  // namespace animal_monitor
