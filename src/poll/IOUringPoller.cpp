#include "uvent/poll/IOUringPoller.h"

#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

#include "uvent/net/Socket.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/AwaitableFrame.h"

namespace usub::uvent::core
{
    using namespace usub::uvent::core::detail;

    namespace
    {
        void drop_recv_ref(net::SocketHeader* h)
        {
            auto s = net::Socket<net::Proto::TCP, net::Role::ACTIVE>::from_existing(h);
        }

        void fail_op_busy(IoOpBase* op)
        {
            op->res = -EBUSY;
            op->err = EBUSY;
            op->completed = true;
            usub::uvent::system::resume_waiter(op->coro);
        }
    } // namespace

    IOUringPoller::IOUringPoller(utils::TimerWheel& wheel_) : wheel(wheel_)
    {
        std::memset(&this->ring, 0, sizeof(this->ring));

        const unsigned flag_sets[] = {
            IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
            IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN,
            IORING_SETUP_COOP_TASKRUN,
            0,
        };

        int ret = -EINVAL;
        for (unsigned flags : flag_sets)
        {
            struct io_uring_params params;
            std::memset(&params, 0, sizeof(params));
            params.flags = flags;

            ret = ::io_uring_queue_init_params(this->ring_entries, &this->ring, &params);
            if (ret == 0)
                break;
        }
        if (ret < 0)
        {
#if UVENT_DEBUG
            throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init_params failed");
#else
            std::abort();
#endif
        }

        sigemptyset(&this->sigmask);

        int bret = 0;
        this->buf_ring_ = ::io_uring_setup_buf_ring(&this->ring, kBufCount, kBgid, 0, &bret);
        if (this->buf_ring_)
        {
            this->buf_pool_ = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(kBufCount) * kBufSize));
            if (!this->buf_pool_)
            {
                ::io_uring_free_buf_ring(&this->ring, this->buf_ring_, kBufCount, kBgid);
                this->buf_ring_ = nullptr;
            }
            else
            {
                for (unsigned i = 0; i < kBufCount; ++i)
                    ::io_uring_buf_ring_add(this->buf_ring_, this->buf_pool_ + static_cast<size_t>(i) * kBufSize,
                                            kBufSize, static_cast<unsigned short>(i),
                                            ::io_uring_buf_ring_mask(kBufCount), static_cast<int>(i));
                ::io_uring_buf_ring_advance(this->buf_ring_, kBufCount);
            }
        }

        this->wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        this->wake_op_.kind = IoOpKind::WakeFd;

#if UVENT_DEBUG
        spdlog::info("IOUringPoller ctor: entries={} buf_ring={}", this->ring_entries, (void*)this->buf_ring_);
