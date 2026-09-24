// UVENT_RUNTIME_DRAIN: Uvent::stop() cancels and drains parked tasks.
#include <atomic>
#include <chrono>

#include "test_common.h"
#include "uvent/Uvent.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

// Draining cancels parked tasks, which needs the cancel kick of REUSEADDR builds.
#if defined(UVENT_RUNTIME_DRAIN) && defined(UVENT_ENABLE_REUSEADDR)
namespace
{
    std::atomic<int> g_created{0}, g_destroyed{0}, g_cancelled{0};

    struct Guard
    {
        Guard() { g_created.fetch_add(1); }
        ~Guard() { g_destroyed.fetch_add(1); }
    };

    task::Awaitable<void> parked_coro()
    {
        Guard g;
        const bool completed = co_await system::this_coroutine::sleep_for(1h);
        CHECK(!completed);
        CHECK(system::this_coroutine::cancel_requested());
        g_cancelled.fetch_add(1);
    }

    task::Awaitable<void> polling_coro()
    {
        Guard g;
        while (!system::this_coroutine::cancel_requested())
            co_await system::this_coroutine::yield();
        g_cancelled.fetch_add(1);
    }

    task::Awaitable<void> stopper(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms);
        rt->stop(); // returns immediately; the workers drain and stop themselves
        co_await system::this_coroutine::sleep_for(1ms);
    }

    // Tasks parked on a 1h timer (any worker), pollers, scoped children and
    // fibers all unwind after stop(); no survivors, every destructor ran.
    void parked_tasks_unwind_on_stop()
    {
        g_created = g_destroyed = g_cancelled = 0;
        settings::stop_drain_timeout_ms = 5000;
        usub::Uvent rt(4);
        for (int i = 0; i < 8; ++i)
            task::spawn(parked_coro(), i % 4);
        for (int i = 0; i < 4; ++i)
            task::spawn(polling_coro(), i % 4);
        task::spawn(
            []() -> task::Awaitable<void>
            {
                task::TaskScope scope;
                for (int i = 0; i < 4; ++i)
                    scope.spawn(parked_coro(), i % 4);
                co_await scope.join();
            }());
#ifdef UVENT_ENABLE_FIBERS
        for (int i = 0; i < 4; ++i)
            task::spawn(fiber::run(
                            []
                            {
                                Guard g;
                                const bool completed = fiber::await(system::this_coroutine::sleep_for(1h));
                                CHECK(!completed);
                                g_cancelled.fetch_add(1);
                            }),
                        i % 4);
        constexpr int kFibers = 4;
#else
        constexpr int kFibers = 0;
#endif
        system::co_spawn_static(stopper(&rt), 0);
        const auto t0 = std::chrono::steady_clock::now();
        rt.run();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(elapsed < 3s);
        CHECK_EQ(g_created.load(), 16 + kFibers);
        CHECK_EQ(g_destroyed.load(), 16 + kFibers);
        CHECK_EQ(g_cancelled.load(), 16 + kFibers);
        CHECK_EQ(static_cast<long>(usub::Uvent::drain_survivors()), 0L);
    }

    task::Awaitable<void> stubborn_coro()
    {
        Guard g;
        for (;;) // never looks at cancel_requested(): cannot be drained (yield always re-queues)
            co_await system::this_coroutine::yield();
    }

    // A task that ignores cancellation only delays stop() by the deadline and
    // is reported as a survivor.
    void deadline_bounds_stragglers()
    {
        g_created = g_destroyed = g_cancelled = 0;
        settings::stop_drain_timeout_ms = 200;
        usub::Uvent rt(2);
        task::spawn(stubborn_coro(), 1);
        task::spawn(parked_coro(), 1);
        system::co_spawn_static(stopper(&rt), 0);
        const auto t0 = std::chrono::steady_clock::now();
        rt.run();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(elapsed >= 200ms);
        CHECK(elapsed < 3s);
        CHECK_EQ(g_cancelled.load(), 1);
        CHECK_EQ(static_cast<long>(usub::Uvent::drain_survivors()), 1L);
        settings::stop_drain_timeout_ms = 5000;
    }

    // Timeout 0 keeps the legacy behaviour: stop() returns and workers exit at once.
    void zero_timeout_is_legacy_stop()
    {
        g_created = g_destroyed = g_cancelled = 0;
        settings::stop_drain_timeout_ms = 0;
        usub::Uvent rt(2);
        task::spawn(parked_coro(), 1);
        system::co_spawn_static(stopper(&rt), 0);
        rt.run();
        CHECK_EQ(g_cancelled.load(), 0);
        settings::stop_drain_timeout_ms = 5000;
    }
} // namespace

int main()
{
    return run_tests({
        {"parked_tasks_unwind_on_stop", parked_tasks_unwind_on_stop},
        {"deadline_bounds_stragglers", deadline_bounds_stragglers},
        {"zero_timeout_is_legacy_stop", zero_timeout_is_legacy_stop},
    });
}
#else
int main()
{
    std::printf("skipped: build with -DUVENT_RUNTIME_DRAIN=ON -DUVENT_ENABLE_REUSEADDR=ON\n");
    return 0;
}
#endif
