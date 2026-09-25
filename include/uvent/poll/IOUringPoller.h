#ifndef UVENT_IOURINGPOLLER_H
#define UVENT_IOURINGPOLLER_H

#include <atomic>
#include <csignal>
#include <semaphore>
#include <vector>
#include <coroutine>

#include <liburing.h>

#include "uvent/poll/EventSource.h"
#include "uvent/poll/PollerBase.h"
#include "uvent/poll/IOUringOps.h"
#include "uvent/system/Defines.h"
#include "uvent/utils/timer/TimerWheel.h"
#include "uvent/net/SocketMetadata.h"

namespace usub::uvent::core
{

    class IOUringPoller
    {
    public:
        explicit IOUringPoller(utils::TimerWheel& wheel);
        ~IOUringPoller();

        void addEvent(net::SocketHeader* header, OperationType op);
        void updateEvent(net::SocketHeader* header, OperationType op);
        void removeEvent(net::SocketHeader* header);

        /// Watch a non-socket descriptor with a multishot POLL_ADD (see EventSource).
        void addSource(EventSource* src, OperationType ops);

        /// Cancel the poll; the source must outlive the cancellation CQE (next poll()).
        void removeSource(EventSource* src);

        bool poll(int timeout_ms);

        bool try_lock();
        void unlock();
        void lock_poll(int timeout_ms);

        void submit_recv(detail::RecvOp* op, int fd);
        void submit_send(detail::SendOp* op, int fd);
        void submit_accept(detail::AcceptOp* op, int fd);
        void submit_accept_multishot(detail::MultishotAcceptOp* op, int fd);
        void submit_recv_multishot(detail::MultishotRecvOp* op, int fd);
        void submit_sendfile(detail::SendFileOp* op, int out_fd);
        void submit_connect(detail::ConnectOp* op, int fd);

        [[nodiscard]] bool has_buf_ring() const noexcept { return this->buf_ring_ != nullptr; }
        [[nodiscard]] uint8_t* buf_base(uint16_t bid) noexcept
        {
            return this->buf_pool_ + static_cast<size_t>(bid) * kBufSize;
        }
        void recycle_buf(uint16_t bid) noexcept;

        void submit_cancel(void* target_op);

        // File ops (uvent/fs): plain one-shot SQEs, completion handled by the generic tail of handle_cqe.
        void submit_file_read(detail::IoOpBase* op, int fd, void* buf, unsigned len, uint64_t off);
        void submit_file_write(detail::IoOpBase* op, int fd, const void* buf, unsigned len, uint64_t off);
        void submit_file_fsync(detail::IoOpBase* op, int fd, bool datasync);

        void deregisterEvent(net::SocketHeader* header) const;

        void wake() noexcept;

    private:
        void handle_cqe(struct io_uring_cqe* cqe);

        void arm_wake();

        void arm_source(EventSource* src);

        struct io_uring_sqe* get_sqe_flush() noexcept;

    private:
        utils::TimerWheel& wheel;

        std::binary_semaphore lock{1};
        std::atomic_bool is_locked{false};

        struct io_uring ring{};
        unsigned int ring_entries{1024};

        static constexpr unsigned kBufCount = 256; // степень двойки
        static constexpr unsigned kBufSize = 16384;
        static constexpr int kBgid = 1;
        struct io_uring_buf_ring* buf_ring_{nullptr};
        uint8_t* buf_pool_{nullptr};

        // Wake-канал (см. wake()).
        int wake_fd_{-1};
        bool wake_armed_{false};
        std::atomic_bool wake_pending_{false};
        detail::IoOpBase wake_op_{};

        sigset_t sigmask{};
    };
} // namespace usub::uvent::core

#endif // UVENT_IOURINGPOLLER_H
