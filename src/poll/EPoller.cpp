//
// Created by kirill on 11/15/24.
//

#include "uvent/poll/EPoller.h"

#include "uvent/net/Socket.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

namespace
{
    // A socket whose epoll registration failed never receives events and hangs
    // until its timeout, so a failed epoll_ctl must not pass silently (it used
    // to for ADD, and for MOD outside UVENT_DEBUG). ENOENT/EBADF/ENOTSOCK mean
    // the fd was closed concurrently: an ordinary race, not an error. Anything
    // else (EEXIST, ENOMEM, ENOSPC = max_user_watches, EPERM) is reported:
    // thrown in debug builds, one stderr line in release builds.
    void check_epoll_ctl(int result, const char* op, int fd)
    {
        if (result == 0) [[likely]]
            return;
        const int err = errno;
        if (err == ENOENT || err == EBADF || err == ENOTSOCK)
            return;
#if UVENT_DEBUG
        throw std::system_error(err, std::generic_category(), std::string("epoll_ctl[") + op + "] (EPoller)");
#else
        std::fprintf(stderr, "uvent: epoll_ctl[%s] fd=%d failed: %s\n", op, fd, std::strerror(err));
#endif
    }
} // namespace

namespace usub::uvent::core
{
    EPoller::EPoller(utils::TimerWheel& wheel) : wheel(wheel)
    {
        this->poll_fd = epoll_create1(EPOLL_CLOEXEC);
        this->events.resize(4096);

        this->wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (this->wake_fd >= 0)
        {
            struct epoll_event wev{};
            wev.data.ptr = this;
            wev.events = EPOLLIN | EPOLLET;
            epoll_ctl(this->poll_fd, EPOLL_CTL_ADD, this->wake_fd, &wev);
        }
    }

    EPoller::~EPoller()
    {
        if (this->wake_fd >= 0)
            ::close(this->wake_fd);
    }

    void EPoller::wake() noexcept
    {
        if (this->wake_fd < 0)
            return;
        if (!this->wake_pending.exchange(true, std::memory_order_acq_rel))
        {
            uint64_t one = 1;
            [[maybe_unused]] ssize_t r = ::write(this->wake_fd, &one, sizeof(one));
        }
    }

    void EPoller::addEvent(net::SocketHeader* header, OperationType initialState)
    {
        struct epoll_event event{};
        event.data.ptr = reinterpret_cast<void*>(header);
        event.events = 0;
        event.events = (EPOLLIN | EPOLLOUT | EPOLLET);

#if UVENT_DEBUG
        spdlog::info("Socket added: fd={} et={} in={} out={}", header->fd, bool(event.events & EPOLLET),
                     bool(event.events & EPOLLIN), bool(event.events & EPOLLOUT));
#endif

        check_epoll_ctl(epoll_ctl(this->poll_fd, EPOLL_CTL_ADD, header->fd, &event), "EPOLL_CTL_ADD", header->fd);
#ifdef UVENT_SOCKET_OWNER_FORWARDING
        // The poller is thread_local: whoever registers the fd owns the header
        // (its timer lives in this thread's wheel, its delete goes through this
        // thread's q_sh). Foreign-thread teardown is forwarded here, see
        // ThreadLocalStorage::push_socket_op.
        header->owner_tid = system::this_thread::detail::t_id;
#endif
    }


    void EPoller::updateEvent(net::SocketHeader* header, OperationType initialState)
    {
        struct epoll_event event{};
        event.data.ptr = reinterpret_cast<void*>(header);
        event.events = 0;

        if (header->is_tcp() && !header->is_passive())
            event.events = (EPOLLIN | EPOLLOUT | EPOLLET);
        else
        {
            event.events = EPOLLIN | EPOLLET;
#ifndef UVENT_ENABLE_REUSEADDR
            if (header->is_tcp() && header->is_passive())
                system::this_thread::detail::is_started.store(true, std::memory_order_relaxed);
#else
            if (header->is_tcp() && header->is_passive())
                system::this_thread::detail::is_started = true;
#endif
        }

#if UVENT_DEBUG
        spdlog::info("Updating socket #{} READ: {}, WRITE: {}", header->fd, static_cast<bool>(event.events & EPOLLIN),
                     static_cast<bool>(event.events & EPOLLOUT));
        spdlog::info("Socket #{} updated with state: {}, read state: {}, write state: {}", header->fd,
                     static_cast<int>(initialState), header->is_reading_now(), header->is_writing_now());
#endif

        check_epoll_ctl(epoll_ctl(this->poll_fd, EPOLL_CTL_MOD, header->fd, &event), "EPOLL_CTL_MOD", header->fd);
    }

    void EPoller::removeEvent(net::SocketHeader* header)
    {
#if UVENT_DEBUG
        spdlog::info("Socket removed: {}", header->fd);
#endif
        using namespace usub::utils::sync::refc;

        epoll_ctl(this->poll_fd, EPOLL_CTL_DEL, header->fd, nullptr);
        ::close(header->fd);
        header->fd = -1;
    }