#endif
    }

    IOUringPoller::~IOUringPoller()
    {
        if (this->buf_ring_)
        {
            ::io_uring_free_buf_ring(&this->ring, this->buf_ring_, kBufCount, kBgid);
            std::free(this->buf_pool_);
        }
        if (this->wake_fd_ >= 0)
            ::close(this->wake_fd_);
        ::io_uring_queue_exit(&this->ring);
    }

    void IOUringPoller::wake() noexcept
    {
        if (this->wake_fd_ < 0)
            return;
        if (!this->wake_pending_.exchange(true, std::memory_order_acq_rel))
        {
            uint64_t one = 1;
            [[maybe_unused]] ssize_t r = ::write(this->wake_fd_, &one, sizeof(one));
        }
    }

    void IOUringPoller::arm_wake()
    {
        if (this->wake_fd_ < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        ::io_uring_prep_poll_multishot(sqe, this->wake_fd_, POLLIN);
        ::io_uring_sqe_set_data(sqe, &this->wake_op_);
        this->wake_armed_ = true;
    }

    void IOUringPoller::recycle_buf(uint16_t bid) noexcept
    {
        ::io_uring_buf_ring_add(this->buf_ring_, this->buf_pool_ + static_cast<size_t>(bid) * kBufSize, kBufSize, bid,
                                ::io_uring_buf_ring_mask(kBufCount), 0);
        ::io_uring_buf_ring_advance(this->buf_ring_, 1);
    }

    struct io_uring_sqe* IOUringPoller::get_sqe_flush() noexcept
    {
        auto* sqe = ::io_uring_get_sqe(&this->ring);
        if (!sqe)
        {
            ::io_uring_submit(&this->ring);
            sqe = ::io_uring_get_sqe(&this->ring);
        }
        return sqe;
    }

    void IOUringPoller::submit_recv_multishot(MultishotRecvOp* op, int fd)
    {
        if (!op || fd < 0 || !this->buf_ring_)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        op->kind = IoOpKind::RecvMultishot;
        ::io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = kBgid;
        ::io_uring_sqe_set_data(sqe, op);
        op->armed = true;
    }

    void IOUringPoller::addEvent(net::SocketHeader*, OperationType) {}

    void IOUringPoller::arm_source(EventSource* src)
    {
        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;
        ::io_uring_prep_poll_multishot(sqe, src->fd, src->poll_mask);
        ::io_uring_sqe_set_data(sqe, &src->op);
        src->armed = true;
    }

    void IOUringPoller::addSource(EventSource* src, OperationType ops)
    {
        src->op.kind = IoOpKind::EventSource;
        src->op.owner = src;
        src->poll_mask = 0;
        if (ops & READ)
            src->poll_mask |= POLLIN | POLLRDHUP;
        if (ops & WRITE)
            src->poll_mask |= POLLOUT;
        src->removed = false;
        this->arm_source(src);
    }

    void IOUringPoller::removeSource(EventSource* src)
    {
        src->removed = true;
        if (src->armed)
            this->submit_cancel(&src->op);
    }

    void IOUringPoller::updateEvent(net::SocketHeader*, OperationType) {}

    void IOUringPoller::removeEvent(net::SocketHeader*) {}

    void IOUringPoller::submit_recv(RecvOp* op, int fd)
    {
        if (!op || fd < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        ::io_uring_prep_recv(sqe, fd, op->buf, op->len, 0);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_send(SendOp* op, int fd)
    {
        if (!op || fd < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        ::io_uring_prep_send(sqe, fd, op->buf, op->len, 0);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_accept(AcceptOp* op, int fd)
    {
        if (!op || fd < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        op->addrlen = sizeof(sockaddr_storage);
        ::io_uring_prep_accept(sqe, fd, reinterpret_cast<sockaddr*>(&op->addr), &op->addrlen,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_accept_multishot(MultishotAcceptOp* op, int fd)
    {
        if (!op || fd < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        ::io_uring_prep_multishot_accept(sqe, fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        ::io_uring_sqe_set_data(sqe, op);
        op->armed = true;
    }

    void IOUringPoller::submit_sendfile(SendFileOp* op, int out_fd)
    {
        if (!op || out_fd < 0 || op->in_fd < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        io_uring_prep_splice(sqe, op->in_fd, op->offset ? *op->offset : -1, out_fd, -1, op->count, 0);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_connect(detail::ConnectOp* op, int fd)
    {
        if (!op || fd < 0)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        ::io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr*>(&op->addr), op->addrlen);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_file_read(IoOpBase* op, int fd, void* buf, unsigned len, uint64_t off)
    {
        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return fail_op_busy(op);
        op->kind = IoOpKind::File;
        ::io_uring_prep_read(sqe, fd, buf, len, off);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_file_write(IoOpBase* op, int fd, const void* buf, unsigned len, uint64_t off)
    {
        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return fail_op_busy(op);
        op->kind = IoOpKind::File;
        ::io_uring_prep_write(sqe, fd, buf, len, off);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_file_fsync(IoOpBase* op, int fd, bool datasync)
    {
        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return fail_op_busy(op);
        op->kind = IoOpKind::File;
        ::io_uring_prep_fsync(sqe, fd, datasync ? IORING_FSYNC_DATASYNC : 0);
        ::io_uring_sqe_set_data(sqe, op);
    }

    void IOUringPoller::submit_cancel(void* target_op)
    {
        if (!target_op)
            return;

        auto* sqe = this->get_sqe_flush();
        if (!sqe)
            return;

        ::io_uring_prep_cancel(sqe, target_op, 0);
        ::io_uring_sqe_set_data(sqe, nullptr);
    }

    void IOUringPoller::handle_cqe(struct io_uring_cqe* cqe)
    {
        auto* base = static_cast<IoOpBase*>(::io_uring_cqe_get_data(cqe));
        if (!base)
            return;

        if (base->kind == IoOpKind::WakeFd)
        {
            // Drain the eventfd BEFORE clearing the flag: a wake() landing between a clear and the read
            // would set the flag, write, and immediately have its count swallowed by this read – leaving
            // the flag set with an empty eventfd, so every later wake() is skipped (lost wakeup until the
            // next timer). With read-then-clear a racing wake() at worst costs one spurious poll return.
            uint64_t v;
            [[maybe_unused]] ssize_t r = ::read(this->wake_fd_, &v, sizeof(v));
            this->wake_pending_.store(false, std::memory_order_release);
            if (!(cqe->flags & IORING_CQE_F_MORE))
                this->wake_armed_ = false;
            return;
        }

        if (base->kind == IoOpKind::EventSource)
        {
            auto* src = static_cast<EventSource::Op*>(base)->owner;
            if (!(cqe->flags & IORING_CQE_F_MORE))
                src->armed = false;
            if (cqe->res < 0)
            {
                // -ECANCELED after removeSource(), or the fd went away: nothing to deliver. A poll that
                // ended for another reason while still wanted is re-armed on the next poll() below.
                if (!src->removed && cqe->res != -ECANCELED && !src->armed)
                    this->arm_source(src);
                return;
            }
            uint32_t ready = 0;
            if (cqe->res & POLLIN)
                ready |= EventSource::READABLE;
            if (cqe->res & POLLOUT)
                ready |= EventSource::WRITABLE;
            if (cqe->res & (POLLHUP | POLLRDHUP))
                ready |= EventSource::HUP;
            if (cqe->res & POLLERR)
                ready |= EventSource::ERR;
            src->on_ready(src, ready);
            if (!src->armed && !src->removed)
                this->arm_source(src); // multishot terminated (e.g. kernel without multishot poll): one-shot re-arm
            return;
        }

        if (base->kind == IoOpKind::RecvMultishot)
        {
            auto* op = static_cast<MultishotRecvOp*>(base);
            const bool was_armed = op->armed;

            if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER))
            {
                const uint16_t bid = static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
                op->push({static_cast<uint32_t>(cqe->res), 0, bid});
            }
            else
            {
                op->has_terminal = true;
                op->terminal = static_cast<int32_t>(cqe->res); // 0=EOF, <0=-errno
            }

            if (!(cqe->flags & IORING_CQE_F_MORE))
                op->armed = false;

            if (op->waiting && op->coro && !op->coro.done())
            {
                op->waiting = false;
                usub::uvent::system::resume_waiter(op->coro);
            }

            if (was_armed && !op->armed)
                drop_recv_ref(op->header);
            return;
        }

        if (base->kind == IoOpKind::AcceptMultishot)
        {
            auto* op = static_cast<MultishotAcceptOp*>(base);
            if (cqe->res >= 0)
                op->pending_fds.push_back(cqe->res);
            else
            {
                op->res = cqe->res;
                op->err = -cqe->res;
            }
            if (!(cqe->flags & IORING_CQE_F_MORE))
                op->armed = false;
            if (op->waiting && op->coro && !op->coro.done())
            {
                op->waiting = false;
                usub::uvent::system::resume_waiter(op->coro);
            }
            return;
        }

        base->res = cqe->res;
        base->err = (cqe->res < 0) ? -cqe->res : 0;
        base->completed = true;

        if (base->coro && !base->coro.done())
        {
            usub::uvent::system::resume_waiter(base->coro);
        }
    }

    bool IOUringPoller::poll(int timeout_ms)
    {
        __kernel_timespec ts{};
        __kernel_timespec* tsp = nullptr;

        if (timeout_ms >= 0)
        {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1'000'000L;
            tsp = &ts;
        }

        if (!this->wake_armed_)
            this->arm_wake();

        ::io_uring_submit(&this->ring);

        if (this->ring.flags & IORING_SETUP_DEFER_TASKRUN)
            ::io_uring_get_events(&this->ring);

#ifndef UVENT_ENABLE_REUSEADDR
        usub::uvent::system::this_thread::detail::g_qsbr.enter();
#endif

        bool any = false;
        struct io_uring_cqe* cqe = nullptr;

        int ret = ::io_uring_wait_cqe_timeout(&this->ring, &cqe, tsp);
        if (ret == -ETIME || ret == -EINTR)
        {
        }
        else if (ret < 0)
        {
#if UVENT_DEBUG
            throw std::system_error(-ret, std::generic_category(), "io_uring_wait_cqe_timeout failed");
#endif
        }
        else
        {
            this->handle_cqe(cqe);
            ::io_uring_cqe_seen(&this->ring, cqe);
            any = true;

            static constexpr unsigned MAX_BATCH = 128;
            struct io_uring_cqe* cqes[MAX_BATCH];
            unsigned count = ::io_uring_peek_batch_cqe(&this->ring, cqes, MAX_BATCH);
            for (unsigned i = 0; i < count; ++i)
            {
                this->handle_cqe(cqes[i]);
                ::io_uring_cqe_seen(&this->ring, cqes[i]);
            }
        }

#ifndef UVENT_ENABLE_REUSEADDR
        usub::uvent::system::this_thread::detail::g_qsbr.leave();
#endif

        return any;
    }

    bool IOUringPoller::try_lock()
    {
        if (this->lock.try_acquire())
        {
            this->is_locked.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    void IOUringPoller::unlock()
    {
        this->is_locked.store(false, std::memory_order_release);
        this->lock.release();
    }

    void IOUringPoller::deregisterEvent(net::SocketHeader* header) const {}

    void IOUringPoller::lock_poll(int timeout_ms)
    {
        this->lock.acquire();
        this->is_locked.store(true, std::memory_order_release);
        this->poll(timeout_ms);
        this->unlock();
    }
} // namespace usub::uvent::core
