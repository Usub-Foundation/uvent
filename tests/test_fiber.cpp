#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/sync/AsyncMutex.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    // ------------------------------------------------------------------ basics
    task::Awaitable<int> coro_answer()
    {
        co_await system::this_coroutine::sleep_for(1ms);
        co_return 42;
    }

    task::Awaitable<void> basics_body(usub::Uvent* rt)
    {
        CHECK(!fiber::in_fiber());
        const int v = co_await fiber::run(
            []
            {
                CHECK(fiber::in_fiber());
                const bool slept = fiber::await(system::this_coroutine::sleep_for(2ms));
                CHECK(slept);
                const int a = fiber::await(coro_answer());
                fiber::yield();
                return a + 1;
            });
        CHECK_EQ(v, 43);
        CHECK(!fiber::in_fiber());

        std::string s = co_await fiber::run([] { return std::string("fi") + "ber"; });
        CHECK(s == "fiber");
        rt->stop();
    }

    void basics()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(basics_body(&rt), 0);
        rt.run();
    }

    // -------------------------------------------------------------- exceptions
    task::Awaitable<int> coro_thrower()
    {
        co_await system::this_coroutine::sleep_for(1ms);
        throw std::runtime_error("inner");
    }

    task::Awaitable<void> exceptions_body(usub::Uvent* rt)
    {
        // exception thrown by the body escapes through the host task
        bool caught = false;
        try
        {
            co_await fiber::run([] { throw std::runtime_error("body"); });
        }
        catch (const std::runtime_error& e)
        {
            caught = std::string(e.what()) == "body";
        }
        CHECK(caught);

        // exception thrown by an awaited coroutine lands inside the fiber
        const std::string got = co_await fiber::run(
            []
            {
                try
                {
                    fiber::await(coro_thrower());
                }
                catch (const std::runtime_error& e)
                {
                    return std::string(e.what());
                }
                return std::string("none");
            });
        CHECK(got == "inner");

        // via JoinHandle + spawn
        auto h = task::spawn(fiber::run([] { throw std::logic_error("spawned"); }));
        caught = false;
        try
        {
            co_await h;
        }
        catch (const std::logic_error&)
        {
            caught = true;
        }
        CHECK(caught);
        rt->stop();
    }

    void exceptions()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(exceptions_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------ spawn / join / mutex
    std::atomic<int> g_counter{0};

    task::Awaitable<void> structured_body(usub::Uvent* rt)
    {
        sync::AsyncMutex mtx;
        int shared = 0;
        {
            task::TaskScope scope;
            for (int i = 0; i < 16; ++i)
                scope.spawn(fiber::run(
                                [&mtx, &shared]
                                {
                                    for (int k = 0; k < 50; ++k)
                                    {
                                        auto g = fiber::await(mtx.lock());
                                        CHECK(g.owns_lock());
                                        const int v = shared;
                                        fiber::yield();
                                        shared = v + 1;
                                    }
                                    g_counter.fetch_add(1, std::memory_order_relaxed);
                                }),
                            i % 2);
            co_await scope.join();
        }
        CHECK_EQ(g_counter.load(), 16);
        CHECK_EQ(shared, 16 * 50);

        // fiber joins coroutine tasks and other fibers
        const int sum = co_await fiber::run(
            []
            {
                auto h1 = task::spawn(coro_answer());
                auto h2 = task::spawn(fiber::run([] { return 8; }), 1);
                return fiber::await(h1) + fiber::await(h2);
            });
        CHECK_EQ(sum, 50);
        rt->stop();
    }

    void structured()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(structured_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ cancellation
    // Proactive wake-up of a sleeping task on cancel exists only in the
    // REUSEADDR (per-thread wheel) runtime mode, same as for coroutines.
#ifdef UVENT_ENABLE_REUSEADDR
    task::Awaitable<void> cancel_body(usub::Uvent* rt)
    {
        std::atomic<int> stage{0};
        auto h = task::spawn(fiber::run(
            [&stage]
            {
                stage.store(1);
                const bool completed = fiber::await(system::this_coroutine::sleep_for(10s));
                stage.store(completed ? 2 : 3);
                CHECK(system::this_coroutine::cancel_requested());
            }));
        co_await system::this_coroutine::sleep_for(20ms);
        CHECK_EQ(stage.load(), 1);
        auto t0 = std::chrono::steady_clock::now();
        h.cancel();
        co_await h;
        CHECK_EQ(stage.load(), 3);
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        rt->stop();
    }

    void cancellation()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(cancel_body(&rt), 0);
        rt.run();
    }
#endif

    // Polling style works in every runtime mode: the fiber observes the
    // cancel flag of its host task between short sleeps.
    std::atomic<int> g_polled_cancelled{0};

    task::Awaitable<void> cancel_poll_body(usub::Uvent* rt)
    {
        task::TaskScope scope;
        for (int i = 0; i < 6; ++i)
            scope.spawn(fiber::run(
                            []
                            {
                                while (!system::this_coroutine::cancel_requested())
                                {
                                    if (!fiber::await(system::this_coroutine::sleep_for(5ms)))
                                        break;
                                }
                                g_polled_cancelled.fetch_add(1, std::memory_order_relaxed);
                            }),
                        i % 2);
        co_await system::this_coroutine::sleep_for(50ms);
        co_await scope.cancel_and_join();
        CHECK_EQ(g_polled_cancelled.load(), 6);
        rt->stop();
    }

    void cancellation_polling()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(cancel_poll_body(&rt), 0);
        rt.run();
    }

    // ---------------------------------------------------------- deep recursion
    int deep(int n)
    {
        volatile char pad[256];
        pad[0] = static_cast<char>(n);
        if (n == 0)
        {
            fiber::yield();
            return 0;
        }
        return deep(n - 1) + 1 + (pad[0] - static_cast<char>(n));
    }

    task::Awaitable<void> deep_body(usub::Uvent* rt)
    {
        // ~2000 frames * >256 bytes each: well beyond what a coroutine-only
        // design could nest without co_await, needs a 1 MB stack.
        const int r = co_await fiber::run([] { return deep(2000); }, fiber::Options{1024 * 1024});
        CHECK_EQ(r, 2000);
        rt->stop();
    }

    void deep_recursion()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(deep_body(&rt), 0);
        rt.run();
    }

    // -------------------------------------------------------------- stress
    task::Awaitable<void> stress_body(usub::Uvent* rt)
    {
        constexpr int kFibers = 400;
        constexpr int kSteps = 100;
        std::atomic<long> total{0};
        {
            task::TaskScope scope;
            for (int i = 0; i < kFibers; ++i)
                scope.spawn(fiber::run(
                                [&total]
                                {
                                    for (int k = 0; k < kSteps; ++k)
                                    {
                                        if ((k & 7) == 0)
                                            fiber::await(system::this_coroutine::sleep_for(1ms));
                                        else
                                            fiber::yield();
                                        total.fetch_add(1, std::memory_order_relaxed);
                                    }
                                }),
                            i % 4);
            co_await scope.join();
        }
        CHECK_EQ(total.load(), long(kFibers) * kSteps);
        rt->stop();
    }

    void stress()
    {
        usub::Uvent rt(4);
        system::co_spawn_static(stress_body(&rt), 0);
        rt.run();
    }

    // -------------------------------------------------------- forced unwind
    struct Never
    {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept {}
    };

    struct Guard
    {
        std::atomic<int>* flag;
        ~Guard() { flag->fetch_add(1); }
    };

    task::Awaitable<void> unwind_body(usub::Uvent* rt)
    {
        std::atomic<int> destroyed{0};
        std::atomic<int> swallowed{0};
        auto aw = fiber::run(
            [&destroyed, &swallowed]
            {
                Guard g{&destroyed};
                try
                {
                    fiber::await(Never{});
                }
                catch (...)
                {
                    // swallowing ForcedUnwind is wrong; the next await re-throws it
                    swallowed.fetch_add(1);
                    fiber::await(Never{});
                }
                CHECK(false);
            });
        auto* frame = aw.get_promise();
        frame->get_coroutine_handle().resume(); // run until the first await
        CHECK_EQ(destroyed.load(), 0);
        frame->destroy(); // host frame destroyed while the fiber is suspended
        CHECK_EQ(destroyed.load(), 1);
        CHECK_EQ(swallowed.load(), 1);
        rt->stop();
        co_return;
    }

    void forced_unwind()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(unwind_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------ misuse & nesting
    task::Awaitable<void> misuse_body(usub::Uvent* rt)
    {
        bool threw = false;
        try
        {
            fiber::await(system::this_coroutine::yield());
        }
        catch (const std::logic_error&)
        {
            threw = true;
        }
        CHECK(threw);

        const int nested = co_await fiber::run(
            []
            {
                return fiber::await(fiber::run(
                    []
                    {
                        fiber::yield();
                        return fiber::await(fiber::run([] { return 7; }));
                    }));
            });
        CHECK_EQ(nested, 7);
        rt->stop();
    }

    void misuse_and_nesting()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(misuse_body(&rt), 0);
        rt.run();
    }

