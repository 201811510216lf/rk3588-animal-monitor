#include "animal_monitor/app/application.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "animal_monitor/app/mock_pipeline_service.h"
#include "animal_monitor/common/status.h"
#include "animal_monitor/logging/logger.h"

namespace animal_monitor {

namespace {

// 信号处理与 Application 的桥接：仅信号回调使用（C 回调无法捕获对象）。
Application* g_active_application = nullptr;

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT)
    {
        if (g_active_application != nullptr)
        {
            g_active_application->RequestShutdown();
        }
        return TRUE;
    }
    return FALSE;
}
#else
extern "C" void SignalHandler(int signal_number)
{
    if (g_active_application != nullptr && (signal_number == SIGINT || signal_number == SIGTERM))
    {
        g_active_application->RequestShutdown();
    }
}
#endif

}  // namespace

Application::Application(std::string config_path) : config_path_(std::move(config_path))
{
}

Status Application::Initialize()
{
    Status status = config::ConfigLoader::LoadFromFile(config_path_, &config_root_);
    if (!status)
    {
        return status;
    }

    std::string problems;
    status = config::ConfigValidator::Validate(config_root_, &problems);
    if (!status)
    {
        return Status(StatusCode::kConfigurationError, problems);
    }

    // 日志级别（logging.level）
    const config::ConfigValue* logging = config_root_.Find("logging");
    const config::ConfigValue* level   = logging->Find("level");
    std::string                level_text;
    if (level != nullptr && level->IsScalar())
    {
        level_text = level->Scalar();
    }
    if (level_text == "debug")
    {
        Logger::Instance().SetLevel(LogLevel::kDebug);
    }
    else if (level_text == "warning")
    {
        Logger::Instance().SetLevel(LogLevel::kWarning);
    }
    else if (level_text == "error")
    {
        Logger::Instance().SetLevel(LogLevel::kError);
    }
    else
    {
        Logger::Instance().SetLevel(LogLevel::kInfo);
    }

    return RegisterServices();
}

Status Application::RegisterServices()
{
    const config::ConfigValue* app = config_root_.Find("app");
    const config::ConfigValue* mock = app != nullptr ? app->Find("mock_pipeline") : nullptr;

    bool mock_enabled = false;
    if (mock != nullptr)
    {
        const config::ConfigValue* enabled = mock->Find("enabled");
        if (enabled != nullptr)
        {
            enabled->AsBool(&mock_enabled);
        }
    }

    // 自动退出时长（演示/自测用；0 = 一直运行直到收到退出信号）
    if (app != nullptr)
    {
        const config::ConfigValue* run_duration = app->Find("run_duration_ms");
        if (run_duration != nullptr)
        {
            run_duration->AsInt64(&auto_exit_ms_);
        }
    }

    // MOCK 流水线：仅验证基础设施接线，不代表任何真实视频/推理能力。
    if (mock_enabled)
    {
        std::int64_t producer_ms = 33;
        std::int64_t duration_ms = 3000;
        if (mock != nullptr)
        {
            const config::ConfigValue* interval = mock->Find("producer_interval_ms");
            const config::ConfigValue* duration = mock->Find("duration_ms");
            if (interval != nullptr)
            {
                interval->AsInt64(&producer_ms);
            }
            if (duration != nullptr)
            {
                duration->AsInt64(&duration_ms);
            }
        }
        services_.push_back(std::make_unique<MockPipelineService>(
            std::chrono::milliseconds(producer_ms), std::chrono::milliseconds(duration_ms)));
        service_notes_.push_back("MOCK：合成 FrameMeta 经有界队列流转，无真实硬件/像素");
    }

    // 其余服务（camera_service / video_pipeline / inference_service ...）随 Phase 2+ 注册。
    return Status::MakeOk();
}

std::vector<Application::ServiceStatus> Application::DescribeServices() const
{
    std::vector<ServiceStatus> result;
    result.reserve(services_.size());
    for (std::size_t i = 0; i < services_.size(); ++i)
    {
        ServiceStatus entry;
        entry.name = services_[i]->Name();
        entry.note = i < service_notes_.size() ? service_notes_[i] : "";
        result.push_back(std::move(entry));
    }
    return result;
}

void Application::RequestShutdown()
{
    stop_requested_.store(true, std::memory_order_relaxed);
}

int Application::Run()
{
#ifdef _WIN32
    g_active_application = this;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#else
    g_active_application = this;
    struct sigaction action
    {
    };
    action.sa_handler = SignalHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif

    // 启动服务
    std::vector<Service*> started;
    for (auto& service : services_)
    {
        const Status status = service->Start();
        if (!status)
        {
            AM_LOG_ERROR() << "service " << service->Name() << " start failed: "
                           << status.ToString();
        }
        else
        {
            started.push_back(service.get());
        }
    }

    AM_LOG_INFO() << "animal_monitor started with " << started.size() << " service(s)";
    for (auto& entry : DescribeServices())
    {
        std::cout << "[SERVICE] " << entry.name;
        if (!entry.note.empty())
        {
            std::cout << " (" << entry.note << ")";
        }
        std::cout << std::endl;
    }

    // 等待退出信号或自动退出时长（20ms 轮询，避免平台差异引入额外依赖）
    // 注意：不使用 time_point::max()，windows.h 的 max 宏会与之冲突。
    const auto auto_exit_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(auto_exit_ms_);
    while (!stop_requested_.load(std::memory_order_relaxed))
    {
        if (auto_exit_ms_ > 0 && std::chrono::steady_clock::now() >= auto_exit_deadline)
        {
            AM_LOG_INFO() << "auto exit after " << auto_exit_ms_ << " ms";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    AM_LOG_INFO() << "shutdown requested, stopping services";
    // 逆序停止：下游先停（依赖顺序与视频链路一致）
    for (auto it = started.rbegin(); it != started.rend(); ++it)
    {
        (*it)->Stop();
    }

    if (!started.empty())
    {
        for (Service* service : started)
        {
            const auto* snapshot = dynamic_cast<MockPipelineService*>(service);
            if (snapshot != nullptr)
            {
                const auto stats = snapshot->GetSnapshot();
                AM_LOG_INFO() << "mock_pipeline stats: produced=" << stats.produced
                              << " consumed=" << stats.consumed << " dropped=" << stats.dropped
                              << " queue_left=" << stats.queue_size;
            }
        }
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
#endif
    g_active_application = nullptr;
    return 0;
}

}  // namespace animal_monitor
