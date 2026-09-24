#include <memory>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo lives in kernel32 on Win7+, no psapi.lib needed
#include <windows.h>     // before psapi.h: MSVC's psapi.h does not pull the base types itself
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

// GCC defines __SANITIZE_*__, clang only exposes __has_feature().
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define UVENT_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define UVENT_TEST_SANITIZED 1
#endif
#endif

#include "test_common.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"

using usub::queue::concurrent::SegmentedMPMCQueue;

namespace
{
    struct Item
    {
        uint32_t producer;
        uint64_t seq;
    };

    std::atomic<long> g_live{0};

    struct Tracked
    {
        std::unique_ptr<uint64_t> v;
        Tracked() : v(nullptr) { g_live.fetch_add(1); }
        explicit Tracked(uint64_t x) : v(std::make_unique<uint64_t>(x)) { g_live.fetch_add(1); }
        Tracked(Tracked&& o) noexcept : v(std::move(o.v)) { g_live.fetch_add(1); }
        Tracked& operator=(Tracked&& o) noexcept
        {
            v = std::move(o.v);
            return *this;
        }
        ~Tracked() { g_live.fetch_sub(1); }
    };

    void fifo_single_thread()
    {
        SegmentedMPMCQueue<uint64_t, 64> q;
        CHECK(q.empty());
        for (uint64_t i = 0; i < 100000; ++i)
            q.enqueue(i);
        CHECK_EQ(q.size(), 100000u);
        uint64_t v;
        for (uint64_t i = 0; i < 100000; ++i)
        {
            CHECK(q.try_dequeue(v));
            CHECK_EQ(v, i);
        }
        CHECK(!q.try_dequeue(v));
        CHECK(q.empty());
    }

    void interleaved_fifo_across_segments()
    {
        SegmentedMPMCQueue<uint64_t, 16> q;
        uint64_t next_in = 0, next_out = 0, v;
        for (int round = 0; round < 2000; ++round)
        {
            for (int i = 0; i < 7; ++i)
                q.enqueue(next_in++);
            for (int i = 0; i < 5; ++i)
            {
                CHECK(q.try_dequeue(v));
                CHECK_EQ(v, next_out++);
            }
        }
        while (q.try_dequeue(v))
            CHECK_EQ(v, next_out++);
        CHECK_EQ(next_in, next_out);
    }

    void unbounded_growth()
    {
        SegmentedMPMCQueue<uint64_t> q;
        constexpr uint64_t n = 5'000'000;
        for (uint64_t i = 0; i < n; ++i)
            q.enqueue(i);
        CHECK_EQ(q.size(), n);
        uint64_t v, expect = 0;
        while (q.try_dequeue(v))
            CHECK_EQ(v, expect++);
        CHECK_EQ(expect, n);
    }

    void bulk_ops()
    {
        SegmentedMPMCQueue<uint64_t, 100> q;
        std::vector<uint64_t> in(333);
        uint64_t next_in = 0, next_out = 0;
        std::vector<uint64_t> out(150);
        for (int round = 0; round < 500; ++round)
        {
            for (auto& x : in)
                x = next_in++;
            CHECK_EQ(q.enqueue_bulk(in.data(), in.size()), in.size());
            size_t got = 0;
            while (got < 200)
            {
                size_t n = q.try_dequeue_bulk(out.data(), 150);
                CHECK(n > 0);
                for (size_t i = 0; i < n; ++i)
                    CHECK_EQ(out[i], next_out++);
                got += n;
            }
        }
        for (;;)
        {
            size_t n = q.try_dequeue_bulk(out.data(), out.size());
            if (!n)
                break;
            for (size_t i = 0; i < n; ++i)
                CHECK_EQ(out[i], next_out++);
        }
        CHECK_EQ(next_in, next_out);
        CHECK_EQ(q.try_dequeue_bulk(out.data(), 0), 0u);
    }

