//
// Created by root on 9/13/25.
//

#ifndef SOCKETMETADATA_H
#define SOCKETMETADATA_H

#include <atomic>
#include <coroutine>

#if UVENT_DEBUG
#include "spdlog/spdlog.h"
#endif

#include "uvent/base/Predefines.h"
#include "uvent/system/Defines.h"
#include "uvent/utils/intrinsics/optimizations.h"
#include "uvent/utils/sync/RefCountedSession.h"
#include "uvent/utils/timer/Timer.h"
#ifdef UVENT_ENABLE_IO_URING
#include "uvent/poll/IOUringOps.h"
#endif

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/ioctl.h>
#endif

/**
 * Owner-forwarding of socket maintenance ops (see SocketHeader::owner_tid,
 * thread::SocketOp). Implemented for the epoll backend in REUSEADDR mode, where
 * poller / timer wheel / header delete queue are thread_local. The io_uring
 * backend keeps its own teardown paths and does not stamp owner_tid.
 */
#if defined(UVENT_ENABLE_REUSEADDR) && !defined(UVENT_ENABLE_IO_URING) && defined(__linux__)
#define UVENT_SOCKET_OWNER_FORWARDING 1
#endif

namespace usub::uvent::net
{
    enum class Proto : uint8_t
    {
        TCP = 1 << 0,
        UDP = 1 << 1
    };

    enum class Role : uint8_t
    {
        PASSIVE = 1 << 2,
        ACTIVE = 1 << 3
    };

    enum class AdditionalState : uint8_t
    {
        CONNECTION_PENDING = 1 << 4,
        CONNECTION_FAILED = 1 << 5,
        DISCONNECTED = 1 << 6,
        TIMEOUT = 1 << 7
    };

    struct alignas(32) SocketHeader
    {
        socket_fd_t fd{INVALID_FD};
        /**
         * \brief Id of the embedded timer in the owner's wheel (0 = not armed).
         *        Atomic because a coroutine on another worker reads it (shutdown /
         *        set_timeout_ms) while the owner's timeout callback resets it; the
         *        protocol tolerates stale values (see tflags), it just must not tear.
         */
        std::atomic<uint64_t> timer_id{0};
        uint8_t socket_info;
        /// SO_ERROR observed by the poller when a pending connect completed with
        /// CONNECTION_FAILED (getsockopt clears it, so async_connect cannot read
        /// it again). ETIMEDOUT here is a real timeout; anything else is a refusal.
        int connect_error{0};
        /**
         * \brief Read / write wake-up words: exactly one of
         *          0        — idle: no waiter, no pending readiness,
         *          READY    — the poller saw an edge and nobody was parked (a hint:
         *                     "probe again before you park"),
         *          <handle> — a parked continuation (frame address, READY bit clear).
         *
         * One atomic word per direction makes "publish the waiter" and "read the
         * hint" a single CAS, so the classic lost wake-up between a coroutine on
         * worker A and the owner's poller on worker B cannot happen:
         *   poller  : prev = rd.exchange(READY); if prev is a handle -> resume it
         *   waiter  : CAS(0 -> handle) parks; if the word is READY the waiter consumes
         *             it (CAS READY -> 0) and does not park at all
         *   consumer: disarm_read() (CAS READY -> 0) BEFORE the probe (recv/accept/
         *             FIONREAD), never after it — a hint cleared after the probe would
         *             erase an edge that landed in between and the coroutine would
         *             sleep on data already in the kernel.
         * After a successful publish the waiter touches neither the frame nor the
         * header again: another worker may resume (and even finish) the coroutine
         * the very next instant.
         */
        std::atomic<uintptr_t> rd{0}, wr{0};
        static constexpr uintptr_t READY = 1;
        utils::Timer timer{0};
#ifdef UVENT_ENABLE_IO_URING
        void* read_op{nullptr};
        void* write_op{nullptr};
        core::detail::MultishotRecvOp ms_recv{};
        bool first_read_done{false};
        bool first_write_done{false};
#endif
        /**
         * \brief Refcount + flag bits (see utils::sync::refc). Atomic in every
         *        build: with UVENT_ENABLE_REUSEADDR a socket is still normally
         *        driven by one worker, but a coroutine that migrated to another
         *        worker may drop references / shut the socket down concurrently
         *        with the owner's timeout callback, so plain RMW would lose
         *        updates (leaked or prematurely destroyed headers).
         */
        std::atomic<uint64_t> state;
#ifdef UVENT_ENABLE_REUSEADDR
        /**
         * \brief Index of the worker thread whose poller/timer wheel own this
         *        header (stamped by PollerImpl::addEvent). -1 = not registered.
         *
         * In UVENT_ENABLE_REUSEADDR builds the poller, the timer wheel and the
         * socket-header delete queue are thread_local, so every operation that
         * touches them (arm/update/cancel timer, removeEvent, delete) must run
         * on this thread. Calls made from another worker are forwarded to the
         * owner via ThreadLocalStorage::push_socket_op().
         */
        int owner_tid{-1};
        /**
         * \brief Cross-thread timer bookkeeping (bits: net::detail::tflags).
         *  - TIMER_CLAIMED: the timer's reference was consumed (timeout fired on
         *    the owner, or shutdown() cancelled it) — set with fetch_or by the
         *    winner, the loser must not release() again;
         *  - TEARDOWN_PENDING: destroy() ran on a foreign thread and Destroy was
         *    forwarded to the owner; the owner's timeout callback becomes a no-op;
         *  - DESTROYED: teardown (timer removed, fd closed) is done on the owner;
         *    the header is freed as soon as pending_ops drains to zero.
         */
        std::atomic<uint8_t> tflags{0};
        /**
         * \brief Number of SocketOps forwarded to the owner and not yet applied.
         *        Incremented by the forwarding thread, decremented by the owner
         *        after applying. The header is deleted only when it is destroyed
         *        AND this drops to zero, so a late op never touches freed memory.
         */
        std::atomic<uint32_t> pending_ops{0};
#endif

#if UVENT_DEBUG
        ~SocketHeader() { spdlog::info("Socket header destroyed: {}", this->fd); }
#endif

#ifdef UVENT_ENABLE_REUSEADDR
        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool has_owner() const noexcept { return this->owner_tid >= 0; }
#endif

