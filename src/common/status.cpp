#include "animal_monitor/common/status.h"

namespace animal_monitor {

const char* StatusCodeName(StatusCode code)
{
    switch (code)
    {
        case StatusCode::kOk:                  return "ok";
        case StatusCode::kCancelled:           return "cancelled";
        case StatusCode::kInvalidArgument:     return "invalid_argument";
        case StatusCode::kNotFound:            return "not_found";
        case StatusCode::kAlreadyExists:       return "already_exists";
        case StatusCode::kPermissionDenied:    return "permission_denied";
        case StatusCode::kResourceExhausted:   return "resource_exhausted";
        case StatusCode::kUnavailable:         return "unavailable";
        case StatusCode::kTimeout:             return "timeout";
        case StatusCode::kUnimplemented:       return "unimplemented";
        case StatusCode::kInternal:            return "internal";
        case StatusCode::kConfigurationError:  return "configuration_error";
        case StatusCode::kHardwareUnavailable: return "hardware_unavailable";
    }
    return "unknown";
}

Status::Status() : code_(StatusCode::kOk)
{
}

Status::Status(StatusCode code, std::string message) : code_(code), message_(std::move(message))
{
}

Status Status::MakeOk()
{
    return Status();
}

std::string Status::ToString() const
{
    if (Ok())
    {
        return "ok";
    }
    return std::string(StatusCodeName(code_)) + ": " + message_;
}

}  // namespace animal_monitor
