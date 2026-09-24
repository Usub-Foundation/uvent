#ifndef UVENT_TASKS_TASK_H
#define UVENT_TASKS_TASK_H

#include <coroutine>
#include <type_traits>
#include <utility>

#include "uvent/sync/AsyncCancellation.h"
#include "uvent/sync/SyncCommon.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/AwaitableFrame.h"
#include "uvent/tasks/TaskState.h"

namespace usub::uvent::task
{
    template <class V>
    class JoinHandle
    {
        TaskState<V>* s_{nullptr};

    public:
        JoinHandle() = default;

        explicit JoinHandle(TaskState<V>* s) noexcept : s_(s) {}

        JoinHandle(const JoinHandle&) = delete;
        JoinHandle& operator=(const JoinHandle&) = delete;

        JoinHandle(JoinHandle&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }

        JoinHandle& operator=(JoinHandle&& o) noexcept
        {
            if (this != &o)
            {
                if (this->s_)
                    this->s_->release();
                this->s_ = o.s_;
                o.s_ = nullptr;
            }
            return *this;
        }

        ~JoinHandle()
        {
            if (this->s_)
                this->s_->release();
        }

        [[nodiscard]] bool valid() const noexcept { return this->s_ != nullptr; }

        [[nodiscard]] bool done() const noexcept { return this->s_ && this->s_->done(); }

        void cancel() noexcept
        {
            if (this->s_)
                this->s_->request_cancel();
        }

        [[nodiscard]] sync::CancellationToken token() const noexcept { return sync::CancellationToken(this->s_); }

        void detach() noexcept
        {
            if (this->s_)
            {
                this->s_->release();
                this->s_ = nullptr;
            }
        }

        struct JoinAwaiter
        {
            TaskState<V>* s;
            sync::Waiter node{};

            bool await_ready() const noexcept { return s->done(); }

            bool await_suspend(std::coroutine_handle<> h) noexcept
            {
                this->node.reset(h, sync::detail::current_thread_id());
                s->done_waiters.lock();
                s->done_waiters.push_locked(&this->node);
                s->done_waiters.unlock();
                if (s->status.load(std::memory_order_seq_cst) != 0)
                {
                    if (s->done_waiters.remove(&this->node))
                        return false;
                }
                return true;
            }

            V await_resume() { return s->take_value(); }
        };

        [[nodiscard]] JoinAwaiter operator co_await() const noexcept { return JoinAwaiter{this->s_}; }
    };

    namespace detail
    {
        inline int default_spawn_tid() noexcept { return system::this_thread::detail::t_id; }
#ifdef UVENT_RUNTIME_DRAIN
        /// \brief Registers a freshly spawned task with the current worker (or the
        /// external list of worker 0) so Uvent::stop() can cancel and drain it.
        void register_task(TaskStateBase* ts) noexcept;
#endif
    } // namespace detail

    template <class V, class F>
    JoinHandle<V> spawn_under(sync::CancelState* parent, int tid, Awaitable<V, F> aw)
    {
        auto* pr = aw.get_promise();
        auto* ts = new TaskState<V>();
        ts->root = pr->get_coroutine_handle();
        ts->owner_tid.store(tid, std::memory_order_relaxed);
        ts->store_result = +[](TaskStateBase* s, uvent::detail::AwaitableFrameBase* f)
        {
            auto* st = static_cast<TaskState<V>*>(s);
            auto* fr = static_cast<F*>(f);
            try
            {
                if constexpr (std::is_void_v<V>)
                    fr->get();
                else
                    st->set_value(fr->get());
            }
            catch (...)
            {
                st->exc = std::current_exception();
            }
        };
        if (parent)
            sync::CancelState::link(parent, ts);
        ts->retain_live_chain();
        ts->add_ref();
#ifdef UVENT_RUNTIME_DRAIN
        detail::register_task(ts);
#endif
        auto* base = static_cast<uvent::detail::AwaitableFrameBase*>(pr);
        base->set_cancel_state(ts);
        base->set_task_state(ts);
        if (tid >= 0)
            system::co_spawn_static(ts->root, tid);
        else
            system::co_spawn(ts->root);
        return JoinHandle<V>(ts);
    }

    template <class V, class F>
    JoinHandle<V> spawn(Awaitable<V, F> aw, int tid)
    {
        return spawn_under(system::this_thread::detail::current_cancel_ptr(), tid, std::move(aw));
    }

    template <class V, class F>
    JoinHandle<V> spawn(Awaitable<V, F> aw)
    {
        return spawn(std::move(aw), detail::default_spawn_tid());
    }

    class TaskScope
    {
        sync::CancelState* s_;

    public:
        TaskScope() : s_(new sync::CancelState())
        {
            if (auto* cur = system::this_thread::detail::current_cancel_ptr())
                sync::CancelState::link(cur, this->s_);
        }

        explicit TaskScope(const sync::CancellationToken& parent) : s_(new sync::CancelState())
        {
            if (parent.state())
                sync::CancelState::link(parent.state(), this->s_);
        }

        explicit TaskScope(std::nullptr_t) : s_(new sync::CancelState()) {}

        TaskScope(const TaskScope&) = delete;
        TaskScope& operator=(const TaskScope&) = delete;
        TaskScope(TaskScope&&) = delete;
        TaskScope& operator=(TaskScope&&) = delete;

        ~TaskScope()
        {
            this->s_->request_cancel();
            this->s_->release();
        }

        template <class V, class F>
        JoinHandle<V> spawn(Awaitable<V, F> aw)
        {
            return spawn_under(this->s_, detail::default_spawn_tid(), std::move(aw));
        }

        template <class V, class F>
        JoinHandle<V> spawn(Awaitable<V, F> aw, int tid)
        {
            return spawn_under(this->s_, tid, std::move(aw));
        }

        void cancel() noexcept { this->s_->request_cancel(); }

        [[nodiscard]] bool cancel_requested() const noexcept { return this->s_->stop_requested(); }

        [[nodiscard]] sync::CancellationToken token() const noexcept { return sync::CancellationToken(this->s_); }

        [[nodiscard]] std::size_t live_tasks() const noexcept
        {
            return this->s_->live_tasks.load(std::memory_order_acquire);
        }

        struct JoinAwaiter
        {
            sync::CancelState* s;
            sync::Waiter node{};

            bool await_ready() const noexcept { return s->live_tasks.load(std::memory_order_acquire) == 0; }

            bool await_suspend(std::coroutine_handle<> h) noexcept
            {
                this->node.reset(h, sync::detail::current_thread_id());
                s->join_waiters.lock();
                s->join_waiters.push_locked(&this->node);
                s->join_waiters.unlock();
                if (s->live_tasks.load(std::memory_order_seq_cst) == 0)
                {
                    if (s->join_waiters.remove(&this->node))
                        return false;
                }
                return true;
            }

            void await_resume() const noexcept {}
        };

        [[nodiscard]] JoinAwaiter join() noexcept { return JoinAwaiter{this->s_}; }

        Awaitable<void> cancel_and_join()
        {
            this->cancel();
            co_await this->join();
        }
    };
} // namespace usub::uvent::task

#endif // UVENT_TASKS_TASK_H
