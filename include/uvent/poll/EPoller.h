//
// Created by kirill on 11/15/24.
//

#ifndef UVENT_EPOLLER_H
#define UVENT_EPOLLER_H

#include <uvent/system/Defines.h>

#include <csignal>
#include <mutex>
#include <utility>
#include "EventSource.h"
#include "PollerBase.h"
#include "uvent/tasks/AwaitableFrame.h"
#include "uvent/utils/timer/TimerWheel.h"

namespace usub::uvent::core
{
    /**
     * \brief Used on Linux systems. Wrapper over epoll.
     */
    class EPoller
    {
    public:
        explicit EPoller(utils::TimerWheel& wheel);

        ~EPoller();

        void addEvent(net::SocketHeader* header, OperationType initialState);

        void updateEvent(net::SocketHeader* header, OperationType initialState);

        void removeEvent(net::SocketHeader* header);

        /// Watch a non-socket descriptor (see EventSource). `ops` selects READ / WRITE / ALL.
        void addSource(EventSource* src, OperationType ops);

        /// Stop watching; does not close `src->fd`.
        void removeSource(EventSource* src);

        bool poll(int timeout);

        bool try_lock();

        void unlock();

        void lock_poll(int timeout);

        void deregisterEvent(net::SocketHeader* header) const;

        int get_poll_fd();

        void wake() noexcept;

    private:
        std::atomic<uint32_t> ticket_next{0};
        std::atomic<uint32_t> ticket_serving{0};
        int poll_fd{-1};
        int wake_fd{-1};
        uint64_t timeoutDuration_ms{5000};
        std::atomic_bool is_locked{false};
        std::atomic_bool wake_pending{false};

    private:
        /// @brief events returned by epoll
        std::vector<epoll_event> events;
        /// @brief used to store all timers
        utils::TimerWheel& wheel;
    };
} // namespace usub::uvent::core

#endif // UVENT_EPOLLER_H
