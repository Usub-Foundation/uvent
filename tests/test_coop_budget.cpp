#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/sync/AsyncUnboundedChannel.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    void hot_channel_loop_shares_thread()
    {
        constexpr int kIters = 20000;
        usub::Uvent rt(1);
        sync::AsyncUnboundedChannel<int> ch;
        std::atomic<int> hot_progress{0};
        std::atomic<int> hot_progress_when_other_ran{-1};
        std::atomic<bool> other_ran{false};

        auto hot = [&]() -> task::Awaitable<void>
        {
            for (int i = 0; i < kIters; ++i)
            {
                CHECK(ch.try_send(i));
                auto v = co_await ch.recv();
                CHECK(v.has_value());
                hot_progress.store(i + 1, std::memory_order_relaxed);
            }
            rt.stop();
        };
        auto other = [&]() -> task::Awaitable<void>
        {
            other_ran.store(true, std::memory_order_release);
            hot_progress_when_other_ran.store(hot_progress.load(std::memory_order_relaxed), std::memory_order_release);
            co_return;
        };
        system::co_spawn_static(hot(), 0);
        system::co_spawn_static(other(), 0);
        rt.run();
        CHECK(other_ran.load());
        CHECK(hot_progress_when_other_ran.load() < kIters);
    }

    void budget_setting_is_respected()
    {
        const auto saved = settings::coop_budget;
        settings::coop_budget = 16;

        usub::Uvent rt(1);
        sync::AsyncUnboundedChannel<int> ch;
        std::atomic<int> first_observed{-1};
        std::atomic<int> progress{0};

        auto hot = [&]() -> task::Awaitable<void>
        {
            for (int i = 0; i < 1000; ++i)
            {
                CHECK(ch.try_send(i));
                auto v = co_await ch.recv();
                CHECK(v.has_value());
                progress.store(i + 1, std::memory_order_relaxed);
            }
            rt.stop();
        };
        auto probe = [&]() -> task::Awaitable<void>
        {
            first_observed.store(progress.load(std::memory_order_relaxed), std::memory_order_release);
            co_return;
        };
        system::co_spawn_static(hot(), 0);
        system::co_spawn_static(probe(), 0);
        rt.run();
        settings::coop_budget = saved;
        CHECK(first_observed.load() >= 0);
        CHECK(first_observed.load() <= 64);
    }
} // namespace

namespace
{
    std::atomic<bool> g_timer_fired{false};
    std::atomic<bool> g_spinner_stop{false};

    task::Awaitable<void> yield_spinner()
    {
        while (!g_spinner_stop.load(std::memory_order_relaxed))
            co_await system::this_coroutine::yield();
    }

    task::Awaitable<void> timer_probe(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(10ms);
        g_timer_fired.store(true);
        g_spinner_stop.store(true);
        rt->stop();
    }

    // A task that yields in a tight loop keeps the run queue non-empty forever.
    // The worker must still tick its timer wheel and drain its inbox in between
    // (settings::loop_task_quantum), otherwise a timer on the same worker never fires.
    void hot_yield_loop_does_not_starve_timers()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(yield_spinner(), 0);
        system::co_spawn_static(timer_probe(&rt), 0);
        const auto t0 = std::chrono::steady_clock::now();
        rt.run();
        CHECK(g_timer_fired.load());
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }
    task::Awaitable<uint64_t> leaf(uint64_t v)
    {
        co_await system::this_coroutine::yield();
        co_return v * 2;
    }

    task::Awaitable<void> deep_awaiter(usub::Uvent* rt)
    {
        uint64_t sum = 0;
        for (uint64_t i = 0; i < 20000; ++i)
            sum += co_await leaf(i); // parent is queued instead of resumed inline (depth limit 0)
        CHECK_EQ(sum, 2 * (20000ull * 19999ull / 2));
        g_spinner_stop.store(true);
        rt->stop();
    }

    // With max_transfer_stack_depth forced to 0 every finished child defers its
    // parent into the run queue; a yield spinner keeps that queue non-empty so the
    // bounded loop reaches the frame-destroy phase before the parent resumes. The
    // child's frame must survive until the parent has read its result.
    void deferred_parent_reads_child_after_destroy_phase()
    {
        const auto saved = settings::max_transfer_stack_depth;
        settings::max_transfer_stack_depth = 0;
        g_spinner_stop.store(false);
        usub::Uvent rt(1);
        system::co_spawn_static(yield_spinner(), 0);
        system::co_spawn_static(deep_awaiter(&rt), 0);
        rt.run();
        settings::max_transfer_stack_depth = saved;
    }
} // namespace

int main()
{
    return run_tests({
        {"hot_yield_loop_does_not_starve_timers", hot_yield_loop_does_not_starve_timers},
        {"deferred_parent_reads_child_after_destroy_phase", deferred_parent_reads_child_after_destroy_phase},
        {"hot_channel_loop_shares_thread", hot_channel_loop_shares_thread},
        {"budget_setting_is_respected", budget_setting_is_respected},
    });
}
