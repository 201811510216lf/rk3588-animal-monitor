#include "animal_monitor/logging/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>

namespace animal_monitor
{

namespace
{

// 线程安全的本地时间格式化（Windows localtime_s / POSIX localtime_r）。
std::string FormatLocalTime(const std::tm& tm_value)
{
    std::ostringstream out;
    out << std::put_time(&tm_value, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::tm ToLocalTime(const std::time_t& t)
{
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &t);
#else
    localtime_r(&t, &result);
#endif
    return result;
}

}  // namespace

const char* LogLevelName(LogLevel level)
{
    switch (level)
    {
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarning:
            return "WARNING";
        case LogLevel::kError:
            return "ERROR";
        case LogLevel::kOff:
            return "OFF";
    }
    return "UNKNOWN";
}

Logger& Logger::Instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() = default;

void Logger::SetLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::GetLevel() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::Log(LogLevel level, const char* file, int line, const std::string& message)
{
    if (level == LogLevel::kOff)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(level_))
    {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    const auto tm_value   = ToLocalTime(time_t_now);

    std::fprintf(stderr, "[%s.%03lld] [%s] %s:%d %s\n", FormatLocalTime(tm_value).c_str(),
                 static_cast<long long>(now_ms.count() % 1000), LogLevelName(level), file, line,
                 message.c_str());
    std::fflush(stderr);
}

LogStream::LogStream(LogLevel level, const char* file, int line)
    : level_(level), file_(file), line_(line)
{}

LogStream::~LogStream() { Logger::Instance().Log(level_, file_, line_, buffer_.str()); }

}  // namespace animal_monitor
