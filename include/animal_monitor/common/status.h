#pragma once

#include <string>

namespace animal_monitor {

// 全局错误码。所有跨模块边界返回 Status，禁止用 bool/int 表达错误语义。
enum class StatusCode
{
    kOk                    = 0,
    kCancelled             = 1,
    kInvalidArgument       = 2,
    kNotFound              = 3,
    kAlreadyExists         = 4,
    kPermissionDenied      = 5,
    kResourceExhausted     = 6,
    kUnavailable           = 7,
    kTimeout               = 8,
    kUnimplemented         = 9,
    kInternal              = 10,
    kConfigurationError    = 11,
    kHardwareUnavailable   = 12,
};

const char* StatusCodeName(StatusCode code);

// 轻量 Status：Ok() 为成功；失败时携带错误码与可读消息。
class Status
{
  public:
    Status();
    Status(StatusCode code, std::string message);

    // 工厂：构造成功状态（与查询方法 Ok() 区分）。
    static Status MakeOk();

    bool               Ok() const { return code_ == StatusCode::kOk; }
    StatusCode         Code() const { return code_; }
    const std::string& Message() const { return message_; }

    // 允许 if (!status) 判断失败
    explicit operator bool() const { return Ok(); }

    std::string ToString() const;

  private:
    StatusCode  code_;
    std::string message_;
};

}  // namespace animal_monitor