    void EPoller::addSource(EventSource* src, OperationType ops)
    {
        struct epoll_event event{};
        event.data.ptr = src->tagged();
        if (ops & READ)
            event.events |= EPOLLIN | EPOLLRDHUP;
        if (ops & WRITE)
            event.events |= EPOLLOUT;
        if (src->edge)
            event.events |= EPOLLET;
        check_epoll_ctl(epoll_ctl(this->poll_fd, EPOLL_CTL_ADD, src->fd, &event), "EPOLL_CTL_ADD(source)", src->fd);
    }

    void EPoller::removeSource(EventSource* src)
    {
        epoll_ctl(this->poll_fd, EPOLL_CTL_DEL, src->fd, nullptr);
    }

    bool EPoller::poll(int timeout)
    {
        int n = ::epoll_wait(this->poll_fd, this->events.data(), static_cast<int>(this->events.size()), timeout);
#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.enter();
#endif
#if UVENT_DEBUG
        if (n < 0 && errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "epoll_wait");
#endif
        for (int i = 0; i < n; i++)
        {
            auto& event = this->events[i];
            if (event.data.ptr == static_cast<void*>(this))
            {
                // Drain the eventfd BEFORE clearing the flag: a wake() landing between a clear and the read
                // would set the flag, write, and immediately have its count swallowed by this read – leaving
                // the flag set with an empty eventfd, so every later wake() is skipped (lost wakeup until the
                // next timer). With read-then-clear a racing wake() at worst costs one spurious poll return.
                uint64_t v;
                [[maybe_unused]] ssize_t r = ::read(this->wake_fd, &v, sizeof(v));
                this->wake_pending.store(false, std::memory_order_release);
                continue;
            }
            if (EventSource::is_tagged(event.data.ptr)) [[unlikely]]
            {
                auto* src = EventSource::untag(event.data.ptr);
                uint32_t ready = 0;
                if (event.events & EPOLLIN)
                    ready |= EventSource::READABLE;
                if (event.events & EPOLLOUT)
                    ready |= EventSource::WRITABLE;
                if (event.events & (EPOLLHUP | EPOLLRDHUP))
                    ready |= EventSource::HUP;
                if (event.events & EPOLLERR)
                    ready |= EventSource::ERR;
                src->on_ready(src, ready);
                continue;
            }
            auto* sock = static_cast<net::SocketHeader*>(event.data.ptr);
#ifndef UVENT_ENABLE_REUSEADDR
            if (sock->is_busy_now() || sock->is_disconnected_now())
                continue;
#endif
            bool hup = !(sock->is_tcp() && sock->is_passive()) && (event.events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR));
            if (hup)
                sock->mark_disconnected();
#ifndef UVENT_ENABLE_REUSEADDR
            sock->try_mark_busy();
#endif
            if (event.events & EPOLLIN)
            {
#if UVENT_DEBUG
                spdlog::info("Socket #{} triggered as IN", sock->fd);
#endif
                if (auto c = sock->fire_read())
                    system::resume_waiter(c);
            }
            if (event.events & EPOLLOUT)
            {
#if UVENT_DEBUG
                spdlog::info("Socket #{} triggered as OUT", sock->fd);
#endif
                if (sock->socket_info & static_cast<uint8_t>(net::AdditionalState::CONNECTION_PENDING))
                {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                    sock->socket_info &= ~static_cast<uint8_t>(net::AdditionalState::CONNECTION_PENDING);
                    if (err != 0)
                    {
                        sock->connect_error = err;
                        sock->socket_info |= static_cast<uint8_t>(net::AdditionalState::CONNECTION_FAILED);
                    }
                }
                if (auto c = sock->fire_write())
                    system::resume_waiter(c);
            }
            if (hup)
            {
                this->removeEvent(sock);
#if UVENT_DEBUG
                spdlog::debug("Socket hup/err fd={}", sock->fd);
#endif
            }
        }
#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.leave();
#endif
        return n > 0;
    }

    bool EPoller::try_lock()
    {
        // Take the next ticket only if it is served right away, i.e. the lock is
        // free and nobody is queued ahead of us.
        uint32_t serving = this->ticket_serving.load(std::memory_order_acquire);
        if (this->ticket_next.compare_exchange_strong(serving, serving + 1, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed))
        {
            this->is_locked.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    void EPoller::unlock()
    {
        this->is_locked.store(false, std::memory_order_release);
        this->ticket_serving.fetch_add(1, std::memory_order_acq_rel);
        this->ticket_serving.notify_all();
    }

    void EPoller::lock_poll(int timeout)
    {
        const uint32_t mine = this->ticket_next.fetch_add(1, std::memory_order_acq_rel);
        for (;;)
        {
            const uint32_t serving = this->ticket_serving.load(std::memory_order_acquire);
            if (serving == mine)
                break;
            this->ticket_serving.wait(serving, std::memory_order_acquire);
        }
        this->is_locked.store(true, std::memory_order_release);
        this->poll(timeout);
        this->unlock();
    }
    void EPoller::deregisterEvent(net::SocketHeader* header) const
    {
        epoll_ctl(this->poll_fd, EPOLL_CTL_DEL, header->fd, nullptr);
    }

    int EPoller::get_poll_fd() { return this->poll_fd; }
} // namespace usub::uvent::core
