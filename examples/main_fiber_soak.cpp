// Fiber soak / smoke: runs several fiber-heavy scenarios concurrently on a
// multi-worker runtime for a fixed wall-clock time and checks invariants
// (every fiber stack unwound, mutex counts exact, echo payloads intact).
//
// Env:
//   UVENT_SOAK_SECONDS         wall-clock budget (default 60)
//   UVENT_SOAK_WORKERS         runtime threads   (default 8)
//   UVENT_SOAK_FIBERS          fibers per mix round per driver (default 256)
//   UVENT_SOAK_PORT            first loopback port, one per worker (default 24500, keep it below net.ipv4.ip_local_port_range so client ephemeral ports never collide with the listeners)
//   UVENT_SOAK_SHUTDOWN_PARKED=1  leave fibers parked on a 1h sleep and stop the
//                              runtime (exercises teardown with suspended fibers)
//
// Exit code 0 = all invariants held.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "uvent/Uvent.h"
#include "uvent/sync/AsyncMutex.h"
#include "uvent/sync/AsyncUnboundedChannel.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

#define SOAK_CHECK(cond)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "SOAK CHECK failed: %s at %s:%d\n", #cond, __FILE__, __LINE__);                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{
    struct Cfg
    {
        int seconds = 60;
        int workers = 8;
        int fibers = 256;
        int port = 24500;
        bool shutdown_parked = false;
        bool only_cancel = false; // UVENT_SOAK_ONLY=cancel
        bool cancel_coro = false; // UVENT_SOAK_CANCEL_CORO=1: coroutine bodies instead of fibers
    } cfg;

    long env_long(const char* name, long def)
    {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : def;
    }

    std::atomic<bool> g_deadline{false};

    struct Stats
    {
        std::atomic<long> guards_created{0}, guards_destroyed{0};
        std::atomic<long> mix_rounds{0}, mix_fibers{0}, mix_awaits{0}, mutex_incs{0}, cross_joins{0},
            caught_inside{0}, body_throws{0}, chan_items{0};
        std::atomic<long> cancel_rounds{0}, cancelled{0};
        std::atomic<long> echo_conns{0}, echo_bytes{0}, echo_errors{0}, echo_abandoned{0}, err_connect{0}, err_write{0}, err_read{0};
        std::atomic<long> stack_rounds{0}, stack_fibers{0};
    } S;

    struct Guard
    {
        Guard() { S.guards_created.fetch_add(1, std::memory_order_relaxed); }
        ~Guard() { S.guards_destroyed.fetch_add(1, std::memory_order_relaxed); }
    };

    inline uint32_t xs(uint32_t& s)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    // ------------------------------------------------------------------ mix
    task::Awaitable<int> coro_hop(int v)
    {
        co_await system::this_coroutine::sleep_for(std::chrono::microseconds(200));
        co_return v;
    }

    struct MixRound
    {
        sync::AsyncMutex mtx;
        long shared = 0;
        std::atomic<long> incs{0};
        sync::AsyncUnboundedChannel<uint64_t> ch;
    };

    void mix_fiber_body(MixRound* r, uint32_t seed, int tid)
    {
        Guard g;
        uint32_t s = seed | 1u;
        long my_incs = 0, my_sent = 0, my_recv = 0;
        const int steps = 32 + static_cast<int>(xs(s) % 64);
        for (int k = 0; k < steps; ++k)
        {
            switch (xs(s) % 8)
            {
            case 0:
                fiber::await(system::this_coroutine::sleep_for(std::chrono::microseconds(xs(s) % 1500)));
                break;
            case 1:
                fiber::yield();
                break;
            case 2:
            {
                auto lk = fiber::await(r->mtx.lock());
                SOAK_CHECK(lk.owns_lock());
                const long v = r->shared;
                if (xs(s) & 1)
                    fiber::yield();
                else
                    fiber::await(system::this_coroutine::sleep_for(std::chrono::microseconds(50)));
                r->shared = v + 1;
                ++my_incs;
                break;
            }
            case 3:
                r->ch.try_send(static_cast<uint64_t>(k));
                ++my_sent;
                break;
            case 4:
                // Each fiber receives at most what it sent, so the channel can never
                // starve a pending receiver (queue length >= pending receivers).
                if (my_recv < my_sent)
                {
                    auto v = fiber::await(r->ch.recv());
                    SOAK_CHECK(v.has_value());
                    ++my_recv;
                    S.chan_items.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            case 5:
            {
                const int nested = fiber::await(fiber::run([&s] {
                    fiber::yield();
                    return fiber::await(fiber::run([] { return 7; })) + static_cast<int>(xs(s) % 3);
                }));
                SOAK_CHECK(nested >= 7 && nested <= 9);
                break;
            }
            case 6:
            {
                const int other = (tid + 1 + static_cast<int>(xs(s) % (cfg.workers > 1 ? cfg.workers - 1 : 1))) %
                                  cfg.workers;
                auto h = task::spawn(coro_hop(k), other);
                SOAK_CHECK(fiber::await(h) == k);
                S.cross_joins.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            case 7:
                try
                {
                    fiber::await(system::this_coroutine::sleep_for(0ms));
                    throw std::runtime_error("inside");
                }
                catch (const std::runtime_error&)
                {
                    S.caught_inside.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            S.mix_awaits.fetch_add(1, std::memory_order_relaxed);
        }
        r->incs.fetch_add(my_incs, std::memory_order_relaxed);
        S.mutex_incs.fetch_add(my_incs, std::memory_order_relaxed);
        S.mix_fibers.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> mix_driver(int base)
    {
        uint32_t seed = 0x9E3779B9u * static_cast<uint32_t>(base + 1);
        while (!g_deadline.load(std::memory_order_relaxed))
        {
            auto r = std::make_unique<MixRound>();
            {
                task::TaskScope scope;
                for (int i = 0; i < cfg.fibers; ++i)
                {
                    const int tid = (base + i) % cfg.workers;
                    scope.spawn(fiber::run([r = r.get(), sd = xs(seed), tid] { mix_fiber_body(r, sd, tid); }), tid);
                }
                // a few fibers whose body throws out through the host task
                std::vector<task::JoinHandle<void>> throwers;
                for (int i = 0; i < 4; ++i)
                    throwers.push_back(task::spawn(fiber::run([] {
                                                       Guard g;
                                                       fiber::yield();
                                                       throw std::logic_error("body");
                                                   }),
                                                   (base + i) % cfg.workers));
                for (auto& h : throwers)
                {
                    try
                    {
                        co_await h;
                        SOAK_CHECK(false);
                    }
                    catch (const std::logic_error&)
                    {
                        S.body_throws.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                co_await scope.join();
            }
            SOAK_CHECK(r->shared == r->incs.load());
            S.mix_rounds.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // --------------------------------------------------------------- cancel
    task::Awaitable<void> cancel_poll_coro()
    {
        Guard g;
        while (!system::this_coroutine::cancel_requested())
            if (!co_await system::this_coroutine::sleep_for(1ms))
                break;
        S.cancelled.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> cancel_driver(int base)
    {
        uint32_t seed = 0xC0FFEEu * static_cast<uint32_t>(base + 1);
        while (!g_deadline.load(std::memory_order_relaxed))
        {
            task::TaskScope scope;
            const int n = 64;
            for (int i = 0; i < n; ++i)
            {
                const int kind = i % 3;
                if (cfg.cancel_coro)
                {
                    scope.spawn(cancel_poll_coro(), (base + i) % cfg.workers);
                    continue;
                }
                scope.spawn(fiber::run([kind] {
                                Guard g;
#ifdef UVENT_ENABLE_REUSEADDR
                                if (kind == 0)
                                {
                                    // parked on a long timer; proactive wake-up on cancel
                                    const bool completed = fiber::await(system::this_coroutine::sleep_for(10s));
                                    SOAK_CHECK(!completed);
                                    SOAK_CHECK(system::this_coroutine::cancel_requested());
                                    S.cancelled.fetch_add(1, std::memory_order_relaxed);
                                    return;
                                }
#endif
                                if (kind == 1)
                                {
                                    // parked inside a nested fiber, cancel must unwind both
                                    fiber::await(fiber::run([] {
                                        Guard inner;
                                        while (!system::this_coroutine::cancel_requested())
                                            if (!fiber::await(system::this_coroutine::sleep_for(1ms)))
                                                break;
                                    }));
                                    S.cancelled.fetch_add(1, std::memory_order_relaxed);
                                    return;
                                }
                                // polling style
                                while (!system::this_coroutine::cancel_requested())
                                    if (!fiber::await(system::this_coroutine::sleep_for(1ms)))
                                        break;
                                S.cancelled.fetch_add(1, std::memory_order_relaxed);
                            }),
                            (base + i) % cfg.workers);
            }
            co_await system::this_coroutine::sleep_for(std::chrono::milliseconds(1 + xs(seed) % 10));
            co_await scope.cancel_and_join();
            S.cancel_rounds.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ----------------------------------------------------------------- echo
    std::atomic<bool> g_echo_stop{false};

    void echo_handler(std::shared_ptr<net::TCPClientSocket> sock)
    {
        Guard g;
        uint8_t buf[4096];
        for (;;)
        {
            const auto r = fiber::await(sock->async_read(buf, sizeof(buf)));
            if (r <= 0)
                break;
            std::size_t off = 0;
            while (off < static_cast<std::size_t>(r))
            {
                const auto w = fiber::await(sock->async_write(buf + off, static_cast<std::size_t>(r) - off));
                if (w <= 0)
                    return;
                off += static_cast<std::size_t>(w);
            }
        }
    }

    // One acceptor per worker on port+tid; handlers stay on the acceptor's worker.
    task::Awaitable<void> acceptor_driver(int tid)
    {
        co_await fiber::run([tid] {
            Guard g;
            net::TCPServerSocket acc{"127.0.0.1", static_cast<uint16_t>(cfg.port + tid)};
            task::TaskScope handlers;
            while (!g_echo_stop.load(std::memory_order_acquire))
            {
                auto c = fiber::await(acc.async_accept());
                if (!c)
                    continue;
                auto sp = std::make_shared<net::TCPClientSocket>(std::move(*c));
                handlers.spawn(fiber::run([sp] { echo_handler(sp); }));
            }
            fiber::await(handlers.join());
        });
    }

    void echo_client_body(int port, uint32_t seed)
    {
        Guard g;
        uint32_t s = seed | 1u;
        net::TCPClientSocket sock;
        auto err = fiber::await(sock.async_connect("127.0.0.1", std::to_string(port), 30000ms));
        if (err.has_value())
        {
            S.err_connect.fetch_add(1, std::memory_order_relaxed);
            S.echo_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<uint8_t> out, in;
        const int msgs = 1 + static_cast<int>(xs(s) % 6);
        for (int m = 0; m < msgs; ++m)
        {
            const std::size_t len = 1 + xs(s) % 3000;
            out.resize(len);
            for (std::size_t i = 0; i < len; ++i)
                out[i] = static_cast<uint8_t>(xs(s));
            std::size_t off = 0;
            while (off < len)
            {
                const auto w = fiber::await(sock.async_write(out.data() + off, len - off));
                if (w <= 0)
                {
                    S.err_write.fetch_add(1, std::memory_order_relaxed);
                    S.echo_errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                off += static_cast<std::size_t>(w);
            }
            if (xs(s) % 16 == 0)
            {
                // abandon mid-stream: handler must see EOF / error and unwind
                S.echo_abandoned.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            in.assign(len, 0);
            std::size_t got = 0;
            while (got < len)
            {
                const auto r = fiber::await(sock.async_read(in.data() + got, len - got));
                if (r <= 0)
                {
                    S.err_read.fetch_add(1, std::memory_order_relaxed);
                    S.echo_errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                got += static_cast<std::size_t>(r);
            }
            SOAK_CHECK(std::memcmp(in.data(), out.data(), len) == 0);
            S.echo_bytes.fetch_add(static_cast<long>(len), std::memory_order_relaxed);
        }
        S.echo_conns.fetch_add(1, std::memory_order_relaxed);
    }

    task::Awaitable<void> echo_client_driver(int tid)
    {
        uint32_t seed = 0xBADC0DEu * static_cast<uint32_t>(tid + 1);
        const int target = cfg.port + (tid + 1) % cfg.workers; // talk to the neighbour's acceptor
        while (!g_deadline.load(std::memory_order_relaxed))
        {
            task::TaskScope scope;
            for (int c = 0; c < 8; ++c)
                scope.spawn(fiber::run([target, sd = xs(seed)] { echo_client_body(target, sd); }));
            co_await scope.join();
        }
    }

    // ---------------------------------------------------------------- stack
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

    task::Awaitable<void> stack_driver(int base)
    {
        static constexpr std::size_t sizes[] = {64 * 1024, 128 * 1024, 256 * 1024, 1024 * 1024};
        while (!g_deadline.load(std::memory_order_relaxed))
        {
            task::TaskScope scope;
            std::vector<task::JoinHandle<int>> hs;
            std::vector<int> depths;
            for (int i = 0; i < 32; ++i)
            {
                const std::size_t sz = sizes[i % 4];
                const int depth = static_cast<int>(sz / 1024); // ~1/3 of the stack with 256B pads
                depths.push_back(depth);
                hs.push_back(scope.spawn(fiber::run([depth] {
                                                        Guard g;
                                                        return deep(depth);
                                                    },
                                                    fiber::Options{sz}),
                                         (base + i) % cfg.workers));
            }
            for (std::size_t i = 0; i < hs.size(); ++i)
                SOAK_CHECK(co_await hs[i] == depths[i]);
            co_await scope.join();
            S.stack_fibers.fetch_add(static_cast<long>(hs.size()), std::memory_order_relaxed);
            S.stack_rounds.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ------------------------------------------------------------- reporting
    void report(const char* tag)
    {
        std::printf("[%s] mix: rounds=%ld fibers=%ld awaits=%ld mutex=%ld cross=%ld caught=%ld throws=%ld chan=%ld | "
                    "cancel: rounds=%ld cancelled=%ld | echo: conns=%ld bytes=%ld abandoned=%ld errors=%ld (c=%ld w=%ld r=%ld) | "
                    "stack: rounds=%ld fibers=%ld | guards %ld/%ld\n",
                    tag, S.mix_rounds.load(), S.mix_fibers.load(), S.mix_awaits.load(), S.mutex_incs.load(),
                    S.cross_joins.load(), S.caught_inside.load(), S.body_throws.load(), S.chan_items.load(),
                    S.cancel_rounds.load(), S.cancelled.load(), S.echo_conns.load(), S.echo_bytes.load(),
                    S.echo_abandoned.load(), S.echo_errors.load(), S.err_connect.load(), S.err_write.load(), S.err_read.load(), S.stack_rounds.load(), S.stack_fibers.load(),
                    S.guards_destroyed.load(), S.guards_created.load());
    }

    task::Awaitable<void> supervisor(usub::Uvent* rt)
    {
        task::TaskScope acceptors;
        task::TaskScope drivers;
        for (int t = 0; t < cfg.workers; ++t)
        {
            if (cfg.only_cancel)
            {
                drivers.spawn(cancel_driver(t), t);
                continue;
            }
            acceptors.spawn(acceptor_driver(t), t);
            drivers.spawn(echo_client_driver(t), t);
            drivers.spawn(mix_driver(t), t);
            if (t % 2 == 0)
                drivers.spawn(cancel_driver(t), t);
            if (t % 4 == 0)
                drivers.spawn(stack_driver(t), t);
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto end = t0 + std::chrono::seconds(cfg.seconds);
        int tick = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            co_await system::this_coroutine::sleep_for(1s);
            if (++tick % 5 == 0)
                report("tick");
        }
        g_deadline.store(true, std::memory_order_relaxed);
        co_await drivers.join(); // clients finish their last round first
        report("drivers-joined");
        if (cfg.only_cancel)
        {
            SOAK_CHECK(S.guards_created.load() == S.guards_destroyed.load());
            std::printf("OK (cancel-only): rounds=%ld cancelled=%ld\n", S.cancel_rounds.load(), S.cancelled.load());
            rt->stop();
            co_return;
        }
        // acceptors are parked in async_accept: poke each one so it re-checks the flag
        g_echo_stop.store(true, std::memory_order_release);
        for (int t = 0; t < cfg.workers; ++t)
        {
            net::TCPClientSocket poke;
            co_await poke.async_connect("127.0.0.1", std::to_string(cfg.port + t), 3000ms);
        }
        co_await acceptors.join();
        report("final");
        SOAK_CHECK(S.guards_created.load() == S.guards_destroyed.load());
        SOAK_CHECK(S.mix_rounds.load() > 0 && S.cancel_rounds.load() > 0 && S.echo_conns.load() > 0 &&
                   S.stack_rounds.load() > 0);
        // connect() may fail under heavy load (listen backlog / ephemeral ports); data errors never may
        SOAK_CHECK(S.err_write.load() == 0 && S.err_read.load() == 0);
        std::printf("OK: all fiber stacks unwound, invariants held (%d s, %d workers)\n", cfg.seconds, cfg.workers);

        if (cfg.shutdown_parked)
        {
            for (int i = 0; i < 16; ++i)
                task::spawn(fiber::run([] {
                                Guard g;
                                fiber::await(system::this_coroutine::sleep_for(1h));
                            }),
                            i % cfg.workers);
            co_await system::this_coroutine::sleep_for(20ms);
            std::printf("stopping runtime with 16 fibers parked on a 1h sleep\n");
        }
        rt->stop();
    }
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    cfg.seconds = static_cast<int>(env_long("UVENT_SOAK_SECONDS", cfg.seconds));
    cfg.workers = static_cast<int>(env_long("UVENT_SOAK_WORKERS", cfg.workers));
    cfg.fibers = static_cast<int>(env_long("UVENT_SOAK_FIBERS", cfg.fibers));
    cfg.port = static_cast<int>(env_long("UVENT_SOAK_PORT", cfg.port));
    cfg.shutdown_parked = env_long("UVENT_SOAK_SHUTDOWN_PARKED", 0) != 0;
    if (const char* o = std::getenv("UVENT_SOAK_ONLY"))
        cfg.only_cancel = std::strcmp(o, "cancel") == 0;
    cfg.cancel_coro = env_long("UVENT_SOAK_CANCEL_CORO", 0) != 0;
    std::printf("fiber soak: %d s, %d workers, %d fibers/round, ports %d..%d%s\n", cfg.seconds, cfg.workers,
                cfg.fibers, cfg.port, cfg.port + cfg.workers - 1, cfg.shutdown_parked ? ", shutdown-parked" : "");
    {
        usub::Uvent rt(cfg.workers);
        system::co_spawn_static(supervisor(&rt), 0);
        rt.run();
    }
    if (cfg.shutdown_parked)
        std::printf("after runtime teardown: guards destroyed %ld of %ld, drain survivors %zu\n",
                    S.guards_destroyed.load(), S.guards_created.load(), usub::Uvent::drain_survivors());
    return 0;
}
