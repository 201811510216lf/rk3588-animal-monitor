#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace animal_monitor
{

// 有界线程安全 FIFO 队列。
//
// 每条流水线边都必须定义：容量、满队列策略、关停行为与指标
// （见 docs/architecture/thread_model.md）。默认策略 kDropNewest：
// 视频链路优先保留新鲜帧，防止延迟无限累积。
//
// Close() 用于确定性关停：唤醒所有等待者，Push 一律失败，
// Pop 先排空剩余元素再返回 false。
template <typename T>
class BoundedQueue
{
   public:
    enum class PushPolicy
    {
        kDropNewest,  // 丢弃新元素（默认）
        kDropOldest,  // 丢弃队头最老元素
        kReject,      // 拒绝入队并返回 false
        kBlock,       // 阻塞等待空间（受超时约束）
    };

    struct Metrics
    {
        std::uint64_t pushes            = 0;
        std::uint64_t pops              = 0;
        std::uint64_t drops             = 0;  // 满队列策略丢弃（含 drop_oldest 换出的）
        std::uint64_t rejects           = 0;
        std::uint64_t push_timeouts     = 0;
        std::uint64_t pop_timeouts      = 0;
        std::uint64_t closed_operations = 0;
    };

    explicit BoundedQueue(std::size_t capacity, PushPolicy policy = PushPolicy::kDropNewest)
        : capacity_(capacity == 0 ? 1 : capacity), policy_(policy)
    {}

    BoundedQueue(const BoundedQueue&)            = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    std::size_t Capacity() const { return capacity_; }

    bool IsClosed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    // 非阻塞入队；kBlock 策略下不等待（等同 reject）。
    bool TryPush(T item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return PushLocked(std::move(item));
    }

    // 限时入队；仅 kBlock 策略在满队列时等待。
    bool Push(T item, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_)
        {
            ++metrics_.closed_operations;
            return false;
        }
        if (queue_.size() < capacity_)
        {
            return PushLocked(std::move(item));
        }
        if (policy_ == PushPolicy::kBlock)
        {
            if (!not_full_.wait_for(lock, timeout,
                                    [this] { return closed_ || queue_.size() < capacity_; }))
            {
                ++metrics_.push_timeouts;
                return false;
            }
            if (closed_)
            {
                ++metrics_.closed_operations;
                return false;
            }
            return PushLocked(std::move(item));
        }
        return PushLocked(std::move(item));
    }

    bool TryPop(T& out)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return PopLocked(out);
    }

    // 限时出队；空队列时等待元素或 Close()。
    bool Pop(T& out, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); }))
        {
            ++metrics_.pop_timeouts;
            return false;
        }
        return PopLocked(out);
    }

    // 关停：唤醒所有等待者。此后 Push 一律失败，Pop 排空后返回 false。
    void Close()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t Size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    Metrics GetMetrics() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }

   private:
    bool PushLocked(T item)
    {
        if (closed_)
        {
            ++metrics_.closed_operations;
            return false;
        }
        if (queue_.size() < capacity_)
        {
            queue_.push_back(std::move(item));
            ++metrics_.pushes;
            not_empty_.notify_one();
            return true;
        }
        switch (policy_)
        {
            case PushPolicy::kDropNewest:
                ++metrics_.drops;
                return false;
            case PushPolicy::kDropOldest:
                queue_.pop_front();
                queue_.push_back(std::move(item));
                ++metrics_.pushes;
                ++metrics_.drops;
                not_empty_.notify_one();
                return true;
            case PushPolicy::kReject:
                ++metrics_.rejects;
                return false;
            case PushPolicy::kBlock:
                // 满队列且不等待（TryPush 路径）：按拒绝处理。
                ++metrics_.rejects;
                return false;
        }
        return false;
    }

    bool PopLocked(T& out)
    {
        if (queue_.empty())
        {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        ++metrics_.pops;
        not_full_.notify_one();
        return true;
    }

    mutable std::mutex      mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T>           queue_;
    const std::size_t       capacity_;
    const PushPolicy        policy_;
    bool                    closed_ = false;
    Metrics                 metrics_;
};

}  // namespace animal_monitor
