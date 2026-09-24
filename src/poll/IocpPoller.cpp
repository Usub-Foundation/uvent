#include "uvent/poll/IocpPoller.h"
#include "uvent/system/SystemContext.h"
#include "uvent/system/Settings.h"
#include "uvent/net/SocketWindows.h"

namespace usub::uvent::core
{
    IocpPoller::IocpPoller(utils::TimerWheel& wheel) :
        wheel(wheel)
    {
        this->iocp_handle = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
        if (!this->iocp_handle)
        {
#if UVENT_DEBUG
            spdlog::error("IocpPoller ctor: CreateIoCompletionPort failed err={}", GetLastError());
#endif
            throw std::system_error(GetLastError(), std::system_category(), "CreateIoCompletionPort");
        }
        this->events.resize(1024);
#if UVENT_DEBUG
        spdlog::info("IocpPoller ctor: handle={}, events_cap={}",
                     (void*)this->iocp_handle,
                     this->events.size());
#endif
    }

    IocpPoller::~IocpPoller()
    {
#if UVENT_DEBUG
        spdlog::info("IocpPoller dtor: handle={}", (void*)this->iocp_handle);
#endif
        if (this->iocp_handle)
            ::CloseHandle(this->iocp_handle);
    }

    void IocpPoller::addEvent(net::SocketHeader* header, OperationType)
    {
#if UVENT_DEBUG
        spdlog::debug("IocpPoller::addEvent: header={}, fd={}",
                      static_cast<void*>(header),
                      header ? static_cast<std::uint64_t>(header->fd) : 0ull);
#endif
        if (!header || header->fd == INVALID_FD)
            return;

        HANDLE h = ::CreateIoCompletionPort(
            reinterpret_cast<HANDLE>(header->fd),
            this->iocp_handle,
            reinterpret_cast<ULONG_PTR>(header),
            0);
#if UVENT_DEBUG
        if (!h)
        {
            spdlog::error("IocpPoller::addEvent: CreateIoCompletionPort(fd={}) failed err={}",
                          (std::uint64_t)header->fd,
                          GetLastError());
        }
        else
        {
            spdlog::debug("IocpPoller::addEvent: associated fd={} with IOCP handle={}",
                          (std::uint64_t)header->fd,
                          (void*)h);
        }
#endif
    }

    void IocpPoller::updateEvent(net::SocketHeader* header, OperationType op)
    {
#if UVENT_DEBUG
        spdlog::trace("IocpPoller::updateEvent: header={}, fd={}, op_mask={}",
                      static_cast<void*>(header),
                      header ? static_cast<std::uint64_t>(header->fd) : 0ull,
                      (int)op);
#endif
    }

    void IocpPoller::removeEvent(net::SocketHeader* header, OperationType)
    {
#if UVENT_DEBUG
        spdlog::debug("IocpPoller::removeEvent: header={}, fd={}",
                      static_cast<void*>(header),
                      header ? static_cast<std::uint64_t>(header->fd) : 0ull);
#endif
        if (!header)
            return;

        if (header->fd != INVALID_FD)
        {
#if UVENT_DEBUG
            spdlog::info("IocpPoller::removeEvent: closesocket fd={}",
                         (std::uint64_t)header->fd);
#endif
            ::closesocket(header->fd);
            header->fd = INVALID_FD;
        }
    }

    void IocpPoller::wake() noexcept
    {
        if (!this->iocp_handle)
            return;
        if (!this->wake_pending.exchange(true, std::memory_order_acq_rel))
            ::PostQueuedCompletionStatus(this->iocp_handle, 0, reinterpret_cast<ULONG_PTR>(this), nullptr);
    }

