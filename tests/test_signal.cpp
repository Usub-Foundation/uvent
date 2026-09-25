// uvent::signal — sigaction + self-pipe driver on worker 0. Covers: a signal raised from a coroutine and
// from a foreign thread, coalescing of repeated deliveries, a SignalSet over two numbers reporting each
// once, delivery to several receivers of the same number, recv() cancelled by TaskScope, a signal that
// arrives before recv() is called (kept), and stop_on() stopping the runtime. POSIX only.
#include <atomic>
#include <csignal>
#include <thread>
#include <unistd.h>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/signal/Signal.h"
#include "uvent/sync/AsyncCancellation.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    task::Awaitable<void> raise_body(usub::Uvent* rt)
    {
        signal::Signal s(SIGUSR1);
        ::kill(::getpid(), SIGUSR1);
        CHECK(co_await s.recv());
        rt->stop();
    }

    void signal_raised_from_coroutine()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(raise_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ foreign thread + coalescing

    task::Awaitable<void> coalesce_body(usub::Uvent* rt)
    {
        signal::Signal s(SIGUSR2);
        std::thread t(
            []
            {
                std::this_thread::sleep_for(5ms);
                for (int i = 0; i < 5; ++i)
                    ::kill(::getpid(), SIGUSR2); // standard signals are not queued by the kernel
            });
        CHECK(co_await s.recv());
        t.join();
        co_await system::this_coroutine::sleep_for(20ms);
        // whatever the kernel merged, at most the 4 other deliveries can still be pending, and they
        // collapse into one recv()
        int extra = 0;
        while (s.try_recv())
            ++extra;
        CHECK(extra <= 1);
        rt->stop();
    }

    void repeated_deliveries_coalesce()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(coalesce_body(&rt), 1); // receiver on worker 1, driver on worker 0
        rt.run();
    }

    // ------------------------------------------------------------ set over two numbers

    task::Awaitable<void> set_body(usub::Uvent* rt)
    {
        signal::SignalSet set{SIGUSR1, SIGUSR2};
        ::kill(::getpid(), SIGUSR2);
        ::kill(::getpid(), SIGUSR1);
        const int a = co_await set.recv();
        const int b = co_await set.recv();
        CHECK((a == SIGUSR1 && b == SIGUSR2) || (a == SIGUSR2 && b == SIGUSR1));
        CHECK_EQ(set.try_recv(), -1);
        rt->stop();
    }

    void signal_set_reports_each_number()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(set_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ broadcast to several receivers

    std::atomic<int> g_got{0};

    task::Awaitable<void> receiver(signal::Signal* s)
    {
        if (co_await s->recv())
            g_got.fetch_add(1);
    }

    task::Awaitable<void> broadcast_body(usub::Uvent* rt)
    {
        signal::Signal a(SIGUSR1), b(SIGUSR1), c(SIGUSR1);
        task::TaskScope scope;
        scope.spawn(receiver(&a));
        scope.spawn(receiver(&b));
        scope.spawn(receiver(&c));
        co_await system::this_coroutine::sleep_for(5ms); // let them park
        ::kill(::getpid(), SIGUSR1);
        co_await scope.join();
        CHECK_EQ(g_got.load(), 3);
        rt->stop();
    }

    void every_receiver_gets_the_signal()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(broadcast_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ cancellation

    task::Awaitable<void> cancelled_receiver(signal::Signal* s, std::atomic<int>* result)
    {
        result->store(co_await s->recv() ? 1 : 2);
    }

    task::Awaitable<void> cancel_body(usub::Uvent* rt)
    {
        signal::Signal s(SIGUSR2);
        std::atomic<int> result{0};
        task::TaskScope scope;
        scope.spawn(cancelled_receiver(&s, &result));
        co_await system::this_coroutine::sleep_for(5ms);
        scope.cancel();
        co_await scope.join();
        CHECK_EQ(result.load(), 2); // recv() returned false, no signal was sent
        rt->stop();
    }

    void recv_returns_false_when_cancelled()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(cancel_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ delivery before recv() is kept

    task::Awaitable<void> early_body(usub::Uvent* rt)
    {
        signal::Signal s(SIGUSR1);
        ::kill(::getpid(), SIGUSR1);
        co_await system::this_coroutine::sleep_for(20ms); // driver ran long before we ask
        CHECK(s.try_recv());
        CHECK(!s.try_recv());
        rt->stop();
    }

    void delivery_before_recv_is_kept()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(early_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ stop_on

    task::Awaitable<void> stop_on_trigger()
    {
        co_await system::this_coroutine::sleep_for(10ms);
        ::kill(::getpid(), SIGTERM);
    }

    void stop_on_stops_the_runtime()
    {
        usub::Uvent rt(2);
        signal::stop_on(rt, {SIGINT, SIGTERM});
        system::co_spawn_static(stop_on_trigger(), 1);
        const auto t0 = std::chrono::steady_clock::now();
        rt.run(); // returns because stop_on() called rt.stop()
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 2000);
        signal::reset_to_default(SIGTERM);
        signal::reset_to_default(SIGINT);
    }

    // ------------------------------------------------------------ misc API

    void bad_numbers_are_rejected()
    {
        usub::Uvent rt(1);
        bool threw = false;
        try
        {
            signal::Signal s(SIGKILL);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try
        {
            signal::Signal s(0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw);
        rt.stop();
    }
} // namespace

int main()
{
    return run_tests({
        {"signal_raised_from_coroutine", signal_raised_from_coroutine},
        {"repeated_deliveries_coalesce", repeated_deliveries_coalesce},
        {"signal_set_reports_each_number", signal_set_reports_each_number},
        {"every_receiver_gets_the_signal", every_receiver_gets_the_signal},
        {"recv_returns_false_when_cancelled", recv_returns_false_when_cancelled},
        {"delivery_before_recv_is_kept", delivery_before_recv_is_kept},
        {"stop_on_stops_the_runtime", stop_on_stops_the_runtime},
        {"bad_numbers_are_rejected", bad_numbers_are_rejected},
    });
}
