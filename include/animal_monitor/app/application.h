#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "animal_monitor/app/service.h"
#include "animal_monitor/config/config_loader.h"

namespace animal_monitor
{

// 应用容器：main 只负责参数解析与创建 Application；
// 配置加载、服务注册、启动/停止顺序、退出信号都在这里。
class Application
{
   public:
    explicit Application(std::string config_path);

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    // 加载并校验配置。失败返回非 Ok。
    Status Initialize();

    // 启动服务并等待退出信号；返回进程退出码。
    int Run();

    // 供信号处理器调用：请求优雅停机。
    void RequestShutdown();

    // 各服务当前状态（用于 --status 输出）。
    struct ServiceStatus
    {
        std::string name;
        std::string note;
    };
    std::vector<ServiceStatus> DescribeServices() const;

   private:
    Status RegisterServices();

    std::string                           config_path_;
    config::ConfigValue                   config_root_;
    std::atomic<bool>                     stop_requested_{false};
    std::vector<std::unique_ptr<Service>> services_;
    std::vector<std::string>              service_notes_;
    std::int64_t                          auto_exit_ms_ = 0;  // 0 = 等退出信号
};

}  // namespace animal_monitor
