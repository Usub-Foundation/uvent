#ifndef UVENT_SYNC_CANCELLATION_H
#define UVENT_SYNC_CANCELLATION_H

#include <coroutine>
#include <variant>

#include "uvent/sync/CancelState.h"
#include "uvent/sync/SyncCommon.h"
#include "uvent/sync/Wait.h"

namespace usub::uvent::sync
{

    struct CancelOp
    {
        CancelState* s;

        using result_type = std::monostate;

        static const char* wait_reason() noexcept { return "cancel.wait"; }

        bool try_complete(std::monostate&) const noexcept { return s->requested.load(std::memory_order_acquire); }

        bool attach(Waiter* w) noexcept
        {
            s->cancel_waiters.lock();
            s->cancel_waiters.push_locked(w);
            s->cancel_waiters.unlock();
            detail::notify_fence();
            return !s->requested.load(std::memory_order_seq_cst);
        }

        bool detach(Waiter* w) noexcept { return s->cancel_waiters.remove(w); }

        void finalize() noexcept {}
    };

    class CancellationToken
    {
        CancelState* s_{nullptr};

    public:
        CancellationToken() = default;

        explicit CancellationToken(CancelState* s, bool add_ref = true) noexcept : s_(s)
        {
            if (s_ && add_ref)
                s_->add_ref();
        }

        CancellationToken(const CancellationToken& o) noexcept : s_(o.s_)
        {
            if (s_)
                s_->add_ref();
        }

        CancellationToken(CancellationToken&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }

        CancellationToken& operator=(const CancellationToken& o) noexcept
        {
            if (this != &o)
            {
                if (o.s_)
                    o.s_->add_ref();
                if (s_)
                    s_->release();
                s_ = o.s_;
            }
            return *this;
        }

        CancellationToken& operator=(CancellationToken&& o) noexcept
        {
            if (this != &o)
            {
                if (s_)
                    s_->release();
                s_ = o.s_;
                o.s_ = nullptr;
            }
            return *this;
        }

        ~CancellationToken()
        {
            if (s_)
                s_->release();
        }

        [[nodiscard]] bool valid() const noexcept { return s_ != nullptr; }

        [[nodiscard]] bool stop_requested() const noexcept
        {
            return s_ && s_->requested.load(std::memory_order_acquire);
        }

        [[nodiscard]] CancelState* state() const noexcept { return s_; }

        [[nodiscard]] CancelOp on_cancel_op() const noexcept { return CancelOp{s_}; }

        struct OnCancelAwaiter
        {
            CancelOp op;
            OpWait<CancelOp> wait{&op};

            bool await_ready() noexcept
            {
                std::monostate m;
                return op.try_complete(m);
            }

            bool await_suspend(std::coroutine_handle<> h) noexcept { return wait.await_suspend(h); }

            bool await_resume() noexcept
            {
                std::monostate m;
                if (op.try_complete(m))
                    return true;
                return false;
            }
        };

        [[nodiscard]] OnCancelAwaiter on_cancel() const noexcept { return OnCancelAwaiter{CancelOp{s_}}; }
    };

    class CancellationSource
    {
        CancelState* s_{nullptr};

    public:
        CancellationSource() : s_(new CancelState()) {}

        explicit CancellationSource(const CancellationToken& parent) : s_(new CancelState())
        {
            if (parent.state())
                CancelState::link(parent.state(), s_);
        }

        CancellationSource(const CancellationSource&) = delete;
        CancellationSource& operator=(const CancellationSource&) = delete;

        CancellationSource(CancellationSource&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }

        CancellationSource& operator=(CancellationSource&& o) noexcept
        {
            if (this != &o)
            {
                if (s_)
                    s_->release();
                s_ = o.s_;
                o.s_ = nullptr;
            }
            return *this;
        }

        ~CancellationSource()
        {
            if (s_)
                s_->release();
        }

        [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken(s_); }

        void request_cancel() noexcept
        {
            if (s_)
                s_->request_cancel();
        }
    };

    inline CancellationToken current_token() noexcept
    {
        return CancellationToken(system::this_thread::detail::current_cancel_ptr());
    }

} // namespace usub::uvent::sync

#endif // UVENT_SYNC_CANCELLATION_H