    void mpmc_stress_impl(unsigned producers, unsigned consumers, uint64_t per_producer, bool bulk)
    {
        SegmentedMPMCQueue<Item, 256> q;
        std::atomic<bool> go{false};
        std::atomic<unsigned> producers_done{0};
        std::atomic<uint64_t> consumed{0};
        std::atomic<uint64_t> sum{0};

        std::vector<std::thread> th;
        for (unsigned p = 0; p < producers; ++p)
            th.emplace_back(
                [&, p]
                {
                    while (!go.load(std::memory_order_acquire))
                        cpu_relax();
                    if (bulk)
                    {
                        Item buf[17];
                        uint64_t s = 0;
                        while (s < per_producer)
                        {
                            size_t n = 0;
                            while (n < 17 && s < per_producer)
                                buf[n++] = Item{p, s++};
                            q.enqueue_bulk(buf, n);
                        }
                    }
                    else
                    {
                        for (uint64_t s = 0; s < per_producer; ++s)
                            q.enqueue(Item{p, s});
                    }
                    producers_done.fetch_add(1, std::memory_order_release);
                });

        for (unsigned c = 0; c < consumers; ++c)
            th.emplace_back(
                [&]
                {
                    std::vector<uint64_t> last(producers, 0);
                    uint64_t local = 0, local_sum = 0;
                    Item buf[32];
                    while (!go.load(std::memory_order_acquire))
                        cpu_relax();
                    for (;;)
                    {
                        size_t n;
                        if (bulk)
                            n = q.try_dequeue_bulk(buf, 32);
                        else
                            n = q.try_dequeue(buf[0]) ? 1 : 0;
                        if (n == 0)
                        {
                            if (producers_done.load(std::memory_order_acquire) == producers && q.empty())
                            {
                                if (bulk)
                                    n = q.try_dequeue_bulk(buf, 32);
                                else
                                    n = q.try_dequeue(buf[0]) ? 1 : 0;
                                if (n == 0)
                                    break;
                            }
                            else
                            {
                                cpu_relax();
                                continue;
                            }
                        }
                        for (size_t i = 0; i < n; ++i)
                        {
                            CHECK(buf[i].producer < producers);
                            CHECK(buf[i].seq + 1 > last[buf[i].producer]);
                            last[buf[i].producer] = buf[i].seq + 1;
                            local_sum += buf[i].seq;
                        }
                        local += n;
                    }
                    consumed.fetch_add(local);
                    sum.fetch_add(local_sum);
                });

        go.store(true, std::memory_order_release);
        for (auto& t : th)
            t.join();

        CHECK_EQ(consumed.load(), uint64_t(producers) * per_producer);
        CHECK_EQ(sum.load(), uint64_t(producers) * (per_producer * (per_producer - 1) / 2));
        CHECK(q.empty());
    }