        UVENT_ALWAYS_INLINE_FN void decrease_ref() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_sub(1, std::memory_order_release);
        }

        UVENT_ALWAYS_INLINE_FN void close_for_new_refs() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_or(CLOSED_MASK, std::memory_order_release);
        }

        UVENT_ALWAYS_INLINE_FN bool try_mark_busy() noexcept
        {
            using namespace usub::utils::sync::refc;
            uint64_t s = this->state.load(std::memory_order_relaxed);
            for (;;)
            {
                if ((s & (CLOSED_MASK | DISCONNECTED_MASK | BUSY_MASK)) != 0)
                    return false;
                const uint64_t ns = s | BUSY_MASK;
                if (this->state.compare_exchange_weak(s, ns, std::memory_order_acq_rel, std::memory_order_relaxed))
                    return true;
                cpu_relax();
            }
        }

        UVENT_ALWAYS_INLINE_FN void clear_busy() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_and(~BUSY_MASK, std::memory_order_release);
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_busy_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & BUSY_MASK) != 0;
        }

        UVENT_ALWAYS_INLINE_FN bool try_mark_reading() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            uint64_t s = this->state.load(std::memory_order_relaxed);
            for (;;)
            {
                if ((s & CLOSED_MASK) != 0)
                    return false;
                const uint64_t ns = s | READING_MASK;
                if (this->state.compare_exchange_weak(s, ns, std::memory_order_acq_rel, std::memory_order_relaxed))
                    return true;
                cpu_relax();
            }
#else
            this->state.fetch_or(READING_MASK, std::memory_order_acq_rel);
            return true;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void clear_reading() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_and(~READING_MASK, std::memory_order_release);
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_reading_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & READING_MASK) != 0;
        }

        UVENT_ALWAYS_INLINE_FN bool try_mark_writing() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            uint64_t s = this->state.load(std::memory_order_relaxed);
            for (;;)
            {
                if ((s & CLOSED_MASK) != 0)
                    return false;
                const uint64_t ns = s | WRITING_MASK;
                if (this->state.compare_exchange_weak(s, ns, std::memory_order_acq_rel, std::memory_order_relaxed))
                    return true;
                cpu_relax();
            }
