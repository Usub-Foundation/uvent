#include <stdexcept>
#include <string>

#include "test_common.h"
#include "uvent/Uvent.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    task::Awaitable<int> answer() { co_return 42; }

    task::Awaitable<std::string> concat(std::string a, std::string b)
    {
        co_await system::this_coroutine::sleep_for(1ms);
        co_return a + b;
    }

    task::Awaitable<int> thrower()
    {
        co_await system::this_coroutine::sleep_for(1ms);
        throw std::runtime_error("boom");
        co_return 0;
    }

    task::Awaitable<void> join_value_body(usub::Uvent* rt)
    {
        auto h1 = task::spawn(answer());
        auto h2 = task::spawn(concat("foo", "bar"), 1);
        const int v = co_await h1;
        CHECK_EQ(v, 42);
        const std::string s = co_await h2;
        CHECK(s == "foobar");

        auto h3 = task::spawn(thrower());
        bool caught = false;
        try
        {
            co_await h3;
        }
        catch (const std::runtime_error& e)
        {
            caught = std::string(e.what()) == "boom";
        }
        CHECK(caught);
        rt->stop();
    }

    void join_values_and_exceptions()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(join_value_body(&rt), 0);
        rt.run();
    }

    std::atomic<int> g_scope_done{0};

    task::Awaitable<void> scope_child(int ms)
    {
        co_await system::this_coroutine::sleep_for(std::chrono::milliseconds(ms));
        g_scope_done.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> scope_join_body(usub::Uvent* rt)
    {
        {
            task::TaskScope scope;
            for (int i = 0; i < 8; ++i)
                scope.spawn(scope_child(5 + i), i % 2);
            co_await scope.join();
            CHECK_EQ(g_scope_done.load(), 8);
            CHECK_EQ(scope.live_tasks(), 0u);
        }
        rt->stop();
    }

    void scope_join_waits_all()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(scope_join_body(&rt), 0);
        rt.run();
    }

    std::atomic<int> g_cancelled_children{0};

    task::Awaitable<void> stubborn_child()
    {
        while (!system::this_coroutine::cancel_requested())
        {
            if (!co_await system::this_coroutine::sleep_for(5ms))
                break;
        }
        g_cancelled_children.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> scope_cancel_body(usub::Uvent* rt)
    {
        task::TaskScope scope;
        for (int i = 0; i < 6; ++i)
            scope.spawn(stubborn_child(), i % 2);
        co_await system::this_coroutine::sleep_for(50ms);
        co_await scope.cancel_and_join();
        CHECK_EQ(g_cancelled_children.load(), 6);
        rt->stop();
    }

    void scope_cancel_and_join()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(scope_cancel_body(&rt), 0);
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }

    std::atomic<bool> g_nested_child_cancelled{false};

    task::Awaitable<void> nested_child()
    {
        if (!co_await system::this_coroutine::sleep_for(60s))
            g_nested_child_cancelled.store(true, std::memory_order_release);
    }

    task::Awaitable<void> nested_parent()
    {
        auto child = task::spawn(nested_child());
        co_await child;
    }

    task::Awaitable<void> nested_cancel_body(usub::Uvent* rt)
    {
        auto parent = task::spawn(nested_parent(), 1);
        co_await system::this_coroutine::sleep_for(50ms);
        parent.cancel();
        co_await parent;
        CHECK(g_nested_child_cancelled.load(std::memory_order_acquire));
        rt->stop();
    }

    void cancel_propagates_to_spawned_children()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(nested_cancel_body(&rt), 0);
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }

    void spawn_from_external_thread()
    {
        usub::Uvent rt(2);
        std::atomic<bool> ok{false};

        auto body = [&]() -> task::Awaitable<void>
        {
            co_await system::this_coroutine::sleep_for(1ms);
            ok.store(true, std::memory_order_release);
            rt.stop();
        };
        auto handle = task::spawn(body());
        CHECK(handle.valid());
        rt.run();
        CHECK(ok.load());
    }
} // namespace

int main()
{
    return run_tests({
        {"join_values_and_exceptions", join_values_and_exceptions},
        {"scope_join_waits_all", scope_join_waits_all},
        {"scope_cancel_and_join", scope_cancel_and_join},
#ifdef UVENT_ENABLE_REUSEADDR
        {"cancel_propagates_to_spawned_children", cancel_propagates_to_spawned_children},
#endif
        {"spawn_from_external_thread", spawn_from_external_thread},
    });
}
