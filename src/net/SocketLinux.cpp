//
// Created by root on 9/11/25.
//

#include "uvent/net/Socket.h"

namespace usub::uvent::net::detail
{
    void processSocketTimeout(std::any arg)
    {
        auto header = std::any_cast<SocketHeader*>(arg);
#ifdef UVENT_ENABLE_REUSEADDR
        // Runs on the owner (the wheel is thread_local). Claim the timer's reference;
        // if shutdown() on another worker already claimed it, or the header's teardown
        // is already forwarded to us, do nothing — in particular do not touch the
        // refcount and do not removeEvent (Destroy will).
        const uint8_t prev = header->tflags.fetch_or(tflags::TIMER_CLAIMED, std::memory_order_acq_rel);
        if (prev & (tflags::TIMER_CLAIMED | tflags::TEARDOWN_PENDING))
        {
            header->timer_id = 0;
            return;
        }
#endif
        auto socket = Socket<Proto::TCP, Role::ACTIVE>::from_existing(header);
        header->timer_id = 0;

#if UVENT_DEBUG
        spdlog::warn("Socket timeout: {}, counter: {}", header->fd, header->get_counter());
#endif
#ifndef UVENT_ENABLE_REUSEADDR
        const uint64_t expected = header->timeout_epoch_load();
        if (!header->try_mark_busy()) {
            return;
        }

        if (header->timeout_epoch_changed(expected)) {
            header->clear_busy();
            return;
        }
        header->mark_disconnected();
#endif
        auto r = header->take_read_waiter();
        auto w = header->take_write_waiter();
#ifndef UVENT_ENABLE_REUSEADDR
        header->clear_busy();
#endif
        system::this_thread::detail::pl.removeEvent(header);
#if UVENT_DEBUG
        spdlog::warn("Socket counter in timeout: {}", header->get_counter());
#endif
        header->socket_info |= static_cast<uint8_t>(AdditionalState::TIMEOUT);
#ifndef UVENT_ENABLE_REUSEADDR
            if (!header->is_done_client_coroutine_with_timeout() && r) system::resume_waiter(r);
            if (!header->is_done_client_coroutine_with_timeout() && w) system::resume_waiter(w);
#else
        if (r) system::resume_waiter(r);
        if (w) system::resume_waiter(w);
#endif
    }

#ifdef UVENT_SOCKET_OWNER_FORWARDING
    void arm_or_refresh_socket_timer(SocketHeader* h, uint64_t timeout_ms) noexcept
    {
        if (h->timer_id != 0)
        {
            system::this_thread::detail::wh.updateTimer(h->timer_id, static_cast<timer_duration_t>(timeout_ms));
            return;
        }
        auto* timer = &h->timer;
        timer->arm_embedded(static_cast<timer_duration_t>(timeout_ms),
                            [](void* hp)
                            {
                                std::any a{static_cast<SocketHeader*>(hp)};
                                processSocketTimeout(a);
                            },
                            h);
        h->timer_id = system::this_thread::detail::wh.addTimer(timer);
    }

    void teardown_socket_header_local(SocketHeader* h) noexcept
    {
        if (h->timer_id != 0)
        {
            system::this_thread::detail::wh.cancelTimer(h->timer_id);
            h->timer_id = 0;
        }
        h->close_for_new_refs();
        system::this_thread::detail::pl.removeEvent(h);
        h->tflags.fetch_or(tflags::DESTROYED, std::memory_order_acq_rel);
        // Ops forwarded before the last reference was dropped may still sit in our
        // queue; the last of them frees the header (see apply_socket_op).
        if (h->pending_ops.load(std::memory_order_acquire) == 0)
            system::this_thread::detail::q_sh.enqueue(h);
    }

    void apply_socket_op(const thread::SocketOp& op) noexcept
    {
        SocketHeader* h = op.header;
        if (!h)
            return;

        using Kind = thread::SocketOp::Kind;
        const uint8_t f = h->tflags.load(std::memory_order_acquire);
        if ((f & tflags::DESTROYED) == 0)
        {
            switch (op.kind)
            {
            case Kind::Timeout:
            {
                // Teardown already queued behind us, or the timer's reference already
                // consumed (fired/cancelled) — arming now would fire later without a
                // reference to drop.
                if (f & tflags::TEARDOWN_PENDING)
                    break;
                if (h->timer_id == 0 && (f & tflags::TIMER_CLAIMED))
                    break;
                arm_or_refresh_socket_timer(h, op.timeout_ms);
                break;
            }
            case Kind::Shutdown:
            {
                // The caller (shutdown() on a foreign worker) has already claimed and
                // released the timer's reference; detach the node from our wheel and
                // shut the fd down here, where the fd is guaranteed to still be ours.
                if (h->timer_id != 0)
                {
                    system::this_thread::detail::wh.cancelTimer(h->timer_id);
                    h->timer_id = 0;
                }
                if (h->fd >= 0)
                    ::shutdown(h->fd, SHUT_RDWR);
                break;
            }
            case Kind::Destroy:
                teardown_socket_header_local(h);
                break;
            case Kind::None:
            default:
                break;
            }
        }

        // this op is done; if the header is already torn down and we were the last
        // outstanding op, free it now
        if (h->pending_ops.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
            (h->tflags.load(std::memory_order_acquire) & tflags::DESTROYED))
        {
            system::this_thread::detail::q_sh.enqueue(h);
        }
    }
#endif
}