#else
            this->state.fetch_or(WRITING_MASK, std::memory_order_acq_rel);
            return true;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void clear_writing() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_and(~WRITING_MASK, std::memory_order_release);
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_writing_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & WRITING_MASK) != 0;
        }

        UVENT_ALWAYS_INLINE_FN void mark_disconnected() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_or(DISCONNECTED_MASK, std::memory_order_release);
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_disconnected_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & DISCONNECTED_MASK) != 0;
        }

        static UVENT_ALWAYS_INLINE_FN std::coroutine_handle<> handle_of(uintptr_t w) noexcept
        {
            return (w & ~READY) ? std::coroutine_handle<>::from_address(reinterpret_cast<void*>(w & ~READY))
                                : std::coroutine_handle<>{};
        }

        /// Poller: an edge arrived. Leaves READY set and returns the parked waiter, if any.
        UVENT_ALWAYS_INLINE_FN std::coroutine_handle<> fire_read() noexcept
        {
            return handle_of(this->rd.exchange(READY, std::memory_order_seq_cst));
        }

        UVENT_ALWAYS_INLINE_FN std::coroutine_handle<> fire_write() noexcept
        {
            return handle_of(this->wr.exchange(READY, std::memory_order_seq_cst));
        }

        /// Timeout / teardown: detach the parked waiter (READY hint dropped as well).
        UVENT_ALWAYS_INLINE_FN std::coroutine_handle<> take_read_waiter() noexcept
        {
            return handle_of(this->rd.exchange(0, std::memory_order_seq_cst));
        }

        UVENT_ALWAYS_INLINE_FN std::coroutine_handle<> take_write_waiter() noexcept
        {
            return handle_of(this->wr.exchange(0, std::memory_order_seq_cst));
        }

        /**
         * \brief Waiter: park \p h. Returns true if parked (the poller / cancel hook
         *        will resume it), false if a READY hint was consumed instead and the
         *        caller must resume \p h itself (or just continue).
         */
        UVENT_ALWAYS_INLINE_FN bool park_read(std::coroutine_handle<> h) noexcept
        {
            return park(this->rd, h);
        }

        UVENT_ALWAYS_INLINE_FN bool park_write(std::coroutine_handle<> h) noexcept
        {
            return park(this->wr, h);
        }

        /// Cancel hook: un-park exactly this waiter. True if we own its resume.
        UVENT_ALWAYS_INLINE_FN bool unpark_read(std::coroutine_handle<> h) noexcept
        {
            uintptr_t e = reinterpret_cast<uintptr_t>(h.address());
            return this->rd.compare_exchange_strong(e, 0, std::memory_order_seq_cst, std::memory_order_seq_cst);
        }

        UVENT_ALWAYS_INLINE_FN bool unpark_write(std::coroutine_handle<> h) noexcept
        {
            uintptr_t e = reinterpret_cast<uintptr_t>(h.address());
            return this->wr.compare_exchange_strong(e, 0, std::memory_order_seq_cst, std::memory_order_seq_cst);
        }

        /// Consumer-side hint check / clear. disarm_* must run BEFORE the readiness probe.
        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_read_armed() const noexcept
        {
            return (this->rd.load(std::memory_order_seq_cst) & READY) != 0;
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_write_armed() const noexcept
        {
            return (this->wr.load(std::memory_order_seq_cst) & READY) != 0;
        }

        UVENT_ALWAYS_INLINE_FN void disarm_read() noexcept
        {
            uintptr_t e = READY;
            this->rd.compare_exchange_strong(e, 0, std::memory_order_seq_cst, std::memory_order_seq_cst);
        }

        UVENT_ALWAYS_INLINE_FN void disarm_write() noexcept
        {
            uintptr_t e = READY;
            this->wr.compare_exchange_strong(e, 0, std::memory_order_seq_cst, std::memory_order_seq_cst);
        }

        /// Kept for callers that used the old hint API: same as the poller's edge without a waiter.
        UVENT_ALWAYS_INLINE_FN void mark_read_pending() noexcept { (void)this->fire_read(); }
        UVENT_ALWAYS_INLINE_FN void mark_write_pending() noexcept { (void)this->fire_write(); }

    private:
        static UVENT_ALWAYS_INLINE_FN bool park(std::atomic<uintptr_t>& word, std::coroutine_handle<> h) noexcept
        {
            const uintptr_t me = reinterpret_cast<uintptr_t>(h.address());
            uintptr_t cur = word.load(std::memory_order_seq_cst);
            for (;;)
            {
                if (cur & READY)
                {
                    // An edge landed since the consumer's probe: consume the hint, do not park.
                    if (word.compare_exchange_weak(cur, 0, std::memory_order_seq_cst, std::memory_order_seq_cst))
                        return false;
                    continue;
                }
                if (cur == 0)
                {
                    if (word.compare_exchange_weak(cur, me, std::memory_order_seq_cst, std::memory_order_seq_cst))
                        return true;
                    continue;
                }
                // Another continuation is already parked on this direction: unsupported
                // (one reader + one writer per socket), keep the old one and treat ours as parked.
                return true;
            }
        }

    public:
        [[nodiscard]] UVENT_ALWAYS_INLINE_FN uint64_t timeout_epoch_snapshot() const noexcept
        {
            using namespace usub::utils::sync::refc;
            return this->state.load(std::memory_order_acquire) & TIMEOUT_EPOCH_MASK;
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN uint64_t timeout_epoch_load() const noexcept
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & TIMEOUT_EPOCH_MASK);
        }

        UVENT_ALWAYS_INLINE_FN void timeout_epoch_bump() noexcept
        {
            using namespace usub::utils::sync::refc;
            this->state.fetch_add(TIMEOUT_EPOCH_STEP, std::memory_order_acq_rel);
        }

        UVENT_ALWAYS_INLINE_FN bool timeout_epoch_changed(uint64_t snap) noexcept
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & TIMEOUT_EPOCH_MASK) != snap;
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_done_client_coroutine_with_timeout() const
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & COUNT_MASK) == 1;
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN uint64_t get_counter() const
        {
            using namespace usub::utils::sync::refc;
            return (this->state.load(std::memory_order_acquire) & COUNT_MASK);
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_tcp() const
        {
            return (this->socket_info & static_cast<uint8_t>(Proto::TCP)) != 0;
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_passive() const
        {
            return (this->socket_info & static_cast<uint8_t>(Role::PASSIVE)) != 0;
        }


        /**
         * \brief Reports whether the kernel buffer of this socket has any
         *        unread bytes pending (cross-platform).
         *
         * Wraps a single non-blocking probe (FIONREAD) and works identically
         * on Linux, BSD/macOS and Windows. Intended for adapters that wrap
         * external libraries which don't expose EAGAIN to the caller (e.g.
         * libpq's PQconsumeInput) and therefore can't otherwise tell whether
         * the socket has been drained.
         *
         * Not used on hot paths of async_read/async_write — those rely on
         * EAGAIN from recv/send directly. Use only when external API hides
         * the EAGAIN signal.
         *
         * \return true if a non-blocking recv() would return at least one
         *         byte, false if the socket buffer is empty or the probe
         *         failed.
         */
        UVENT_ALWAYS_INLINE_FN bool has_unread_bytes() const noexcept
        {
            if (this->fd < 0)
                return false;
#ifdef _WIN32
            u_long n = 0;
            if (::ioctlsocket(this->fd, FIONREAD, &n) != 0)
                return false;
            return n > 0;
#else
            int n = 0;
            if (::ioctl(this->fd, FIONREAD, &n) != 0)
                return false;
            return n > 0;
#endif
        }
    };

#ifndef UVENT_ENABLE_REUSEADDR
    static void delete_header(void* ptr) { delete static_cast<SocketHeader*>(ptr); }
#endif

    template <Proto p, Role r>
    class Socket;

    namespace detail
    {
        /// Datagram send: to Socket::address when one is set (like async_send),
        /// otherwise plain send() for a connect(2)-ed datagram socket.
        inline ssize_t udp_send(socket_fd_t fd, const client_addr_t& addr, const uint8_t* p, size_t n,
                                int flags) noexcept
        {
            if (const auto* a4 = std::get_if<sockaddr_in>(&addr); a4 && a4->sin_family == AF_INET)
                return ::sendto(fd, p, n, flags, reinterpret_cast<const sockaddr*>(a4), sizeof(*a4));
            if (const auto* a6 = std::get_if<sockaddr_in6>(&addr); a6 && a6->sin6_family == AF_INET6)
                return ::sendto(fd, p, n, flags, reinterpret_cast<const sockaddr*>(a6), sizeof(*a6));
            return ::send(fd, p, n, flags);
        }
    } // namespace detail

    using TCPServerSocket = Socket<Proto::TCP, Role::PASSIVE>;
    using TCPClientSocket = Socket<Proto::TCP, Role::ACTIVE>;
    using UDPBoundSocket = Socket<Proto::UDP, Role::ACTIVE>;
    using UDPSocket = Socket<Proto::UDP, Role::PASSIVE>;
} // namespace usub::uvent::net

#endif // SOCKETMETADATA_H
