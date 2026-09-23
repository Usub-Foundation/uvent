#ifndef UVENT_SYNC_CANCELSTATE_H
#define UVENT_SYNC_CANCELSTATE_H

#include <atomic>
#include <cstdint>

#include "uvent/sync/WaitList.h"

namespace usub::uvent::sync
{
    struct CancelState
    {
        std::atomic<uint32_t> refs{1};
        std::atomic<bool> requested{false};
        std::atomic<uint32_t> live_tasks{0};
        std::atomic_flag tree_lock{};
        CancelState* parent{nullptr};
        CancelState* first_child{nullptr};
        CancelState* prev_sib{nullptr};
        CancelState* next_sib{nullptr};
        WaitList cancel_waiters;
        WaitList join_waiters;
        bool is_task{false};

        CancelState() = default;
        CancelState(const CancelState&) = delete;
        CancelState& operator=(const CancelState&) = delete;
        virtual ~CancelState() = default;

        [[nodiscard]] bool stop_requested() const noexcept { return this->requested.load(std::memory_order_relaxed); }

        void add_ref() noexcept { this->refs.fetch_add(1, std::memory_order_relaxed); }

        [[nodiscard]] bool try_add_ref() noexcept
        {
            uint32_t r = this->refs.load(std::memory_order_relaxed);
            while (r != 0)
                if (this->refs.compare_exchange_weak(r, r + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                    return true;
            return false;
        }

        void release() noexcept;

        void request_cancel() noexcept;

        void lock_tree() noexcept
        {
            while (this->tree_lock.test_and_set(std::memory_order_acquire))
                cpu_relax();
        }

        void unlock_tree() noexcept { this->tree_lock.clear(std::memory_order_release); }

        static void link(CancelState* parent, CancelState* child) noexcept;

        void unlink_from_parent() noexcept;

        void retain_live_chain() noexcept;

        void drop_live_chain() noexcept;
    };
} // namespace usub::uvent::sync

#endif // UVENT_SYNC_CANCELSTATE_H
