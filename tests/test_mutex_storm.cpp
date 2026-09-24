// Regression stress for AsyncMutex hand-off storms on a single worker.
#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/sync/AsyncMutex.h"
#include "uvent/system/Settings.h"

#ifdef UVENT_ENABLE_REUSEADDR
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace usub::uvent;
using namespace std::chrono_literals;
using clock_t_ = std::chrono::steady_clock;
#ifdef UVENT_ENABLE_REUSEADDR
using usub::utils::errors::ConnectError;
#endif

namespace
{
    int scale()
    {
        if (const char* e = std::getenv("UVENT_TEST_SCALE"))
            return std::max(1, std::atoi(e));
        return 1;
    }

    struct Storm
    {
        sync::AsyncMutex m;
        std::atomic<int> done{0};
        std::atomic<int> dropped{0};
        std::atomic<int> ran{0};
        std::atomic<int> journaled{0};
    };

    task::Awaitable<void> stop_runtime(usub::Uvent& rt)
    {
        rt.stop();
        co_return;
    }

    task::Awaitable<void> journal(Storm& s)
    {
        co_await system::this_coroutine::sleep_for(1ms);
        s.journaled.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<bool> run_hook(Storm& s, std::string request, clock_t_::time_point deadline,
                                   std::chrono::milliseconds hold)
    {
        auto g = co_await s.m.lock();
        CHECK(g.owns_lock());
        if (clock_t_::now() > deadline)
        {
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            co_return false;
        }
        s.ran.fetch_add(1, std::memory_order_relaxed);
        if (hold.count() > 0)
            co_await system::this_coroutine::sleep_for(hold);
        CHECK(request.size() > 0);
        co_return true;
    }

    task::Awaitable<bool> create_impl(Storm& s, int i, clock_t_::time_point deadline, std::chrono::milliseconds hold)
    {
        std::vector<char> body(4096, static_cast<char>('a' + i % 26));
        std::string request(body.begin(), body.end());
        const bool ok = co_await run_hook(s, request + "|hook", deadline, hold);
        if (!ok)
            system::co_spawn(journal(s));
        co_return ok;
    }

    task::Awaitable<void> rpc(Storm& s, int i, clock_t_::time_point deadline, std::chrono::milliseconds hold)
    {
        (void)co_await create_impl(s, i, deadline, hold);
        s.done.fetch_add(1, std::memory_order_release);
    }

    constexpr int kWaiters = 24;

    void spawn_round(Storm& s, std::chrono::milliseconds hold)
    {
        const auto now = clock_t_::now();
        system::co_spawn(rpc(s, 0, now + 10s, hold));
        for (int i = 1; i <= kWaiters; ++i)
            system::co_spawn(rpc(s, i, now + hold / 4, 0ms));
    }

    void handoff_storm_single_worker()
    {
        const int rounds = 150 * scale();
        usub::Uvent rt(1);
        Storm s;
        auto driver = [&]() -> task::Awaitable<void> {
            for (int r = 0; r < rounds; ++r)
            {
                const int target = s.done.load(std::memory_order_acquire) + kWaiters + 1;
                spawn_round(s, 8ms);
                while (s.done.load(std::memory_order_acquire) < target)
                    co_await system::this_coroutine::sleep_for(1ms);
            }
            for (int i = 0; i < 2000 && s.journaled.load(std::memory_order_acquire) < rounds * kWaiters; ++i)
                co_await system::this_coroutine::sleep_for(1ms);
            rt.stop();
        };
        system::co_spawn(driver());
        rt.run();
        CHECK_EQ(s.done.load(), rounds * (kWaiters + 1));
        CHECK_EQ(s.ran.load(), rounds);
        CHECK_EQ(s.dropped.load(), rounds * kWaiters);
        CHECK_EQ(s.journaled.load(), rounds * kWaiters);
    }

    void handoff_storm_foreign_spawn()
    {
        const int rounds = 100 * scale();
        usub::Uvent rt(1);
        Storm s;
        std::atomic<bool> finished{false};
        std::thread producer([&] {
            for (int r = 0; r < rounds; ++r)
            {
                const int target = s.done.load(std::memory_order_acquire) + kWaiters + 1;
                spawn_round(s, 8ms);
                while (s.done.load(std::memory_order_acquire) < target)
                    std::this_thread::sleep_for(1ms);
            }
            for (int i = 0; i < 2000 && s.journaled.load(std::memory_order_acquire) < rounds * kWaiters; ++i)
                std::this_thread::sleep_for(1ms);
            finished.store(true, std::memory_order_release);
            system::co_spawn(stop_runtime(rt));
        });
        rt.run();
        producer.join();
        CHECK(finished.load());
        CHECK_EQ(s.done.load(), rounds * (kWaiters + 1));
        CHECK_EQ(s.dropped.load(), rounds * kWaiters);
    }

    void handoff_storm_mixed_deadlines()
    {
        const int rounds = 100 * scale();
        usub::Uvent rt(1);
        Storm s;
        auto driver = [&]() -> task::Awaitable<void> {
            for (int r = 0; r < rounds; ++r)
            {
                const int target = s.done.load(std::memory_order_acquire) + kWaiters + 1;
                const auto now = clock_t_::now();
                system::co_spawn(rpc(s, 0, now + 10s, 6ms));
                for (int i = 1; i <= kWaiters; ++i)
                    system::co_spawn(rpc(s, i, (i % 2) ? now + 10s : now + 1ms, (i % 2) ? 1ms : 0ms));
                while (s.done.load(std::memory_order_acquire) < target)
                    co_await system::this_coroutine::sleep_for(1ms);
            }
            co_await system::this_coroutine::sleep_for(20ms);
            rt.stop();
        };
        system::co_spawn(driver());
        rt.run();
        CHECK_EQ(s.done.load(), rounds * (kWaiters + 1));
        CHECK_EQ(s.ran.load() + s.dropped.load(), rounds * (kWaiters + 1));
    }

    struct Heavy
    {
        std::string a;
        std::vector<int> b;
        Heavy() : a(96, 'q'), b(64, 7) {}
    };

    task::Awaitable<Heavy> new_table(Storm& s)
    {
        auto g = co_await s.m.lock();
        CHECK(g.owns_lock());
        Heavy h;
        h.a += "|table";
        co_return h;
    }

    task::Awaitable<bool> run_hook_val(Storm& s, Heavy req, clock_t_::time_point deadline, std::chrono::milliseconds hold)
    {
        auto g = co_await s.m.lock();
        CHECK(g.owns_lock());
        if (clock_t_::now() > deadline)
        {
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            co_return false;
        }
        s.ran.fetch_add(1, std::memory_order_relaxed);
        if (hold.count() > 0)
            co_await system::this_coroutine::sleep_for(hold);
        CHECK(req.b.size() == 64);
        co_return true;
    }

    task::Awaitable<bool> create_impl_val(Storm& s, int i, clock_t_::time_point deadline, std::chrono::milliseconds hold)
    {
        Heavy req = co_await new_table(s);
        req.a += std::to_string(i);
        CHECK(req.b.size() == 64 && req.a.size() > 100);
        Heavy recip = co_await new_table(s);
        recip.b.push_back(i);
        const bool ok = co_await run_hook_val(s, std::move(req), deadline, hold);
        if (!ok)
            system::co_spawn(journal(s));
        co_return ok;
    }

    task::Awaitable<void> rpc_val(Storm& s, int i, clock_t_::time_point deadline, std::chrono::milliseconds hold)
    {
        (void)co_await create_impl_val(s, i, deadline, hold);
        s.done.fetch_add(1, std::memory_order_release);
    }

    void storm_value_returning_lock_helper(std::size_t quantum, std::size_t transfer_depth)
    {
        const int rounds = 120 * scale();
        const auto saved_q = settings::loop_task_quantum;
        const auto saved_d = settings::max_transfer_stack_depth;
        settings::loop_task_quantum = quantum;
        settings::max_transfer_stack_depth = transfer_depth;
        {
            usub::Uvent rt(1);
            Storm s;
            auto driver = [&]() -> task::Awaitable<void> {
                for (int r = 0; r < rounds; ++r)
                {
                    const int target = s.done.load(std::memory_order_acquire) + kWaiters + 1;
                    const auto now = clock_t_::now();
                    system::co_spawn(rpc_val(s, 0, now + 10s, 8ms));
                    for (int i = 1; i <= kWaiters; ++i)
                        system::co_spawn(rpc_val(s, i, now + 2ms, 0ms));
                    while (s.done.load(std::memory_order_acquire) < target)
                        co_await system::this_coroutine::sleep_for(1ms);
                }
                for (int i = 0; i < 2000 && s.journaled.load(std::memory_order_acquire) < rounds * kWaiters; ++i)
                    co_await system::this_coroutine::sleep_for(1ms);
                rt.stop();
            };
            system::co_spawn(driver());
            rt.run();
            CHECK_EQ(s.done.load(), rounds * (kWaiters + 1));
            CHECK_EQ(s.ran.load(), rounds);
            CHECK_EQ(s.dropped.load(), rounds * kWaiters);
        }
        settings::loop_task_quantum = saved_q;
        settings::max_transfer_stack_depth = saved_d;
    }

    void handoff_storm_value_helper() { storm_value_returning_lock_helper(settings::loop_task_quantum, settings::max_transfer_stack_depth); }
    void handoff_storm_value_helper_deferred() { storm_value_returning_lock_helper(settings::loop_task_quantum, 0); }
    void handoff_storm_value_helper_quantum1() { storm_value_returning_lock_helper(1, settings::max_transfer_stack_depth); }
    void handoff_storm_value_helper_q1_deferred() { storm_value_returning_lock_helper(1, 0); }

#ifdef UVENT_ENABLE_REUSEADDR
    inline int tarpit_port() { return 24701 + static_cast<int>(::getpid() % 400) * 2; }
    inline int refused_port() { return tarpit_port() + 1; } // nobody listens there

    struct Tarpit
    {
        int listener{-1};
        int held[2]{-1, -1};
        Tarpit()
        {
            listener = ::socket(AF_INET, SOCK_STREAM, 0);
            CHECK(listener >= 0);
            int one = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(static_cast<uint16_t>(tarpit_port()));
            CHECK(::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) == 1);
            CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
            CHECK(::listen(listener, 1) == 0);
            for (int& fd : held)
            {
                fd = ::socket(AF_INET, SOCK_STREAM, 0);
                CHECK(fd >= 0);
                CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
            }
        }
        ~Tarpit()
        {
            for (int fd : held)
                ::close(fd);
            ::close(listener);
        }
    };

