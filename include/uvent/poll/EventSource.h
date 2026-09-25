#ifndef UVENT_POLL_EVENTSOURCE_H
#define UVENT_POLL_EVENTSOURCE_H

#include <cstdint>

#include "uvent/system/Defines.h"
#ifdef UVENT_ENABLE_IO_URING
#include "uvent/poll/IOUringOps.h"
#endif

namespace usub::uvent::core
{
    /**
     * \brief A readiness source that is not a socket: signal pipe, eventfd, timerfd, pidfd, inotify, …
     *
     * A socket is driven by the awaiter machinery in SocketHeader; an EventSource is driven by a plain
     * callback invoked on the polling worker, inside PollerImpl::poll(), every time the descriptor is
     * ready. The callback decides what to do (read the fd, wake waiters through resume_waiter, …).
     *
     * Registration: `PollerImpl::addSource(src, READ|WRITE)` on the worker whose poller should watch it,
     * `PollerImpl::removeSource(src)` before the source is destroyed (it never closes `fd`).
     *
     * Readiness contract: the callback is invoked at least once after every new readiness of the fd and must
     * drain what it wants (read until EAGAIN). Whether *stale* readiness is reported again on the next poll
     * is backend-specific: epoll and kqueue are level-triggered by default (`edge = true` gives EPOLLET /
     * EV_CLEAR), io_uring's multishot POLL_ADD fires once per wake-up of the descriptor — i.e. behaves like
     * edge-triggered — and IOCP has no notion of readiness at all (see below). Portable callbacks therefore
     * always drain.
     *
     * The poller tells a source apart from a socket header by the low bit of the pointer it stores in
     * epoll_data / kevent.udata / the IOCP completion key, so both types must be at least 2-byte aligned
     * (SocketHeader is alignas(32); this struct is alignas(8)). That costs one predictable branch per
     * event in the dispatch loop and nothing else.
     *
     * Windows: there is no descriptor to watch. A source is a completion key; whoever produces the event
     * (a console control handler, a thread) calls `IocpPoller::post(src, ready)` and the poller invokes
     * `on_ready` on its worker. `addSource` / `removeSource` are no-ops there.
     *
     * io_uring: `addSource` arms a multishot POLL_ADD on the fd. `removeSource` cancels it; the source
     * must stay alive until the poller has processed the cancellation CQE (in practice: until the next
     * poll() on that worker) — keep sources long-lived or remove them from the owning worker and destroy
     * them one loop iteration later.
     */
    struct alignas(8) EventSource
    {
        enum Ready : uint32_t
        {
            READABLE = 1u << 0,
            WRITABLE = 1u << 1,
            HUP = 1u << 2,
            ERR = 1u << 3 // not ERROR: windows.h defines that macro
        };

        using Callback = void (*)(EventSource* self, uint32_t ready) noexcept;

        socket_fd_t fd{INVALID_FD};
        Callback on_ready{nullptr};
        /// Free for the owner (registry pointer, index, …).
        void* user{nullptr};
        /// Edge-triggered registration (EPOLLET / EV_CLEAR). io_uring is always wake-driven; IOCP ignores it.
        bool edge{false};
#ifdef UVENT_ENABLE_IO_URING
        /// State of the multishot POLL_ADD that watches `fd` (kind = IoOpKind::EventSource).
        struct Op : detail::IoOpBase
        {
            EventSource* owner{nullptr};
        } op{};
        uint32_t poll_mask{0};
        bool armed{false};
        bool removed{false};
#endif

        static constexpr uintptr_t kTag = 1;

        [[nodiscard]] void* tagged() noexcept { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(this) | kTag); }

        /// True when a pointer stored by the poller refers to an EventSource rather than a SocketHeader.
        [[nodiscard]] static bool is_tagged(const void* p) noexcept
        {
            return (reinterpret_cast<uintptr_t>(p) & kTag) != 0;
        }

        [[nodiscard]] static EventSource* untag(void* p) noexcept
        {
            return reinterpret_cast<EventSource*>(reinterpret_cast<uintptr_t>(p) & ~kTag);
        }
    };
} // namespace usub::uvent::core

#endif // UVENT_POLL_EVENTSOURCE_H
