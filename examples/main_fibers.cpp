// Fibers walkthrough: blocking-style code on the coroutine runtime.
//
//   1. a fiber echo server: accept/read/write without a single co_await
//   2. fiber clients talking to it, joined through a TaskScope
//   3. legacy-style deep recursion on a bigger fiber stack
//   4. cancellation: a fiber parked on a timer is woken by scope.cancel_and_join()
//   5. shutdown: Uvent::stop() (with UVENT_RUNTIME_DRAIN every parked fiber
//      unwinds through fiber::await; without it the process just exits)
//
// Build with -DUVENT_BUILD_EXAMPLES=ON (fibers are on by default), run: ./uvent_example_fibers

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "uvent/Uvent.h"
#include "uvent/sync/AsyncMutex.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    constexpr uint16_t kPort = 24800; // keep listeners below net.ipv4.ip_local_port_range

    // ---------------------------------------------------------------- 1. server
    // Plain blocking-looking loop. fiber::await parks only this fiber; the worker
    // keeps serving everything else.
    void echo_session(std::shared_ptr<net::TCPClientSocket> sock)
    {
        uint8_t buf[1024];
        for (;;)
        {
            const auto n = fiber::await(sock->async_read(buf, sizeof(buf)));
            if (n <= 0)
                return; // EOF, error or ECANCELED after cancellation
            std::size_t off = 0;
            while (off < static_cast<std::size_t>(n))
            {
                const auto w = fiber::await(sock->async_write(buf + off, static_cast<std::size_t>(n) - off));
                if (w <= 0)
                    return;
                off += static_cast<std::size_t>(w);
            }
        }
    }

    task::Awaitable<void> echo_server(std::atomic<bool>* stop)
    {
        co_await fiber::run([stop] {
            net::TCPServerSocket acceptor{"127.0.0.1", kPort};
            task::TaskScope sessions;
            while (!stop->load(std::memory_order_acquire))
            {
                auto client = fiber::await(acceptor.async_accept());
                if (!client)
                    continue;
                auto sp = std::make_shared<net::TCPClientSocket>(std::move(*client));
                sessions.spawn(fiber::run([sp] { echo_session(sp); }));
            }
            fiber::await(sessions.join());
        });
    }

    // ---------------------------------------------------------------- 2. client
    std::string echo_once(const std::string& msg)
    {
        net::TCPClientSocket sock;
        if (fiber::await(sock.async_connect("127.0.0.1", std::to_string(kPort), 2000ms)).has_value())
            return "<connect failed>";
        fiber::await(sock.async_write(reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data())), msg.size()));
        std::string back(msg.size(), '\0');
        std::size_t got = 0;
        while (got < msg.size())
        {
            const auto r = fiber::await(sock.async_read(reinterpret_cast<uint8_t*>(back.data()) + got, msg.size() - got));
            if (r <= 0)
                return "<read failed>";
            got += static_cast<std::size_t>(r);
        }
        return back;
    }

    // ------------------------------------------------------- 3. deep recursion
    // No co_await anywhere in the recursion: this is what fibers are for.
    long fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

    // ------------------------------------------------------------ 4. cancel
    task::Awaitable<void> demo(usub::Uvent* rt, std::atomic<bool>* stop_server)
    {
        // clients: fibers in a nursery, joined like any other tasks
        {
            task::TaskScope clients;
            for (int i = 0; i < 4; ++i)
                clients.spawn(fiber::run([i] {
                    const std::string reply = echo_once("hello from fiber " + std::to_string(i));
                    std::printf("[client %d] %s\n", i, reply.c_str());
                }));
            co_await clients.join();
        }

        // a shared counter under AsyncMutex from several fibers on several workers
        sync::AsyncMutex mtx;
        long counter = 0;
        {
            task::TaskScope workers;
            for (int i = 0; i < 8; ++i)
                workers.spawn(fiber::run([&] {
                    for (int k = 0; k < 100; ++k)
                    {
                        auto lock = fiber::await(mtx.lock());
                        const long v = counter;
                        fiber::yield(); // let others run while holding the lock: still correct
                        counter = v + 1;
                    }
                }), i % 2);
            co_await workers.join();
        }
        std::printf("[mutex] counter = %ld (expected 800)\n", counter);

        // deep recursion on a 1 MB stack (default is settings::fiber_stack_size, 256 KiB)
        const long f = co_await fiber::run([] { return fib(27); }, fiber::Options{1024 * 1024});
        std::printf("[stack] fib(27) = %ld\n", f);

        // cancellation reaches a fiber parked on a timer
        {
            task::TaskScope scope;
            scope.spawn(fiber::run([] {
                const bool completed = fiber::await(system::this_coroutine::sleep_for(1h));
                std::printf("[cancel] sleep_for returned %s, cancel_requested=%d\n", completed ? "true" : "false",
                            static_cast<int>(system::this_coroutine::cancel_requested()));
            }));
            co_await system::this_coroutine::sleep_for(10ms);
            co_await scope.cancel_and_join();
        }

        // ------------------------------------------------------- 5. shutdown
        stop_server->store(true, std::memory_order_release);
        {
            net::TCPClientSocket poke; // wake the acceptor so it re-checks the flag
            co_await poke.async_connect("127.0.0.1", std::to_string(kPort), 2000ms);
        }
        // a fiber left parked on purpose: with UVENT_RUNTIME_DRAIN stop() cancels it
        // and its destructor runs; without it the frame is simply abandoned.
        task::spawn(fiber::run([] {
            struct Bye { ~Bye() { std::printf("[drain] parked fiber unwound cleanly\n"); } } bye;
            fiber::await(system::this_coroutine::sleep_for(1h));
        }));
        co_await system::this_coroutine::sleep_for(10ms);
        std::printf("[stop] Uvent::stop()\n");
        rt->stop();
    }
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::atomic<bool> stop_server{false};
    usub::Uvent rt(2);
    system::co_spawn_static(echo_server(&stop_server), 0);
    system::co_spawn_static(demo(&rt, &stop_server), 1);
    rt.run();
#ifdef UVENT_RUNTIME_DRAIN
    std::printf("[exit] drain survivors: %zu\n", usub::Uvent::drain_survivors());
#else
    std::printf("[exit] built without UVENT_RUNTIME_DRAIN: the parked fiber was abandoned, its destructor never ran\n");
#endif
    return 0;
}
