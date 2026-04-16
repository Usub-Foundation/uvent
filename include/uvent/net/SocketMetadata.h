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
        uint64_t timer_id{0};
        uint8_t socket_info;
        std::coroutine_handle<> first, second;
#ifndef UVENT_ENABLE_REUSEADDR
        std::atomic<uint64_t> state;
#else
        uint64_t state;
#endif

#if UVENT_DEBUG
        ~SocketHeader() { spdlog::info("Socket header destroyed: {}", this->fd); }
#endif

        UVENT_ALWAYS_INLINE_FN void decrease_ref() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_sub(1, std::memory_order_release);
#else
            --this->state;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void close_for_new_refs() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_or(CLOSED_MASK, std::memory_order_release);
#else
            this->state |= CLOSED_MASK;
#endif
        }

        UVENT_ALWAYS_INLINE_FN bool try_mark_busy() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
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
#else
            if ((this->state & (CLOSED_MASK | DISCONNECTED_MASK | BUSY_MASK)) != 0)
                return false;
            this->state |= BUSY_MASK;
            return true;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void clear_busy() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_and(~BUSY_MASK, std::memory_order_release);
#else
            this->state &= ~BUSY_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_busy_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & BUSY_MASK) != 0;
#else
            return (this->state & BUSY_MASK) != 0;
#endif
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
            this->state |= READING_MASK;
            return true;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void clear_reading() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_and(~READING_MASK, std::memory_order_release);
#else
            this->state &= ~READING_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_reading_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & READING_MASK) != 0;
#else
            return this->state & READING_MASK;
#endif
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
            this->state |= WRITING_MASK;
            return true;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void clear_writing() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_and(~WRITING_MASK, std::memory_order_release);
#else
            this->state &= ~WRITING_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_writing_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & WRITING_MASK) != 0;
#else
            return this->state & WRITING_MASK;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void mark_disconnected() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_or(DISCONNECTED_MASK, std::memory_order_release);
#else
            this->state |= DISCONNECTED_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_disconnected_now() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & DISCONNECTED_MASK) != 0;
#else
            return this->state & DISCONNECTED_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN uint64_t timeout_epoch_snapshot() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return this->state.load(std::memory_order_acquire) & TIMEOUT_EPOCH_MASK;
#else
            return this->state & TIMEOUT_EPOCH_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN uint64_t timeout_epoch_load() const noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & TIMEOUT_EPOCH_MASK);
#else
            return this->state & TIMEOUT_EPOCH_MASK;
#endif
        }

        UVENT_ALWAYS_INLINE_FN void timeout_epoch_bump() noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            this->state.fetch_add(TIMEOUT_EPOCH_STEP, std::memory_order_acq_rel);
#else
            this->state += TIMEOUT_EPOCH_STEP;
#endif
        }

        UVENT_ALWAYS_INLINE_FN bool timeout_epoch_changed(uint64_t snap) noexcept
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & TIMEOUT_EPOCH_MASK) != snap;
#else
            return (((this->state) & TIMEOUT_EPOCH_MASK) != snap);
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_done_client_coroutine_with_timeout() const
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & COUNT_MASK) == 1;
#else
            return ((this->state & COUNT_MASK) == 1);
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN uint64_t get_counter() const
        {
            using namespace usub::utils::sync::refc;
#ifndef UVENT_ENABLE_REUSEADDR
            return (this->state.load(std::memory_order_acquire) & COUNT_MASK);
#else
            return this->state & COUNT_MASK;
#endif
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_tcp() const
        {
            return (this->socket_info & static_cast<uint8_t>(Proto::TCP)) != 0;
        }

        [[nodiscard]] UVENT_ALWAYS_INLINE_FN bool is_passive() const
        {
            return (this->socket_info & static_cast<uint8_t>(Role::PASSIVE)) != 0;
        }
    };

#ifndef UVENT_ENABLE_REUSEADDR
    static void delete_header(void* ptr) { delete static_cast<SocketHeader*>(ptr); }
#endif

    template <Proto p, Role r>
    class Socket;

    using TCPServerSocket = Socket<Proto::TCP, Role::PASSIVE>;
    using TCPClientSocket = Socket<Proto::TCP, Role::ACTIVE>;
    using UDPBoundSocket = Socket<Proto::UDP, Role::ACTIVE>;
    using UDPSocket = Socket<Proto::UDP, Role::PASSIVE>;
} // namespace usub::uvent::net

#endif // SOCKETMETADATA_H
