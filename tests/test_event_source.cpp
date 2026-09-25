// poll::EventSource — a non-socket descriptor watched by a worker's poller, delivered through a plain
// callback on that worker. Covers: level-triggered eventfd delivery from a foreign thread, HUP on a pipe
// whose writer closes, edge- vs level-triggered semantics when the callback leaves data unread, and
// removeSource() stopping delivery. POSIX only (eventfd / pipe); on kqueue hosts eventfd is a pipe.
#include <atomic>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/poll/EventSource.h"
#include "uvent/sync/AsyncEvent.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    struct Probe
    {
        std::atomic<uint64_t> hits{0};
        std::atomic<uint64_t> sum{0};
        std::atomic<uint32_t> last_ready{0};
        std::atomic<int> tid{-1};
        bool consume{true};
    };

    // eventfd on Linux, a non-blocking pipe elsewhere; returns {read_fd, write_fd}
    std::pair<int, int> make_counter_fd()
    {
#ifdef __linux__
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        CHECK(fd >= 0);
        return {fd, fd};
#else
        int p[2];
        CHECK(::pipe(p) == 0);
        ::fcntl(p[0], F_SETFL, O_NONBLOCK);
        return {p[0], p[1]};
#endif
    }

    void write_counter(int wfd, uint64_t v)
    {
#ifdef __linux__
        CHECK(::write(wfd, &v, sizeof v) == static_cast<ssize_t>(sizeof v));
#else
        for (uint64_t i = 0; i < v; ++i)
        {
            const char c = 1;
            CHECK(::write(wfd, &c, 1) == 1);
        }
#endif
    }

    void on_ready(core::EventSource* self, uint32_t ready) noexcept
    {
        auto* p = static_cast<Probe*>(self->user);
        p->hits.fetch_add(1, std::memory_order_relaxed);
        p->last_ready.store(ready, std::memory_order_relaxed);
        p->tid.store(system::this_thread::detail::t_id, std::memory_order_relaxed);
        if (!p->consume)
            return;
        for (;;)
        {
#ifdef __linux__
            uint64_t v = 0;
            const ssize_t r = ::read(self->fd, &v, sizeof v);
            if (r == static_cast<ssize_t>(sizeof v))
                p->sum.fetch_add(v, std::memory_order_relaxed);
#else
            char buf[64];
            const ssize_t r = ::read(self->fd, buf, sizeof buf);
            if (r > 0)
                p->sum.fetch_add(static_cast<uint64_t>(r), std::memory_order_relaxed);
#endif
            if (r <= 0)
                break;
        }
    }

    task::Awaitable<void> wait_until(std::atomic<uint64_t>& v, uint64_t want, int max_ms)
    {
        for (int i = 0; i < max_ms && v.load(std::memory_order_relaxed) < want; ++i)
            co_await system::this_coroutine::sleep_for(1ms);
    }

    // ------------------------------------------------------------ level-triggered delivery

    task::Awaitable<void> level_body(usub::Uvent* rt)
    {
        auto [rfd, wfd] = make_counter_fd();
        Probe probe;
        core::EventSource src;
        src.fd = rfd;
        src.on_ready = &on_ready;
        src.user = &probe;
        system::this_thread::detail::pl.addSource(&src, core::READ);

        std::thread producer(
            [wfd]
            {
                for (int i = 1; i <= 5; ++i)
                {
                    std::this_thread::sleep_for(2ms);
                    write_counter(wfd, static_cast<uint64_t>(i)); // 1+2+3+4+5 = 15
                }
            });
        co_await wait_until(probe.sum, 15, 2000);
        producer.join();
        CHECK_EQ(probe.sum.load(), 15u);
        CHECK(probe.hits.load() >= 1 && probe.hits.load() <= 5); // deliveries may coalesce, never multiply
        CHECK_EQ(probe.tid.load(), 0);                           // callback ran on the registering worker
        CHECK(probe.last_ready.load() & core::EventSource::READABLE);

        system::this_thread::detail::pl.removeSource(&src);
        co_await system::this_coroutine::sleep_for(5ms);
        const auto hits_after_remove = probe.hits.load();
        write_counter(wfd, 1);
        co_await system::this_coroutine::sleep_for(20ms);
        CHECK_EQ(probe.hits.load(), hits_after_remove); // removed source is silent
        ::close(rfd);
        if (wfd != rfd)
            ::close(wfd);
        rt->stop();
    }

    void level_triggered_eventfd_from_foreign_thread()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(level_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ HUP on a pipe

    task::Awaitable<void> hup_body(usub::Uvent* rt)
    {
        int p[2];
        CHECK(::pipe(p) == 0);
        ::fcntl(p[0], F_SETFL, O_NONBLOCK);
        Probe probe;
        core::EventSource src;
        src.fd = p[0];
        src.on_ready = &on_ready;
        src.user = &probe;
        system::this_thread::detail::pl.addSource(&src, core::READ);

        ::close(p[1]); // writer gone -> HUP on the reader
        for (int i = 0; i < 500 && !(probe.last_ready.load() & core::EventSource::HUP); ++i)
            co_await system::this_coroutine::sleep_for(1ms);
        CHECK(probe.last_ready.load() & core::EventSource::HUP);
        system::this_thread::detail::pl.removeSource(&src);
        ::close(p[0]);
        rt->stop();
    }

    void hup_when_pipe_writer_closes()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(hup_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ edge vs level when data is left unread

    task::Awaitable<void> trigger_body(usub::Uvent* rt, bool edge)
    {
        auto [rfd, wfd] = make_counter_fd();
        Probe probe;
        probe.consume = false; // leave the data in the fd
        core::EventSource src;
        src.fd = rfd;
        src.on_ready = &on_ready;
        src.user = &probe;
        src.edge = edge;
        system::this_thread::detail::pl.addSource(&src, core::READ);

        write_counter(wfd, 1);
        co_await system::this_coroutine::sleep_for(30ms);
        const auto hits = probe.hits.load();
        if (edge)
            CHECK_EQ(hits, 1u); // one edge, no matter how many polls
        else
        {
#ifdef UVENT_ENABLE_IO_URING
            CHECK(hits >= 1); // multishot POLL_ADD fires per wake-up: unread data is not re-reported
#else
            CHECK(hits >= 2); // level: reported on every poll while unread
#endif
        }
        system::this_thread::detail::pl.removeSource(&src);
        ::close(rfd);
        if (wfd != rfd)
            ::close(wfd);
        rt->stop();
    }

    void edge_triggered_reports_once()
    {
#ifdef UVENT_ENABLE_IO_URING
        return; // POLL_ADD is level-only; `edge` is documented as ignored there
#endif
        usub::Uvent rt(1);
        system::co_spawn_static(trigger_body(&rt, true), 0);
        rt.run();
    }

    void level_triggered_reports_until_consumed()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(trigger_body(&rt, false), 0);
        rt.run();
    }

    // ------------------------------------------------------------ callback wakes a parked coroutine

    struct WakeProbe
    {
        sync::AsyncEvent ev{sync::Reset::Manual};
        int fd{-1};
    };

    void on_ready_wake(core::EventSource* self, uint32_t) noexcept
    {
        auto* p = static_cast<WakeProbe*>(self->user);
        uint64_t v;
        while (::read(self->fd, &v, sizeof v) > 0)
        {
        }
        p->ev.set(); // resume_waiter under the hood: the parked coroutine continues on its worker
    }

    task::Awaitable<void> wake_body(usub::Uvent* rt)
    {
        auto [rfd, wfd] = make_counter_fd();
        WakeProbe probe;
        core::EventSource src;
        src.fd = rfd;
        src.on_ready = &on_ready_wake;
        src.user = &probe;
        system::this_thread::detail::pl.addSource(&src, core::READ);

        std::thread producer(
            [wfd]
            {
                std::this_thread::sleep_for(10ms);
                write_counter(wfd, 1);
            });
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = co_await probe.ev.wait();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        producer.join();
        CHECK(ok);
        CHECK(ms < 1000);
        system::this_thread::detail::pl.removeSource(&src);
        ::close(rfd);
        if (wfd != rfd)
            ::close(wfd);
        rt->stop();
    }

    void callback_wakes_parked_coroutine()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(wake_body(&rt), 0);
        rt.run();
    }
} // namespace

int main()
{
    return run_tests({
        {"level_triggered_eventfd_from_foreign_thread", level_triggered_eventfd_from_foreign_thread},
        {"hup_when_pipe_writer_closes", hup_when_pipe_writer_closes},
        {"edge_triggered_reports_once", edge_triggered_reports_once},
        {"level_triggered_reports_until_consumed", level_triggered_reports_until_consumed},
        {"callback_wakes_parked_coroutine", callback_wakes_parked_coroutine},
    });
}
