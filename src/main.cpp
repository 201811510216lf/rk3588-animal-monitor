// animal_monitor 入口：只负责参数解析、依赖注入、服务启动、信号处理与退出。
// 业务逻辑一律放在 Application 与各 Service 中。
#include <iostream>
#include <string>

#include "animal_monitor/app/application.h"
#include "animal_monitor/common/status.h"
#include "animal_monitor/logging/logger.h"

namespace {

constexpr const char* kVersion   = "0.1.0";
constexpr const char* kDefaultConfig = "configs/default.yaml";

void PrintUsage(const char* program)
{
    std::cout << "用法: " << program << " [选项]\n"
              << "选项:\n"
              << "  --config <path>   YAML 配置文件（默认 " << kDefaultConfig << "）\n"
              << "  --log-level <l>   debug|info|warning|error（默认取配置文件）\n"
              << "  --help            显示本帮助\n"
              << "  --version         显示版本\n";
}

}  // namespace

int main(int argc, char** argv)
{
    std::string config_path = kDefaultConfig;
    std::string log_level;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--version")
        {
            std::cout << "animal_monitor " << kVersion << "\n";
            return 0;
        }
        if (arg == "--config")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--config 需要参数\n";
                return 2;
            }
            config_path = argv[++i];
        }
        else if (arg == "--log-level")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "--log-level 需要参数\n";
                return 2;
            }
            log_level = argv[++i];
        }
        else
        {
            std::cerr << "未知参数: " << arg << "\n";
            PrintUsage(argv[0]);
            return 2;
        }
    }

    if (log_level == "debug")
    {
        animal_monitor::Logger::Instance().SetLevel(animal_monitor::LogLevel::kDebug);
    }
    else if (log_level == "warning")
    {
        animal_monitor::Logger::Instance().SetLevel(animal_monitor::LogLevel::kWarning);
    }
    else if (log_level == "error")
    {
        animal_monitor::Logger::Instance().SetLevel(animal_monitor::LogLevel::kError);
    }

    animal_monitor::Application app(config_path);
    const animal_monitor::Status status = app.Initialize();
    if (!status)
    {
        std::cerr << "初始化失败: " << status.ToString() << "\n";
        return 1;
    }
    return app.Run();
}
