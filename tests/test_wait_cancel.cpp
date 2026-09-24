// sync::OpWait cancellation paths: a waiter parked on an event or on a full
// channel is cancelled (on_cancel detaches it and resumes with false), and a
// task that is already cancelled never parks at all (own_cancel fast path).
#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/sync/AsyncChannel.h"
#include "uvent/sync/AsyncEvent.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    std::atomic<int> g_event_result{-1};

    task::Awaitable<void> event_waiter(sync::AsyncEvent* ev)
    {
        const bool ok = co_await ev->wait();
        g_event_result.store(ok ? 1 : 0, std::memory_order_release);
    }

    task::Awaitable<void> event_cancel_driver(usub::Uvent* rt)
    {
        sync::AsyncEvent ev{sync::Reset::Manual};
        auto h = task::spawn(event_waiter(&ev));
        co_await system::this_coroutine::sleep_for(50ms);
        CHECK_EQ(g_event_result.load(), -1); // still parked
        h.cancel();
        for (int i = 0; i < 200 && g_event_result.load() == -1; ++i)
            co_await system::this_coroutine::sleep_for(5ms);
        CHECK_EQ(g_event_result.load(), 0); // woke with false, event never set
        ev.set();                           // no dangling waiter left behind
        rt->stop();
    }

    void parked_event_waiter_is_cancelled()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(event_cancel_driver(&rt), 0);
        rt.run();
    }

    std::atomic<int> g_send_result{-1};

    task::Awaitable<void> blocked_sender(sync::AsyncChannel<int>* ch)
    {
        const bool ok = co_await ch->send(42);
        g_send_result.store(ok ? 1 : 0, std::memory_order_release);
    }

    task::Awaitable<void> send_cancel_driver(usub::Uvent* rt)
    {
        sync::AsyncChannel<int> ch(2);
        while (ch.try_send(1))
        {
        }
        auto h = task::spawn(blocked_sender(&ch));
        co_await system::this_coroutine::sleep_for(50ms);
        CHECK_EQ(g_send_result.load(), -1); // parked on the full channel
        h.cancel();
        for (int i = 0; i < 200 && g_send_result.load() == -1; ++i)
            co_await system::this_coroutine::sleep_for(5ms);
        CHECK_EQ(g_send_result.load(), 0);
        // The cancelled sender left nothing in the queue: draining yields only the fill values.
        sync::AsyncChannel<int>::value_type v; // std::tuple<int>
        int drained = 0;
        while (ch.try_recv(v))
        {
            CHECK_EQ(std::get<0>(v), 1);
            ++drained;
        }
        CHECK(drained >= 1);
        rt->stop();
    }

    void parked_sender_is_cancelled()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(send_cancel_driver(&rt), 0);
        rt.run();
    }

    std::atomic<int> g_precancelled_result{-1};

    task::Awaitable<void> precancelled_waiter(sync::AsyncEvent* ev)
    {
        // Spin until the parent has cancelled us, then try to wait.
        while (!system::this_coroutine::cancel_requested())
            co_await system::this_coroutine::yield();
        const bool ok = co_await ev->wait();
        g_precancelled_result.store(ok ? 1 : 0, std::memory_order_release);
    }

    task::Awaitable<void> precancel_driver(usub::Uvent* rt)
    {
        sync::AsyncEvent ev{sync::Reset::Manual};
        auto h = task::spawn(precancelled_waiter(&ev));
        co_await system::this_coroutine::sleep_for(20ms);
        h.cancel();
        for (int i = 0; i < 200 && g_precancelled_result.load() == -1; ++i)
            co_await system::this_coroutine::sleep_for(5ms);
        CHECK_EQ(g_precancelled_result.load(), 0); // never parked: immediate false
        rt->stop();
    }

    void already_cancelled_task_does_not_park()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(precancel_driver(&rt), 0);
        rt.run();
    }
} // namespace

int main()
{
    return run_tests({
#ifdef UVENT_ENABLE_REUSEADDR
        // Waking a parked task on cancel needs the cancel kick, which only the
        // REUSEADDR build has; without it cancellation is purely cooperative.
        {"parked_event_waiter_is_cancelled", parked_event_waiter_is_cancelled},
        {"parked_sender_is_cancelled", parked_sender_is_cancelled},
#endif
        {"already_cancelled_task_does_not_park", already_cancelled_task_does_not_park},
    });
}
