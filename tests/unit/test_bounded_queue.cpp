#include <atomic>
#include <chrono>
#include <thread>

#include "test_harness.h"

#include "animal_monitor/common/bounded_queue.h"

using animal_monitor::BoundedQueue;

namespace {

using namespace std::chrono_literals;

AM_TEST(push_pop_fifo_order)
{
    BoundedQueue<int> queue(4);
    for (int i = 1; i <= 4; ++i)
    {
        AM_EXPECT(queue.TryPush(i));
    }
    for (int i = 1; i <= 4; ++i)
    {
        int value = 0;
        AM_EXPECT(queue.TryPop(value));
        AM_EXPECT_EQ(value, i);
    }
}

AM_TEST(drop_newest_policy_keeps_oldest)
{
    BoundedQueue<int> queue(2, BoundedQueue<int>::PushPolicy::kDropNewest);
    AM_EXPECT(queue.TryPush(1));
    AM_EXPECT(queue.TryPush(2));
    AM_EXPECT(!queue.TryPush(3));  // 满：丢新

    int value = 0;
    AM_EXPECT(queue.TryPop(value));
    AM_EXPECT_EQ(value, 1);
    AM_EXPECT(queue.TryPop(value));
    AM_EXPECT_EQ(value, 2);

    const auto metrics = queue.GetMetrics();
    AM_EXPECT_EQ(metrics.drops, 1u);
}

AM_TEST(drop_oldest_policy_evicts_head)
{
    BoundedQueue<int> queue(2, BoundedQueue<int>::PushPolicy::kDropOldest);
    AM_EXPECT(queue.TryPush(1));
    AM_EXPECT(queue.TryPush(2));
    AM_EXPECT(queue.TryPush(3));  // 满：换出 1

    int value = 0;
    AM_EXPECT(queue.TryPop(value));
    AM_EXPECT_EQ(value, 2);
    AM_EXPECT(queue.TryPop(value));
    AM_EXPECT_EQ(value, 3);

    const auto metrics = queue.GetMetrics();
    AM_EXPECT_EQ(metrics.drops, 1u);
}

AM_TEST(reject_policy_counts_rejects)
{
    BoundedQueue<int> queue(1, BoundedQueue<int>::PushPolicy::kReject);
    AM_EXPECT(queue.TryPush(1));
    AM_EXPECT(!queue.TryPush(2));

    const auto metrics = queue.GetMetrics();
    AM_EXPECT_EQ(metrics.rejects, 1u);
}

AM_TEST(block_policy_wakes_on_pop)
{
    BoundedQueue<int> queue(1, BoundedQueue<int>::PushPolicy::kBlock);
    AM_EXPECT(queue.TryPush(1));

    std::atomic<bool> pushed{false};
    std::thread       pusher([&queue, &pushed] {
        pushed.store(queue.Push(2, 1s));
    });

    std::this_thread::sleep_for(50ms);
    int value = 0;
    AM_EXPECT(queue.TryPop(value));
    AM_EXPECT_EQ(value, 1);
    pusher.join();
    AM_EXPECT(pushed.load());

    AM_EXPECT(queue.TryPop(value));
    AM_EXPECT_EQ(value, 2);
}

AM_TEST(push_timeout_on_full_block_queue)
{
    BoundedQueue<int> queue(1, BoundedQueue<int>::PushPolicy::kBlock);
    AM_EXPECT(queue.TryPush(1));
    AM_EXPECT(!queue.Push(2, 100ms));  // 超时

    const auto metrics = queue.GetMetrics();
    AM_EXPECT_EQ(metrics.push_timeouts, 1u);
}

AM_TEST(pop_timeout_on_empty)
{
    BoundedQueue<int> queue(1);
    int               value = 0;
    AM_EXPECT(!queue.Pop(value, 100ms));

    const auto metrics = queue.GetMetrics();
    AM_EXPECT_EQ(metrics.pop_timeouts, 1u);
}

AM_TEST(close_wakes_waiters_and_drains)
{
    BoundedQueue<int> queue(4);
    AM_EXPECT(queue.TryPush(1));
    AM_EXPECT(queue.TryPush(2));

    std::thread popper([&queue] {
        int value = 0;
        while (queue.Pop(value, 10s))
        {
        }
    });

    std::this_thread::sleep_for(50ms);
    queue.Close();
    popper.join();

    AM_EXPECT(queue.IsClosed());
    AM_EXPECT(!queue.TryPush(3));  // 关闭后 push 失败
    int value = 0;
    AM_EXPECT(!queue.TryPop(value));  // 已排空
}

AM_TEST(capacity_zero_falls_back_to_one)
{
    BoundedQueue<int> queue(0);
    AM_EXPECT_EQ(queue.Capacity(), 1u);
    AM_EXPECT(queue.TryPush(42));
}

}  // namespace

int main()
{
    return am_test::RunAll();
}
