//
// Created by kirill on 11/16/24.
//

#ifndef UVENT_KQUEUEPOLLER_H
#define UVENT_KQUEUEPOLLER_H

#include <mutex>
#include <csignal>
#include <utility>
#include <vector>
#include <system_error>

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include "uvent/utils/timer/TimerWheel.h"
#include "EventSource.h"
#include "PollerBase.h"
#include "uvent/tasks/AwaitableFrame.h"

namespace usub::uvent::core
{
    class KQueuePoller
    {
    public:
        explicit KQueuePoller(utils::TimerWheel& wheel);

        ~KQueuePoller() = default;

        void addEvent(net::SocketHeader* header, OperationType initialState);

        void updateEvent(net::SocketHeader* header, OperationType initialState);

        void removeEvent(net::SocketHeader* header, OperationType op);

        /// Watch a non-socket descriptor (see EventSource). `ops` selects READ / WRITE / ALL.
        void addSource(EventSource* src, OperationType ops);

        /// Stop watching; does not close `src->fd`.
        void removeSource(EventSource* src);

        bool poll(int timeout_ms);

        bool try_lock();

        void unlock();

        void lock_poll(int timeout_ms);

        int get_poll_fd() const;

        void deregisterEvent(net::SocketHeader* header) const;

        void wake() noexcept;

    private:
        static constexpr uintptr_t kWakeIdent = 0;

        inline void enable_read(net::SocketHeader* h, bool enable, bool clear_edge) const
        {
            uint16_t flags = (enable ? (EV_ADD | EV_ENABLE) : (EV_ADD | EV_DISABLE));
            if (clear_edge) flags |= EV_CLEAR;
            struct kevent ev{};
            EV_SET(&ev, h->fd, EVFILT_READ, flags, 0, 0, h);
            if (kevent(this->poll_fd, &ev, 1, nullptr, 0, nullptr) == -1)
                throw std::system_error(errno, std::generic_category(), "kevent(change)");
        }

        inline void enable_write(net::SocketHeader* h, bool enable, bool clear_edge) const
        {
            uint16_t flags = (enable ? (EV_ADD | EV_ENABLE) : (EV_ADD | EV_DISABLE));
            if (clear_edge) flags |= EV_CLEAR;
            struct kevent ev{};
            EV_SET(&ev, h->fd, EVFILT_WRITE, flags, 0, 0, h);
            if (kevent(this->poll_fd, &ev, 1, nullptr, 0, nullptr) == -1)
                throw std::system_error(errno, std::generic_category(), "kevent(change)");
        }

    private:
        std::binary_semaphore lock{1};
        int poll_fd{-1};
        uint64_t timeoutDuration_ms{5000};
        std::atomic_bool is_locked{false};
        std::atomic_bool wake_pending{false};

    private:
        /// events returned by kevent
        std::vector<struct kevent> events;
        /// mask for signals (держим для совместимости, kqueue сам по себе их не фильтрует)
        sigset_t sigmask{};
        /// timers storage
        utils::TimerWheel& wheel;
    };
} // namespace usub::uvent::core

#endif // UVENT_KQUEUEPOLLER_H
