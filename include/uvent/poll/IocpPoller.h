#ifndef UVENT_IOCPPOLLER_H
#define UVENT_IOCPPOLLER_H

#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#endif

#include "uvent/system/Defines.h"
#include "uvent/poll/EventSource.h"
#include "uvent/poll/PollerBase.h"
#include "uvent/utils/timer/TimerWheel.h"
#include "uvent/net/SocketMetadata.h"

namespace usub::uvent::core
{
    class IocpPoller
    {
    public:
        explicit IocpPoller(utils::TimerWheel& wheel);

        ~IocpPoller();

        void addEvent(net::SocketHeader* header, OperationType op);

        void updateEvent(net::SocketHeader* header, OperationType op);

        void removeEvent(net::SocketHeader* header, OperationType op);

        void deregisterEvent(net::SocketHeader* header) const;

        /// No descriptor to watch on IOCP: registration is a no-op, events arrive through post().
        void addSource(EventSource*, OperationType) {}

        void removeSource(EventSource*) {}

        /// Deliver `ready` to `src->on_ready` on this poller's worker. Safe from any thread.
        void post(EventSource* src, uint32_t ready) noexcept;

        bool poll(int timeout_ms);

        bool try_lock();

        void unlock();

        void lock_poll(int timeout_ms);

        void wake() noexcept;

    private:
        std::binary_semaphore lock{1};
        std::atomic_bool is_locked{false};
        std::atomic_bool wake_pending{false};

    private:
        HANDLE iocp_handle{nullptr};
        utils::TimerWheel& wheel;
        std::vector<OVERLAPPED_ENTRY> events;
    };
} // namespace usub::uvent::core

#endif // UVENT_IOCPPOLLER_H