#if !defined(_WIN32) && defined(UVENT_ENABLE_REUSEADDR)
    // --------------------------------------------------------------- sockets
    // Owner-forwarding layout only: on the legacy shared poller (UVENT_ENABLE_REUSEADDR=OFF) a fiber parked in
    // async_accept / async_read was not always resumed (release-no-reuseaddr timed out here), the same residual
    // race that keeps test_socket_io / test_socket_client out of that layout (docs/build-flags.md).
    constexpr uint16_t kPort = 24611; // below net.ipv4.ip_local_port_range: never collides with a client ephemeral port

    task::Awaitable<void> socket_body(usub::Uvent* rt)
    {
        const std::string echoed = co_await fiber::run(
            []() -> std::string
            {
                net::TCPServerSocket acceptor{"127.0.0.1", kPort};
                auto client = fiber::await(acceptor.async_accept());
                CHECK(client.has_value());
                uint8_t buf[64];
                std::string got;
                while (got.size() < 5)
                {
                    const ssize_t r = fiber::await(client->async_read(buf, sizeof(buf)));
                    CHECK(r > 0);
                    got.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(r));
                }
                const ssize_t w = fiber::await(client->async_write(reinterpret_cast<uint8_t*>(got.data()), got.size()));
                CHECK_EQ(w, static_cast<ssize_t>(got.size()));
                return got;
            });
        CHECK(echoed == "hello");
        rt->stop();
    }

    void sockets()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(socket_body(&rt), 0);
        std::string back;
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kPort);
                CHECK(::send(fd, "hello", 5, 0) == 5);
                char buf[16];
                std::size_t n = 0;
                while (n < 5)
                {
                    const ssize_t r = ::recv(fd, buf + n, sizeof(buf) - n, 0);
                    CHECK(r > 0);
                    n += static_cast<std::size_t>(r);
                }
                back.assign(buf, n);
                ::close(fd);
            });
        rt.run();
        client.join();
        CHECK(back == "hello");
    }
#endif
} // namespace

int main()
{
    return run_tests({
        {"basics", basics},
        {"exceptions", exceptions},
        {"structured", structured},
#ifdef UVENT_ENABLE_REUSEADDR
        {"cancellation", cancellation},
#endif
        {"cancellation_polling", cancellation_polling},
        {"deep_recursion", deep_recursion},
        {"stress", stress},
        {"forced_unwind", forced_unwind},
        {"misuse_and_nesting", misuse_and_nesting},
#if !defined(_WIN32) && defined(UVENT_ENABLE_REUSEADDR)
        {"sockets", sockets},
#endif
    });
}
