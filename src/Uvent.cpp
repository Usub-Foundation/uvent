//
// Created by kirill on 11/17/24.
//

#include <chrono>

#include "uvent/Uvent.h"

namespace usub
{
    Uvent::Uvent(int threadCount) : pool(threadCount), thread_count_(threadCount)
    {
        uvent::system::global::detail::thread_count = threadCount;
#ifdef UVENT_RUNTIME_DRAIN
        uvent::system::global::detail::draining.store(false, std::memory_order_relaxed);
        uvent::system::global::detail::drain_deadline_ns.store(0, std::memory_order_relaxed);
        uvent::system::global::detail::drain_survivors.store(0, std::memory_order_relaxed);
#endif
    }

    void Uvent::stop()
    {
#ifdef UVENT_RUNTIME_DRAIN
        using namespace uvent::system::global::detail;
        if (uvent::settings::stop_drain_timeout_ms > 0 && !draining.exchange(true, std::memory_order_acq_rel))
        {
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            drain_deadline_ns.store(static_cast<uint64_t>(now) + uvent::settings::stop_drain_timeout_ms * 1000000ull,
                                    std::memory_order_relaxed);
            for (int i = 0; i < this->thread_count_; ++i)
                tls_registry->getStorage(i)->wake_poller();
            return; // workers finish the drain and stop themselves (Thread::drainStep)
        }
#endif
        this->pool.stop();
    }

    std::size_t Uvent::drain_survivors() noexcept
    {
#ifdef UVENT_RUNTIME_DRAIN
        return uvent::system::global::detail::drain_survivors.load(std::memory_order_relaxed);
#else
        return 0;
#endif
    }

    void Uvent::run() { this->pool.addThread(uvent::system::CURRENT); }

    void Uvent::for_each_thread(std::function<void(int, uvent::thread::ThreadLocalStorage*)> f) const
    {
        for (int i = 0; i < this->thread_count_; i++)
            f(i, uvent::system::global::detail::tls_registry->getStorage(i));
    }
} // namespace usub