    void mpmc_stress_1p1c() { mpmc_stress_impl(1, 1, 2'000'000, false); }
    void mpmc_stress_4p4c() { mpmc_stress_impl(4, 4, 1'000'000, false); }
    void mpmc_stress_many()
    {
        unsigned n = std::max(2u, hw_threads() / 2);
        mpmc_stress_impl(n, n, 500'000, false);
    }
    void mpmc_stress_bulk() { mpmc_stress_impl(4, 4, 1'000'000, true); }
    void mpmc_stress_asym()
    {
        mpmc_stress_impl(8, 1, 300'000, false);
        mpmc_stress_impl(1, 8, 2'000'000, false);
    }

    void move_only_no_leak()
    {
        {
            SegmentedMPMCQueue<Tracked, 32> q;
            for (uint64_t i = 0; i < 1000; ++i)
                q.emplace(i);
            Tracked t;
            for (uint64_t i = 0; i < 500; ++i)
            {
                CHECK(q.try_dequeue(t));
                CHECK_EQ(*t.v, i);
            }
        }
        CHECK_EQ(g_live.load(), 0);
    }

    void move_only_mt()
    {
        {
            SegmentedMPMCQueue<Tracked, 64> q;
            std::atomic<bool> stop{false};
            std::atomic<uint64_t> got{0};
            std::vector<std::thread> th;
            for (int p = 0; p < 4; ++p)
                th.emplace_back(
                    [&]
                    {
                        for (uint64_t i = 0; i < 100000; ++i)
                            q.emplace(i);
                    });
            for (int c = 0; c < 4; ++c)
                th.emplace_back(
                    [&]
                    {
                        Tracked t;
                        uint64_t n = 0;
                        while (!stop.load(std::memory_order_acquire))
                            if (q.try_dequeue(t))
                                ++n;
                        while (q.try_dequeue(t))
                            ++n;
                        got.fetch_add(n);
                    });
            for (int p = 0; p < 4; ++p)
                th[p].join();
            stop.store(true, std::memory_order_release);
            for (int c = 4; c < 8; ++c)
                th[c].join();
            CHECK_EQ(got.load(), 400000u);
        }
        CHECK_EQ(g_live.load(), 0);
    }

    long rss_kb()
    {
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        return static_cast<long>(pmc.PeakWorkingSetSize / 1024);
#else
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
        return static_cast<long>(ru.ru_maxrss / 1024); // bytes on macOS
#else
        return static_cast<long>(ru.ru_maxrss); // kB on Linux/BSD
#endif
#endif
    }

    void reclamation_bounded_memory()
    {
        SegmentedMPMCQueue<uint64_t, 1024> q;
        const long before = rss_kb();
        std::vector<std::thread> th;
        const unsigned n = std::max(2u, std::min(8u, hw_threads() / 2));
        for (unsigned i = 0; i < n; ++i)
            th.emplace_back(
                [&]
                {
                    uint64_t v;
                    for (uint64_t k = 0; k < 20'000'000 / n; ++k)
                    {
                        q.enqueue(k);
                        q.enqueue(k);
                        while (!q.try_dequeue(v))
                            cpu_relax();
                        while (!q.try_dequeue(v))
                            cpu_relax();
                    }
                });
        for (auto& t : th)
            t.join();
        uint64_t v;
        CHECK(!q.try_dequeue(v));
        const long after = rss_kb();
        std::printf("   rss before=%ld kB after=%ld kB\n", before, after);
#if !defined(UVENT_TEST_SANITIZED)
        CHECK(after - before < 64 * 1024);
#endif
    }

    void thread_churn()
    {
        SegmentedMPMCQueue<uint64_t, 64> q;
        std::atomic<uint64_t> pushed{0}, popped{0};
        for (int round = 0; round < 200; ++round)
        {
            std::vector<std::thread> th;
            for (int i = 0; i < 6; ++i)
                th.emplace_back(
                    [&, i]
                    {
                        uint64_t v;
                        for (uint64_t k = 0; k < 2000; ++k)
                        {
                            if ((i + k) & 1)
                            {
                                q.enqueue(k);
                                pushed.fetch_add(1);
                            }
                            else if (q.try_dequeue(v))
                                popped.fetch_add(1);
                        }
                    });
            for (auto& t : th)
                t.join();
        }
        uint64_t v;
        while (q.try_dequeue(v))
            popped.fetch_add(1);
        CHECK_EQ(pushed.load(), popped.load());
        std::printf("   hazard records=%zu\n", usub::utils::sync::HazardDomain::instance().records_count());
        CHECK(usub::utils::sync::HazardDomain::instance().records_count() <= 64);
    }

    void destructor_drains_leftovers()
    {
        {
            SegmentedMPMCQueue<Tracked, 8> q;
            for (uint64_t i = 0; i < 100; ++i)
                q.emplace(i);
            Tracked t;
            for (int i = 0; i < 37; ++i)
                CHECK(q.try_dequeue(t));
        }
        CHECK_EQ(g_live.load(), 0);
    }
} // namespace

void legacy_bounded_bulk_no_loss()
{
    using usub::queue::concurrent::MPMCQueue;
    constexpr int kProducers = 4, kConsumers = 4;
    constexpr uint64_t kPerProducer = 200000;
    MPMCQueue<uint64_t> q{256};
    std::atomic<uint64_t> consumed_sum{0}, consumed_cnt{0};
    std::atomic<int> producers_done{0};

    std::vector<std::thread> th;
    for (int p = 0; p < kProducers; ++p)
        th.emplace_back(
            [&, p]
            {
                uint64_t buf[16];
                uint64_t next = 0;
                while (next < kPerProducer)
                {
                    size_t n = std::min<uint64_t>(16, kPerProducer - next);
                    for (size_t i = 0; i < n; ++i)
                        buf[i] = uint64_t(p) * kPerProducer + next + i;
                    size_t pushed = 0;
                    while (pushed < n)
                        pushed += q.try_enqueue_bulk(buf + pushed, n - pushed);
                    next += n;
                }
                producers_done.fetch_add(1);
            });
    for (int c = 0; c < kConsumers; ++c)
        th.emplace_back(
            [&]
            {
                uint64_t buf[32];
                uint64_t sum = 0, cnt = 0;
                for (;;)
                {
                    size_t n = q.try_dequeue_bulk(buf, 32);
                    for (size_t i = 0; i < n; ++i)
                    {
                        sum += buf[i];
                        ++cnt;
                    }
                    if (n == 0)
                    {
                        if (producers_done.load() == kProducers && q.empty())
                            break;
                        std::this_thread::yield();
                    }
                }
                consumed_sum.fetch_add(sum);
                consumed_cnt.fetch_add(cnt);
            });
    for (auto& t : th)
        t.join();

    const uint64_t total = kProducers * kPerProducer;
    CHECK_EQ(consumed_cnt.load(), total);
    CHECK_EQ(consumed_sum.load(), total * (total - 1) / 2);
    CHECK(q.empty());
}

int main()
{
    return run_tests({
        {"fifo_single_thread", fifo_single_thread},
        {"interleaved_fifo_across_segments", interleaved_fifo_across_segments},
        {"unbounded_growth", unbounded_growth},
        {"bulk_ops", bulk_ops},
        {"mpmc_stress_1p1c", mpmc_stress_1p1c},
        {"mpmc_stress_4p4c", mpmc_stress_4p4c},
        {"mpmc_stress_many", mpmc_stress_many},
        {"mpmc_stress_bulk", mpmc_stress_bulk},
        {"mpmc_stress_asym", mpmc_stress_asym},
        {"move_only_no_leak", move_only_no_leak},
        {"move_only_mt", move_only_mt},
        {"destructor_drains_leftovers", destructor_drains_leftovers},
        {"thread_churn", thread_churn},
        {"reclamation_bounded_memory", reclamation_bounded_memory},
        {"legacy_bounded_bulk_no_loss", legacy_bounded_bulk_no_loss},
    });
}
