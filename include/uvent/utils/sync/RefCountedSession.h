//
// Created by root on 9/11/25.
//

#ifndef REFCOUNTEDSESSION_H
#define REFCOUNTEDSESSION_H

#include <atomic>
#include <concepts>

#include "uvent/system/Defines.h"
#include "uvent/utils/intrinsics/optimizations.h"

namespace usub::utils::sync::refc
{
    inline constexpr uint64_t CLOSED_MASK = 1ull << 63;
    inline constexpr uint64_t BUSY_MASK = 1ull << 62;
    inline constexpr uint64_t READING_MASK = 1ull << 61;
    inline constexpr uint64_t WRITING_MASK = 1ull << 60;
    inline constexpr uint64_t DISCONNECTED_MASK = 1ull << 59;
    inline constexpr uint64_t FLAGS_MASK = (CLOSED_MASK | BUSY_MASK | READING_MASK | WRITING_MASK | DISCONNECTED_MASK);

    inline constexpr uint64_t REFCOUNT_BITS = 40;
    inline constexpr uint64_t COUNT_MASK = (1ull << REFCOUNT_BITS) - 1ull;

    static constexpr unsigned TIMEOUT_EPOCH_SHIFT = REFCOUNT_BITS;
    static constexpr unsigned TIMEOUT_EPOCH_BITS = 16;
    static constexpr uint64_t TIMEOUT_EPOCH_STEP = 1ull << TIMEOUT_EPOCH_SHIFT;
    static constexpr uint64_t TIMEOUT_EPOCH_MASK =
        (((1ull << TIMEOUT_EPOCH_BITS) - 1ull) << TIMEOUT_EPOCH_SHIFT); // [55:40]

    static_assert((TIMEOUT_EPOCH_MASK & DISCONNECTED_MASK) == 0, "epoch overlaps DISCONNECTED");
    static_assert((TIMEOUT_EPOCH_MASK & BUSY_MASK) == 0, "epoch overlaps flags");
    static_assert((TIMEOUT_EPOCH_MASK & COUNT_MASK) == 0, "epoch overlaps refcount");

    template <class Derived>
    class RefCounted
    {
    public:
        RefCounted() noexcept = default;
        virtual ~RefCounted() = default;

        bool try_add_ref() noexcept
        {
            auto& st = state();
#ifndef UVENT_ENABLE_REUSEADDR
            uint64_t s = st.load(std::memory_order_relaxed);
            for (;;) {
                if (is_closed(s)) return false;
                if ((s & COUNT_MASK) == COUNT_MASK) return false;
                uint64_t ns = (s & ~COUNT_MASK) | ((s & COUNT_MASK) + 1);
                if (st.compare_exchange_weak(s, ns, std::memory_order_acquire, std::memory_order_relaxed))
                    return true;
                cpu_relax();
            }
#else
            const uint64_t s = st;
            if (is_closed(s)) return false;
            if ((s & COUNT_MASK) == COUNT_MASK) return false;
            st = (s & ~COUNT_MASK) | ((s & COUNT_MASK) + 1);
            return true;
#endif
        }

        void add_ref() noexcept
        {
#ifndef UVENT_ENABLE_REUSEADDR
#if defined(UVENT_DEBUG)
            uint64_t s = state().load(std::memory_order_relaxed);
            if (is_closed(s)) __builtin_trap();
#endif
            state().fetch_add(1, std::memory_order_relaxed);
#else
            ++state();
#endif
        }

        void release() noexcept
        {
#ifndef UVENT_ENABLE_REUSEADDR
            uint64_t prev = state().fetch_sub(1, std::memory_order_release);
            if ((prev & COUNT_MASK) == 1)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
                destroy();
            }
#else
            uint64_t& st = this->state();
            const uint64_t prev = st;
            const uint64_t cnt = prev & COUNT_MASK;
            // if (cnt == 0) __builtin_trap();

            st = (prev & ~COUNT_MASK) | ((cnt - 1) & COUNT_MASK);
            if (cnt == 1)
                destroy();

#endif
        }

        UVENT_ALWAYS_INLINE_FN void close_for_new_refs() noexcept
        {
            static_cast<Derived*>(this)->close_for_new_refs();
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_disconnected_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state().load(std::memory_order_acquire) & DISCONNECTED_MASK) != 0;
#else
            return this->state() & DISCONNECTED_MASK;
#endif
        }

    protected:
        static constexpr uint64_t pack(uint64_t cnt, bool closed) noexcept
        {
            return (cnt & COUNT_MASK) | (closed ? CLOSED_MASK : 0);
        }

        static constexpr uint64_t initial_state() noexcept { return pack(1, false); }

        static constexpr bool is_closed(uint64_t s) noexcept { return (s & CLOSED_MASK) != 0; }

        virtual void destroy() noexcept { delete static_cast<Derived*>(this); }

    private:
#ifndef UVENT_ENABLE_REUSEADDR
        std::atomic<uint64_t>& state() noexcept { return static_cast<Derived*>(this)->header_->state; }
        [[nodiscard]] const std::atomic<uint64_t>& state() const noexcept
        {
            return static_cast<const Derived*>(this)->header_->state;
        }
#else
        uint64_t& state() noexcept
        {
            return static_cast<Derived*>(this)->header_->state;
        }

        [[nodiscard]] const uint64_t& state() const noexcept
        {
            return static_cast<const Derived*>(this)->header_->state;
        }
#endif
    };
}

#endif //REFCOUNTEDSESSION_H
