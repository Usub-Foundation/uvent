#include <algorithm>
#include <cstdlib>

#include "test_common.h"
#include "uvent/Uvent.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    void source_token_basics()
    {
        sync::CancellationSource src;
        auto tok = src.token();
        CHECK(!tok.stop_requested());
        src.request_cancel();
        CHECK(tok.stop_requested());
        auto tok2 = tok;
        CHECK(tok2.stop_requested());
    }

    void tree_propagation()
    {
        sync::CancellationSource parent;
        sync::CancellationSource child{parent.token()};
        sync::CancellationSource grandchild{child.token()};
        CHECK(!grandchild.token().stop_requested());
        parent.request_cancel();
        CHECK(child.token().stop_requested());
        CHECK(grandchild.token().stop_requested());
    }

    void child_of_cancelled_is_born_cancelled()
    {
        sync::CancellationSource parent;
        parent.request_cancel();
        sync::CancellationSource child{parent.token()};
        CHECK(child.token().stop_requested());
    }

    std::atomic<bool> g_on_cancel_fired{false};

    task::Awaitable<void> on_cancel_waiter(usub::Uvent* rt, sync::CancellationToken tok)
    {
        const bool fired = co_await tok.on_cancel();
        CHECK(fired);
        g_on_cancel_fired.store(true, std::memory_order_release);
        rt->stop();
    }

    void on_cancel_wakeup()
    {
        usub::Uvent rt(2);
        sync::CancellationSource src;
        system::co_spawn_static(on_cancel_waiter(&rt, src.token()), 0);
        std::thread killer(
            [&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                src.request_cancel();
            });
        rt.run();
        killer.join();
        CHECK(g_on_cancel_fired.load());
    }

    std::atomic<bool> g_sleep_cancelled{false};

    task::Awaitable<void> long_sleeper()
    {
        const bool completed = co_await system::this_coroutine::sleep_for(60s);
        if (!completed)
            g_sleep_cancelled.store(true, std::memory_order_release);
    }

    task::Awaitable<void> sleep_cancel_driver(usub::Uvent* rt)
    {
        auto handle = task::spawn(long_sleeper());
        co_await system::this_coroutine::sleep_for(50ms);
        handle.cancel();
        co_await handle;
        CHECK(g_sleep_cancelled.load(std::memory_order_acquire));
        rt->stop();
    }

    void sleep_cancellation_is_prompt()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(sleep_cancel_driver(&rt), 0);
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(elapsed < 5s);
    }

    std::atomic<int> g_loop_ticks{0};

    task::Awaitable<void> cooperative_loop()
    {
        while (!system::this_coroutine::cancel_requested())
        {
            g_loop_ticks.fetch_add(1, std::memory_order_relaxed);
            if (!co_await system::this_coroutine::sleep_for(5ms))
                break;
        }
    }

    task::Awaitable<void> loop_cancel_driver(usub::Uvent* rt)
    {
        auto handle = task::spawn(cooperative_loop(), 1);
        co_await system::this_coroutine::sleep_for(100ms);
        handle.cancel();
        co_await handle;
        rt->stop();
    }

    void cross_thread_task_cancel()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(loop_cancel_driver(&rt), 0);
        rt.run();
        CHECK(g_loop_ticks.load() >= 1);
    }
    // Regression: a parent's request_cancel() walks its children under the tree
    // lock. A polling child that exits after seeing the parent's flag may drop
    // its last reference and block in unlink_from_parent() on that lock; the
    // walk then reached it with refs == 0 and kick() resurrected it into the
    // worker's kick queue, where it was popped after deletion (use-after-free).
    std::atomic<long> g_polled_done{0};

    task::Awaitable<void> polling_child(bool spin)
    {
        while (!system::this_coroutine::cancel_requested())
        {
            if (spin)
                co_await system::this_coroutine::yield(); // exits the moment the parent's flag is set
            else if (!co_await system::this_coroutine::sleep_for(1ms))
                break;
        }
        g_polled_done.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> scope_cancel_stress_driver(usub::Uvent* rt, int rounds)
    {
        constexpr int kChildren = 128;
        for (int r = 0; r < rounds; ++r)
        {
            task::TaskScope scope;
            for (int i = 0; i < kChildren; ++i)
                scope.spawn(polling_child((i & 1) != 0), i % 8);
            co_await system::this_coroutine::sleep_for(std::chrono::milliseconds(1 + r % 4));
            co_await scope.cancel_and_join();
        }
        CHECK_EQ(g_polled_done.load(), long(rounds) * kChildren);
        rt->stop();
    }

    void scope_cancel_polling_children_stress()
    {
        int rounds = 1000;
        if (const char* e = std::getenv("UVENT_TEST_SCALE"))
            if (auto d = std::strtol(e, nullptr, 10); d > 1)
                rounds = std::max(20, rounds / static_cast<int>(d));
        usub::Uvent rt(8);
        system::co_spawn_static(scope_cancel_stress_driver(&rt, rounds), 0);
        rt.run();
    }
} // namespace

int main()
{
    return run_tests({
        {"source_token_basics", source_token_basics},
        {"tree_propagation", tree_propagation},
        {"child_of_cancelled_is_born_cancelled", child_of_cancelled_is_born_cancelled},
        {"on_cancel_wakeup", on_cancel_wakeup},
#ifdef UVENT_ENABLE_REUSEADDR
        {"sleep_cancellation_is_prompt", sleep_cancellation_is_prompt},
#endif
        {"cross_thread_task_cancel", cross_thread_task_cancel},
#ifdef UVENT_ENABLE_REUSEADDR
        // Regression for the cancel-kick UAF; the kick only exists with REUSEADDR, and the
        // legacy shared-poller mode is known to starve timers under 8 workers (CI hang).
        {"scope_cancel_polling_children_stress", scope_cancel_polling_children_stress},
#endif
    });
}
