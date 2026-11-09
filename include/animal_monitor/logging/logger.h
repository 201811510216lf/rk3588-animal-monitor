#pragma once

#include <mutex>
#include <sstream>
#include <string>

namespace animal_monitor
{

enum class LogLevel
{
    kDebug = 0,
    kInfo,
    kWarning,
    kError,
    kOff,
};

const char* LogLevelName(LogLevel level);

// 线程安全日志器：写 stderr（默认）。文件输出与轮转在 storage 阶段规划
// （status: planned，见 docs/architecture/initial_architecture.md）。
class Logger
{
   public:
    static Logger& Instance();

    void     SetLevel(LogLevel level);
    LogLevel GetLevel() const;

    void Log(LogLevel level, const char* file, int line, const std::string& message);

   private:
    Logger();

    mutable std::mutex mutex_;
    LogLevel           level_ = LogLevel::kInfo;
};

// 流式日志入口；析构时落盘。用法：
//   AM_LOG_INFO() << "camera " << id << " started";
class LogStream
{
   public:
    LogStream(LogLevel level, const char* file, int line);
    ~LogStream();

    LogStream(const LogStream&)            = delete;
    LogStream& operator=(const LogStream&) = delete;

    template <typename T>
    LogStream& operator<<(const T& value)
    {
        buffer_ << value;
        return *this;
    }

   private:
    LogLevel           level_;
    const char*        file_;
    int                line_;
    std::ostringstream buffer_;
};

}  // namespace animal_monitor

#define AM_LOG_DEBUG() \
    animal_monitor::LogStream(animal_monitor::LogLevel::kDebug, __FILE__, __LINE__)
#define AM_LOG_INFO() animal_monitor::LogStream(animal_monitor::LogLevel::kInfo, __FILE__, __LINE__)
#define AM_LOG_WARNING() \
    animal_monitor::LogStream(animal_monitor::LogLevel::kWarning, __FILE__, __LINE__)
#define AM_LOG_ERROR() \
    animal_monitor::LogStream(animal_monitor::LogLevel::kError, __FILE__, __LINE__)
