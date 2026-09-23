// Runtime corners with no dedicated tests: exceptions crossing awaits,
// co_yield generators, TaskScope tokens/join, AsyncMutex::try_lock and Guard
// moves, AsyncEvent::reset, WaitGroup/Semaphore counters, select() when the
// channel wins after the sleep op is armed, select_recv over bounded channels,
// and a bounded recv() that parks, is cancelled, or sees close().
#include <stdexcept>
#include <string>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/sync/AsyncCancellation.h"
#include "uvent/sync/AsyncChannel.h"
#include "uvent/sync/AsyncEvent.h"
#include "uvent/sync/AsyncMutex.h"
#include "uvent/sync/AsyncSemaphore.h"
#include "uvent/sync/AsyncUnboundedChannel.h"
#include "uvent/sync/AsyncWaitGroup.h"
#include "uvent/sync/Select.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    // ------------------------------------------------------------ exceptions

    task::Awaitable<int> leaf_throws()
    {
        throw std::runtime_error("leaf failed");
        co_return 1;
    }

    task::Awaitable<int> middle()
    {
        const int v = co_await leaf_throws(); // rethrown here, propagates further up
        co_return v + 1;
    }

    task::Awaitable<void> void_throws()
    {
        co_await system::this_coroutine::sleep_for(1ms); // throw after a real suspension
        throw std::logic_error("void failed");
    }

    task::Awaitable<void> exception_body(usub::Uvent* rt)
    {
        bool caught = false;
        try
        {
            co_await middle();
        }
        catch (const std::runtime_error& e)
        {
            caught = std::string(e.what()) == "leaf failed";
        }
        CHECK(caught);

        caught = false;
        try
        {
            co_await void_throws();
        }
        catch (const std::logic_error& e)
        {
            caught = std::string(e.what()) == "void failed";
        }
        CHECK(caught);

        // The runtime is intact afterwards: timers and awaits keep working.
        co_await system::this_coroutine::sleep_for(5ms);
        CHECK_EQ(co_await []() -> task::Awaitable<int> { co_return 7; }(), 7);
        rt->stop();
    }

    void exceptions_propagate_through_await_chain()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(exception_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------- co_yield

    task::Awaitable<int, detail::AwaitableFrame<int>> counter(int n)
    {
        for (int i = 1; i <= n; ++i)
            co_yield i;
        co_return 0;
    }

    task::Awaitable<void> generator_body(usub::Uvent* rt)
    {
        auto g = counter(4);
        std::vector<int> got;
        for (;;)
        {
            got.push_back(co_await g);
            if (g.get_promise()->get_coroutine_handle().done())
                break;
        }
        CHECK_EQ(got.size(), 5u);
        for (int i = 0; i < 4; ++i)
            CHECK_EQ(got[static_cast<std::size_t>(i)], i + 1);
        CHECK_EQ(got[4], 0);
        rt->stop();
    }

    void generator_yields_in_order()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(generator_body(&rt), 0);
        rt.run();
    }

    // -------------------------------------------------- TaskScope + tokens

    std::atomic<int> g_scope_children_exited{0};

    task::Awaitable<void> scoped_child()
    {
        const auto tok = sync::current_token();
        CHECK(tok.valid());
        while (!tok.stop_requested())
            co_await system::this_coroutine::yield();
        g_scope_children_exited.fetch_add(1, std::memory_order_acq_rel);
    }

    task::Awaitable<void> scope_body(usub::Uvent* rt)
    {
        {
            task::TaskScope scope;
            const sync::CancellationToken tok = scope.token();
            CHECK(tok.valid());
            CHECK(!scope.cancel_requested());
            for (int i = 0; i < 3; ++i)
                scope.spawn(scoped_child());
            co_await system::this_coroutine::sleep_for(20ms);
            CHECK_EQ(scope.live_tasks(), 3u);
            CHECK_EQ(g_scope_children_exited.load(), 0);

            // token copies and moves keep pointing at the same state
            sync::CancellationToken copy;
            CHECK(!copy.valid());
            copy = tok;
            CHECK(copy.valid());
            sync::CancellationToken moved;
            moved = std::move(copy);
            CHECK(moved.valid());
            CHECK(!moved.stop_requested());

            scope.cancel();
            CHECK(scope.cancel_requested());
            CHECK(moved.stop_requested());
            co_await scope.join();
            CHECK_EQ(scope.live_tasks(), 0u);
            CHECK_EQ(g_scope_children_exited.load(), 3);
        }
        {
            // A source moved into another keeps its token working.
            sync::CancellationSource a;
            sync::CancellationSource b;
            b = std::move(a);
            const auto t = b.token();
            CHECK(t.valid());
            b.request_cancel();
            CHECK(t.stop_requested());
        }
        {
            // A scope parented to an explicit token inherits cancellation.
            sync::CancellationSource src;
            task::TaskScope scope{src.token()};
            src.request_cancel();
            CHECK(scope.cancel_requested());
            task::TaskScope detached{nullptr};
            CHECK(!detached.cancel_requested());
        }
        rt->stop();
    }

    void task_scope_tokens_and_join()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(scope_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------- AsyncMutex::try_lock

    std::atomic<bool> g_waiter_locked{false};

    task::Awaitable<void> mutex_waiter(sync::AsyncMutex* m)
    {
        auto g = co_await m->lock();
        CHECK(g.owns_lock());
        g_waiter_locked.store(true, std::memory_order_release);
    }

    task::Awaitable<void> mutex_body(usub::Uvent* rt)
    {
        sync::AsyncMutex m;
        auto g = m.try_lock();
        CHECK(g.owns_lock());
        auto g2 = m.try_lock();
        CHECK(!g2.owns_lock());

        sync::AsyncMutex::Guard moved;
        CHECK(!moved.owns_lock());
        moved = std::move(g);
        CHECK(moved.owns_lock());
        CHECK(!g.owns_lock());

        auto h = task::spawn(mutex_waiter(&m));
        co_await system::this_coroutine::sleep_for(20ms);
        CHECK(!g_waiter_locked.load()); // still held by `moved`
        moved.unlock();
        CHECK(!moved.owns_lock());
        for (int i = 0; i < 200 && !g_waiter_locked.load(); ++i)
            co_await system::this_coroutine::sleep_for(5ms);
        CHECK(g_waiter_locked.load());
        co_await h;

        auto again = m.try_lock(); // waiter's guard released at its exit
        CHECK(again.owns_lock());
        rt->stop();
    }

    void mutex_try_lock_and_guard_moves()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(mutex_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------ AsyncEvent::reset, counters

    std::atomic<int> g_event_wakeups{0};

    task::Awaitable<void> event_waiter(sync::AsyncEvent* ev)
    {
        if (co_await ev->wait())
            g_event_wakeups.fetch_add(1, std::memory_order_acq_rel);
    }

    task::Awaitable<void> event_body(usub::Uvent* rt)
    {
        sync::AsyncEvent ev{sync::Reset::Manual};
        CHECK(!ev.is_set());
        ev.set();
        CHECK(ev.is_set());
        CHECK(co_await ev.wait()); // already set: returns at once
        ev.reset();
        CHECK(!ev.is_set());
        auto h = task::spawn(event_waiter(&ev));
        co_await system::this_coroutine::sleep_for(20ms);
        CHECK_EQ(g_event_wakeups.load(), 0); // reset really cleared it: waiter parked
        ev.set();
        co_await h;
        CHECK_EQ(g_event_wakeups.load(), 1);

        sync::WaitGroup wg;
        CHECK_EQ(wg.count(), 0);
        wg.add(2);
        CHECK_EQ(wg.count(), 2);
        wg.done();
        CHECK_EQ(wg.count(), 1);
        wg.done();
        co_await wg.wait();

        sync::AsyncSemaphore sem{2};
        CHECK_EQ(sem.available(), 2);
        CHECK(sem.try_acquire());
        CHECK_EQ(sem.available(), 1);
        co_await sem.acquire();
        CHECK_EQ(sem.available(), 0);
        CHECK(!sem.try_acquire());
        sem.release(2);
        CHECK_EQ(sem.available(), 2);
        rt->stop();
    }

    void event_reset_and_counters()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(event_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------ select corners

    task::Awaitable<void> late_producer(sync::AsyncUnboundedChannel<int>* ch)
    {
        co_await system::this_coroutine::sleep_for(30ms);
        ch->try_send(11);
    }

    task::Awaitable<void> late_bounded_producer(sync::AsyncChannel<int>* ch)
    {
        co_await system::this_coroutine::sleep_for(30ms);
        CHECK(ch->try_send(21));
    }

    task::Awaitable<void> select_body(usub::Uvent* rt)
    {
        {
            // The sleep op is armed first; the channel wins later and the
            // sleep must be detached (no stray timer firing afterwards).
            sync::AsyncUnboundedChannel<int> ch;
            task::spawn(late_producer(&ch));
            const auto t0 = std::chrono::steady_clock::now();
            auto r = co_await sync::select(ch.recv_op(), sync::sleep_op(2s));
            CHECK(!r.cancelled());
            CHECK_EQ(r.index, 0);
            CHECK(std::chrono::steady_clock::now() - t0 < 1s);
        }
        {
            sync::AsyncChannel<int> a(2), b(2);
            CHECK(b.try_send(5));
            auto r = co_await sync::select_recv(a, b);
            CHECK(r.has_value());
            CHECK_EQ(r->first, 1u);
            CHECK_EQ(std::get<0>(r->second), 5);

            task::spawn(late_bounded_producer(&a)); // both empty now: must park, then a wins
            auto r2 = co_await sync::select_recv(a, b);
            CHECK(r2.has_value());
            CHECK_EQ(r2->first, 0u);
            CHECK_EQ(std::get<0>(r2->second), 21);
        }
        rt->stop();
    }

    void select_channel_wins_after_sleep_armed()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(select_body(&rt), 0);
        rt.run();
    }

    // -------------------------------------------- bounded recv park/cancel

    std::atomic<int> g_recv_state{-1}; // -1 pending, 0 nullopt, 1 value

    task::Awaitable<void> bounded_receiver(sync::AsyncChannel<int>* ch)
    {
        auto v = co_await ch->recv();
        g_recv_state.store(v.has_value() ? 1 : 0, std::memory_order_release);
    }

    task::Awaitable<void> bounded_recv_body(usub::Uvent* rt)
    {
        sync::AsyncChannel<int> ch(2);
        {
            auto h = task::spawn(bounded_receiver(&ch));
            co_await system::this_coroutine::sleep_for(20ms);
            CHECK_EQ(g_recv_state.load(), -1); // parked on the empty channel
            CHECK(ch.try_send(1));
            co_await h;
            CHECK_EQ(g_recv_state.load(), 1);
        }
#ifdef UVENT_ENABLE_REUSEADDR
        {
            // Cancel kick (REUSEADDR builds only) wakes the parked receiver.
            g_recv_state.store(-1);
            auto h = task::spawn(bounded_receiver(&ch));
            co_await system::this_coroutine::sleep_for(20ms);
            h.cancel(); // OpWait<ChannelRecvOp>::on_cancel
            for (int i = 0; i < 200 && g_recv_state.load() == -1; ++i)
                co_await system::this_coroutine::sleep_for(5ms);
            CHECK_EQ(g_recv_state.load(), 0);
        }
#endif
        {
            g_recv_state.store(-1);
            auto h = task::spawn(bounded_receiver(&ch));
            co_await system::this_coroutine::sleep_for(20ms);
            ch.close(); // wakes the parked receiver with nullopt
            co_await h;
            CHECK_EQ(g_recv_state.load(), 0);
            CHECK(!ch.try_send(2)); // closed
        }
        rt->stop();
    }

    void bounded_recv_parks_cancels_and_closes()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(bounded_recv_body(&rt), 0);
        rt.run();
    }
} // namespace

int main()
{
    return run_tests({
        {"exceptions_propagate_through_await_chain", exceptions_propagate_through_await_chain},
        {"generator_yields_in_order", generator_yields_in_order},
        {"task_scope_tokens_and_join", task_scope_tokens_and_join},
        {"mutex_try_lock_and_guard_moves", mutex_try_lock_and_guard_moves},
        {"event_reset_and_counters", event_reset_and_counters},
        {"select_channel_wins_after_sleep_armed", select_channel_wins_after_sleep_armed},
        {"bounded_recv_parks_cancels_and_closes", bounded_recv_parks_cancels_and_closes},
    });
}