    bool IocpPoller::poll(int timeout_ms)
    {
        DWORD timeout = (timeout_ms < 0) ? 0 : static_cast<DWORD>(timeout_ms);
        ULONG n = 0;

#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.enter();
#endif

        BOOL ok = ::GetQueuedCompletionStatusEx(
            this->iocp_handle,
            this->events.data(),
            static_cast<ULONG>(this->events.size()),
            &n,
            timeout,
            FALSE);

        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT)
            {
#if UVENT_DEBUG
                // spdlog::trace("IocpPoller::poll: WAIT_TIMEOUT");
#endif
#ifndef UVENT_ENABLE_REUSEADDR
                system::this_thread::detail::g_qsbr.leave();
#endif
                return false;
            }
#if UVENT_DEBUG
            spdlog::debug("IocpPoller::poll: GetQueuedCompletionStatusEx error={} n={}", err, n);
#endif
        }

        for (ULONG i = 0; i < n; ++i)
        {
            auto& e = this->events[i];
            if (e.lpCompletionKey == reinterpret_cast<ULONG_PTR>(this) && e.lpOverlapped == nullptr)
            {
                this->wake_pending.store(false, std::memory_order_release);
                continue;
            }
            auto* header = reinterpret_cast<net::SocketHeader*>(e.lpCompletionKey);
            auto* ov = reinterpret_cast<net::IocpOverlapped*>(e.lpOverlapped);

#if UVENT_DEBUG
            spdlog::trace("IocpPoller::poll: event[{}]: header={}, ov={}, bytes={}, status={}",
                          i,
                          static_cast<void*>(header),
                          static_cast<void*>(ov),
                          e.dwNumberOfBytesTransferred,
                          e.Internal);
#endif

            if (!header || !ov)
                continue;

#ifndef UVENT_ENABLE_REUSEADDR
            if (header->is_busy_now() || header->is_disconnected_now()) {
#if UVENT_DEBUG
            spdlog::trace("IocpPoller::poll: skip event, busy={} disconnected={} fd={}",
                          header->is_busy_now(),
                          header->is_disconnected_now(),
                          (std::uint64_t) header->fd);
#endif
            continue;
            }

            header->try_mark_busy();
#endif

            bool hup = false;
            DWORD transferred = e.dwNumberOfBytesTransferred;
            ov->bytes_transferred = transferred;

            if (transferred == 0 &&
                (ov->op == net::IocpOp::READ || ov->op == net::IocpOp::WRITE))
            {
                hup = true;
                header->mark_disconnected();
#if UVENT_DEBUG
                spdlog::info("IocpPoller::poll: HUP detected fd={} op={}",
                             (std::uint64_t)header->fd,
                             (int)ov->op);
#endif
            }

            if (ov->op == net::IocpOp::READ || ov->op == net::IocpOp::ACCEPT)
            {
#if UVENT_DEBUG
                spdlog::info("IOCP READ/ACCEPT fd={} op={} bytes={}",
                             (std::uint64_t)header->fd,
                             (int)ov->op,
                             transferred);
#endif
                if (auto c = header->fire_read())
                {
#if UVENT_DEBUG
                    spdlog::trace("IocpPoller::poll: enqueue FIRST continuation fd={}",
                                  (std::uint64_t)header->fd);
#endif
                    system::resume_waiter(c);
                }
            }
            else if (ov->op == net::IocpOp::WRITE || ov->op == net::IocpOp::CONNECT)
            {
#if UVENT_DEBUG
                spdlog::info("IOCP WRITE/CONNECT fd={} op={} bytes={}",
                             (std::uint64_t)header->fd,
                             (int)ov->op,
                             transferred);
#endif
                if (header->socket_info &
                    static_cast<uint8_t>(net::AdditionalState::CONNECTION_PENDING))
                {
                    int err_code = 0;
                    int optlen = sizeof(err_code);
                    ::getsockopt(header->fd,
                                 SOL_SOCKET,
                                 SO_ERROR,
                                 reinterpret_cast<char*>(&err_code),
                                 &optlen);
                    header->socket_info &=
                        ~static_cast<uint8_t>(net::AdditionalState::CONNECTION_PENDING);
#if UVENT_DEBUG
                    spdlog::debug("IocpPoller::poll: connect completion fd={} err_code={}",
                                  (std::uint64_t)header->fd,
                                  err_code);
#endif
                    if (err_code != 0)
                        header->socket_info |= static_cast<uint8_t>(net::AdditionalState::CONNECTION_FAILED);
                }

                if (auto c = header->fire_write())
                {
#if UVENT_DEBUG
                    spdlog::trace("IocpPoller::poll: enqueue SECOND continuation fd={}",
                                  (std::uint64_t)header->fd);
#endif
                    system::resume_waiter(c);
                }
            }

            if (hup)
            {
                this->removeEvent(header, ALL);
#if UVENT_DEBUG
                spdlog::debug("Socket hup/err fd={}", (std::uint64_t)header->fd);
#endif
            }
        }

        if (n == static_cast<ULONG>(this->events.size()))
        {
            this->events.resize(this->events.size() << 1);
#if UVENT_DEBUG
            spdlog::debug("IocpPoller::poll: events buffer grown to {}", this->events.size());
#endif
        }

#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.leave();
#endif

#if UVENT_DEBUG
        spdlog::trace("IocpPoller::poll: leave, n={}", n);
#endif
        return n > 0;
    }

    bool IocpPoller::try_lock()
    {
        if (this->lock.try_acquire())
        {
            this->is_locked.store(true, std::memory_order_release);
#if UVENT_DEBUG
            spdlog::trace("IocpPoller::try_lock: success");
#endif
            return true;
        }
#if UVENT_DEBUG
        spdlog::trace("IocpPoller::try_lock: fail");
#endif
        return false;
    }

    void IocpPoller::unlock()
    {
#if UVENT_DEBUG
        spdlog::trace("IocpPoller::unlock");
#endif
        this->is_locked.store(false, std::memory_order_release);
        this->lock.release();
    }

    void IocpPoller::deregisterEvent(net::SocketHeader* header) const
    {
    }

    void IocpPoller::lock_poll(int timeout_ms)
    {
#if UVENT_DEBUG
        spdlog::trace("IocpPoller::lock_poll: timeout_ms={}", timeout_ms);
#endif
        this->lock.acquire();
        this->is_locked.store(true, std::memory_order_release);
        this->poll(timeout_ms);
        this->unlock();
    }
} // namespace usub::uvent::core
