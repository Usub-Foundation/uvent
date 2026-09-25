// blocking::run — a function on the pool, the result back on the caller's worker. Covers: value and void results,
// exceptions, many concurrent jobs from several workers each resuming on its own worker, lazy thread creation,
// and use from a fiber through fiber::await.
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/blocking/Blocking.h"
#include "uvent/tasks/Task.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    task::Awaitable<void> value_body(usub::Uvent* rt)
    {
        const std::thread::id me = std::this_thread::get_id();
        std::thread::id pool_thread;
        const int v = co_await blocking::run(
            [&]
            {
                pool_thread = std::this_thread::get_id();
                std::this_thread::sleep_for(2ms);
                return 41 + 1;
            });
        CHECK_EQ(v, 42);
        CHECK(pool_thread != me);                          // it really ran elsewhere
        CHECK(std::this_thread::get_id() == me);           // and we are back on our worker
        CHECK_EQ(system::this_thread::detail::t_id, 0);

        std::string s = co_await blocking::run([] { return std::string("moved out"); });
        CHECK(s == "moved out");

        bool ran = false;
        co_await blocking::run([&] { ran = true; }); // void
        CHECK(ran);
        rt->stop();
    }

    void returns_value_on_own_worker()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(value_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ exceptions

    task::Awaitable<void> exception_body(usub::Uvent* rt)
    {
        bool caught = false;
        try
        {
            co_await blocking::run([]() -> int { throw std::runtime_error("from the pool"); });
        }
        catch (const std::runtime_error& e)
        {
            caught = std::string(e.what()) == "from the pool";
        }
        CHECK(caught);
        rt->stop();
    }

    void exception_propagates()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(exception_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ many jobs, several workers

    std::atomic<int> g_done{0};
    std::atomic<int> g_wrong_worker{0};

    task::Awaitable<void> one_job(int expect_tid, int i)
    {
        const int r = co_await blocking::run(
            [i]
            {
                if (i % 7 == 0)
                    std::this_thread::sleep_for(1ms);
                return i * 2;
            });
        CHECK_EQ(r, i * 2);
        if (system::this_thread::detail::t_id != expect_tid)
            g_wrong_worker.fetch_add(1);
        g_done.fetch_add(1);
    }

    task::Awaitable<void> spawner(int tid, int n)
    {
        task::TaskScope scope;
        for (int i = 0; i < n; ++i)
            scope.spawn(one_job(tid, i));
        co_await scope.join();
    }

    task::Awaitable<void> waiter_body(usub::Uvent* rt, int total)
    {
        while (g_done.load() < total)
            co_await system::this_coroutine::sleep_for(2ms);
        CHECK_EQ(g_wrong_worker.load(), 0);
        CHECK(blocking::detail::thread_count() >= 1);
        rt->stop();
    }

    void many_jobs_from_several_workers()
    {
        constexpr int per_worker = 300;
        usub::Uvent rt(3);
        system::co_spawn_static(spawner(0, per_worker), 0);
        system::co_spawn_static(spawner(1, per_worker), 1);
        system::co_spawn_static(spawner(2, per_worker), 2);
        system::co_spawn_static(waiter_body(&rt, 3 * per_worker), 0);
        rt.run();
    }

    // ------------------------------------------------------------ lazy threads / idle retirement

    task::Awaitable<void> lazy_body(usub::Uvent* rt)
    {
        settings::blocking_idle_timeout_ms = 30;
        const auto before = blocking::detail::thread_count();
        task::TaskScope scope;
        for (int i = 0; i < 8; ++i)
            scope.spawn([]() -> task::Awaitable<void>
                        { co_await blocking::run([] { std::this_thread::sleep_for(20ms); }); }());
        co_await system::this_coroutine::sleep_for(5ms);
        const auto during = blocking::detail::thread_count();
        CHECK(during > before);
        co_await scope.join();
        co_await system::this_coroutine::sleep_for(200ms); // idle timeout passed: threads retire
        CHECK(blocking::detail::thread_count() < during);
        settings::blocking_idle_timeout_ms = 10000;
        rt->stop();
    }

    void threads_are_lazy_and_retire()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(lazy_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ from a fiber

#ifdef UVENT_ENABLE_FIBERS
    task::Awaitable<void> fiber_body(usub::Uvent* rt)
    {
        const int v = co_await fiber::run(
            []
            {
                const int a = fiber::await(blocking::run([] { return 20; }));
                const int b = fiber::await(blocking::run([] { return 22; }));
                return a + b;
            });
        CHECK_EQ(v, 42);
        rt->stop();
    }

    void works_from_fiber()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(fiber_body(&rt), 0);
        rt.run();
    }
#endif
} // namespace

int main()
{
    return run_tests({
        {"returns_value_on_own_worker", returns_value_on_own_worker},
        {"exception_propagates", exception_propagates},
        {"many_jobs_from_several_workers", many_jobs_from_several_workers},
        {"threads_are_lazy_and_retire", threads_are_lazy_and_retire},
#ifdef UVENT_ENABLE_FIBERS
        {"works_from_fiber", works_from_fiber},
#endif
    });
}
