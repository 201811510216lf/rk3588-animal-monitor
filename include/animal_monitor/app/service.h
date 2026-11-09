#pragma once

#include "animal_monitor/common/status.h"

namespace animal_monitor {

// 服务基类：所有可运行子系统（camera_service、video_pipeline、inference_service 等）
// 统一生命周期：Start() 成功后才允许进入运行态；Stop() 必须幂等且可安全重入。
class Service
{
  public:
    virtual ~Service() = default;

    // 展示名（状态表使用）。
    virtual const char* Name() const = 0;

    // 初始化并启动。失败返回非 Ok，且必须保证资源已回滚（RAII）。
    virtual Status Start() = 0;

    // 停止：唤醒等待者、排空/取消队列、释放下游引用后销毁资源。
    // 必须幂等；在 Start 前或 Start 失败后调用也应安全。
    virtual void Stop() = 0;
};

}  // namespace animal_monitor
