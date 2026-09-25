//
// Created by root on 10/21/25.
//

#ifndef TLS_UVENT_H
#define TLS_UVENT_H

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <mutex>
#include <uvent/base/Predefines.h>
#include <uvent/poll/PollerBase.h>
#include <uvent/tasks/AwaitableFrame.h>
#include <uvent/tasks/TaskState.h>
#include <uvent/utils/datastructures/queue/ConcurrentQueues.h>
#include <uvent/utils/datastructures/queue/IntrusiveMPSC.h>
#include <vector>

namespace usub::uvent::net
{
    struct SocketHeader;
}

namespace usub::uvent::thread
{
#ifdef UVENT_SOCKET_OWNER_FORWARDING
    /**
     * \brief Socket maintenance request forwarded to the worker that owns the
     *        socket header (see SocketHeader::owner_tid).
     *
     * With UVENT_ENABLE_REUSEADDR the poller, the timer wheel and the header
     * delete queue are thread_local. A coroutine that migrated to another
     * worker must not touch them directly: cancelTimer() would hit a foreign
     * wheel (no-op + id collision in cancelledPending_), removeEvent() a
     * foreign epoll and `delete` would free memory the owner's wheel still
     * references (heap-use-after-free in TimerWheel::tick). Instead the
     * operation is queued here and applied by the owner in its event loop
     * (Thread::processSocketOps).
     */
    struct SocketOp
    {
        enum class Kind : uint8_t
        {
            None = 0,
            /// arm (timer_id == 0) or refresh (timer_id != 0) the socket timeout
            Timeout,
            /// shutdown() from a foreign thread: cancel the socket timer and
            /// shutdown(2) the fd — both on the owner, so a stale fd number
            /// (owner already closed it) can never hit an unrelated socket
            Shutdown,
            /// full teardown: cancel timer, removeEvent, queue header for delete
            Destroy
        };

        Kind kind{Kind::None};
        net::SocketHeader* header{nullptr};
        uint64_t timeout_ms{0};
    };
#endif

    struct alignas(data_structures::metadata::CACHELINE_SIZE) ThreadLocalStorage
    {
        friend class system::Thread;

        void push_task_inbox(std::coroutine_handle<> task);

        void push_cancel_kick(uvent::task::TaskStateBase* t);

#ifdef UVENT_SOCKET_OWNER_FORWARDING
        /**
         * \brief Forward a socket maintenance op to this (owner) worker and wake it.
         *        Thread-safe; may be called from any thread except the owner itself
         *        (the owner applies ops directly).
         */
        void push_socket_op(const SocketOp& op);
#endif

        void set_poller(core::PollerImpl* p) noexcept { this->poller_.store(p, std::memory_order_release); }

        /// \brief The worker's poller, or nullptr before the worker started / after it exited.
        [[nodiscard]] core::PollerImpl* poller() const noexcept { return this->poller_.load(std::memory_order_acquire); }

        /**
         * \brief Unregister the poller before the worker destroys it (thread exit).
         *        Blocks until every concurrent wake that already grabbed the pointer has finished,
         *        so nobody writes to a closed wake fd.
         */
        void unset_poller() noexcept;

        void wake_poller() noexcept;

#ifdef UVENT_RUNTIME_DRAIN
        /**
         * \brief Remember a spawned task (holds one reference). Called on this
         *        worker's own thread only: plain vector push, no atomics besides
         *        the add_ref the caller already did. Done entries are swept
         *        lazily (amortised O(1)) when the vector doubles.
         */
        void register_task(uvent::task::TaskStateBase* t);

        /// \brief Same from a non-worker thread (rare path, mutex).
        void register_task_external(uvent::task::TaskStateBase* t);

        /// \brief request_cancel() every live registered task. Worker thread only.
        void cancel_registered_tasks();

        /// \brief Drop done entries; returns how many live tasks remain. Worker thread only.
        std::size_t sweep_tasks();

        /// \brief Releases every remaining registry reference. Worker thread only.
        void release_registered_tasks();

        [[nodiscard]] bool drain_idle() const noexcept { return this->drain_idle_.load(std::memory_order_acquire); }

        void set_drain_idle(bool v) noexcept { this->drain_idle_.store(v, std::memory_order_release); }
#endif

    private:
        queue::concurrent::IntrusiveMPSCQueue<detail::AwaitableFrameBase> inbox_q_;
        std::atomic_bool is_added_new_{false};
        queue::concurrent::IntrusiveMPSCQueue<uvent::task::TaskStateBase> kick_q_;
        std::atomic_bool has_kicks_{false};
#ifdef UVENT_SOCKET_OWNER_FORWARDING
        queue::concurrent::SegmentedMPMCQueue<SocketOp> sock_ops_q_;
        std::atomic_bool has_sock_ops_{false};
#endif
        std::atomic<core::PollerImpl*> poller_{nullptr};
        std::atomic<int> wake_inflight_{0};
#ifdef UVENT_RUNTIME_DRAIN
        std::vector<uvent::task::TaskStateBase*> tasks_;
        std::size_t sweep_at_{64};
        std::mutex ext_mtx_;
        std::vector<uvent::task::TaskStateBase*> ext_tasks_;
        std::atomic<bool> drain_idle_{false};

        void take_external_tasks();
#endif

        void kick_poller() noexcept;
    };
} // namespace usub::uvent::thread

#endif // TLS_UVENT_H