    task::Awaitable<bool> run_hook_socket(Storm& s, std::string request, clock_t_::time_point deadline,
                                          std::chrono::milliseconds hold)
    {
        auto g = co_await s.m.lock();
        CHECK(g.owns_lock());
        if (clock_t_::now() > deadline)
        {
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            co_return false;
        }
        s.ran.fetch_add(1, std::memory_order_relaxed);
        if (hold.count() > 0)
        {
            net::TCPClientSocket c;
            const auto err = co_await c.async_connect("127.0.0.1", std::to_string(tarpit_port()), hold);
            CHECK(err.has_value());
        }
        CHECK(request.size() > 0);
        co_return true;
    }

    task::Awaitable<void> journal_socket(Storm& s)
    {
        net::TCPClientSocket c;
        (void)co_await c.async_connect("127.0.0.1", std::to_string(refused_port()), 500ms);
        s.journaled.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> rpc_socket(Storm& s, int i, clock_t_::time_point deadline, std::chrono::milliseconds hold)
    {
        std::vector<char> body(4096, static_cast<char>('a' + i % 26));
        std::string request(body.begin(), body.end());
        const bool ok = co_await run_hook_socket(s, request + "|hook", deadline, hold);
        if (!ok)
            system::co_spawn(journal_socket(s));
        s.done.fetch_add(1, std::memory_order_release);
    }

    void handoff_storm_socket_hold()
    {
        const int rounds = 60 * scale();
        Tarpit tarpit;
        usub::Uvent rt(1);
        Storm s;
        auto driver = [&]() -> task::Awaitable<void> {
            for (int r = 0; r < rounds; ++r)
            {
                const int target = s.done.load(std::memory_order_acquire) + kWaiters + 1;
                const auto now = clock_t_::now();
                system::co_spawn(rpc_socket(s, 0, now + 10s, 30ms));
                for (int i = 1; i <= kWaiters; ++i)
                    system::co_spawn(rpc_socket(s, i, now + 5ms, 0ms));
                while (s.done.load(std::memory_order_acquire) < target)
                    co_await system::this_coroutine::sleep_for(1ms);
            }
            for (int i = 0; i < 2000 && s.journaled.load(std::memory_order_acquire) < rounds * kWaiters; ++i)
                co_await system::this_coroutine::sleep_for(1ms);
            rt.stop();
        };
        system::co_spawn(driver());
        rt.run();
        CHECK_EQ(s.done.load(), rounds * (kWaiters + 1));
        CHECK_EQ(s.ran.load(), rounds);
        CHECK_EQ(s.dropped.load(), rounds * kWaiters);
        CHECK_EQ(s.journaled.load(), rounds * kWaiters);
    }

    void handoff_storm_socket_foreign()
    {
        const int rounds = 60 * scale();
        Tarpit tarpit;
        usub::Uvent rt(1);
        Storm s;
        std::thread producer([&] {
            for (int r = 0; r < rounds; ++r)
            {
                const int target = s.done.load(std::memory_order_acquire) + kWaiters + 1;
                const auto now = clock_t_::now();
                system::co_spawn(rpc_socket(s, 0, now + 10s, 30ms));
                std::this_thread::sleep_for(2ms); // holder is parked on the socket now
                for (int i = 1; i <= kWaiters; ++i)
                    system::co_spawn(rpc_socket(s, i, now + 5ms, 0ms));
                while (s.done.load(std::memory_order_acquire) < target)
                    std::this_thread::sleep_for(1ms);
            }
            for (int i = 0; i < 2000 && s.journaled.load(std::memory_order_acquire) < rounds * kWaiters; ++i)
                std::this_thread::sleep_for(1ms);
            system::co_spawn(stop_runtime(rt));
        });
        rt.run();
        producer.join();
        CHECK_EQ(s.done.load(), rounds * (kWaiters + 1));
        CHECK_EQ(s.dropped.load(), rounds * kWaiters);
        CHECK_EQ(s.journaled.load(), rounds * kWaiters);
    }
#endif
} // namespace

int main()
{
    return run_tests({
        {"handoff_storm_single_worker", handoff_storm_single_worker},
        {"handoff_storm_foreign_spawn", handoff_storm_foreign_spawn},
        {"handoff_storm_mixed_deadlines", handoff_storm_mixed_deadlines},
        {"handoff_storm_value_helper", handoff_storm_value_helper},
        {"handoff_storm_value_helper_deferred", handoff_storm_value_helper_deferred},
        {"handoff_storm_value_helper_quantum1", handoff_storm_value_helper_quantum1},
        {"handoff_storm_value_helper_q1_deferred", handoff_storm_value_helper_q1_deferred},
#ifdef UVENT_ENABLE_REUSEADDR
        {"handoff_storm_socket_hold", handoff_storm_socket_hold},
        {"handoff_storm_socket_foreign", handoff_storm_socket_foreign},
#endif
    });
}
