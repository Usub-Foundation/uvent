//
// Created by kirill on 1/4/25.
//

#include "uvent/tasks/AwaitableFrame.h"
#include "uvent/system/Introspection.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/TaskState.h"

namespace usub::uvent::detail
{
    void AwaitableFrameBase::destroy(DestroyingPolicy policy)
    {
        if (policy == FORCED)
        {
            AwaitableFrameBase* current = this;
            while (current)
            {
                auto prev_handle = std::exchange(current->prev_, {});
                auto* prev = prev_handle
                    ? &std::coroutine_handle<AwaitableFrameBase>::from_address(prev_handle.address()).promise()
                    : nullptr;
                std::coroutine_handle<AwaitableFrameBase>::from_promise(*current).destroy();
                current = prev;
            }
        }
        else
            std::coroutine_handle<AwaitableFrameBase>::from_promise(*this).destroy();
    }

    void AwaitableFrameBase::resume() { this->coro_.resume(); }

    void AwaitableFrameBase::set_calling_coroutine(std::coroutine_handle<> h) { this->prev_ = h; }

    std::coroutine_handle<> AwaitableFrameBase::get_coroutine_handle() { return this->coro_; }

    std::coroutine_handle<> AwaitableFrameBase::get_calling_coroutine() { return this->prev_; }

    void AwaitableFrameBase::push_frame_into_task_queue(std::coroutine_handle<> h)
    {
        system::this_thread::detail::q.enqueue(h);
#if UVENT_DEBUG
        spdlog::trace("Coroutine returned into local queue: {}", h.address());
#endif
    }

    AwaitableFrameBase::AwaitableFrameBase()
    {
        this->t_id_ = system::this_thread::detail::t_id;
        this->cancel_ = system::this_thread::detail::current_cancel;
        this->trace_id_ = system::this_thread::detail::current_trace;
#ifdef UVENT_TASK_INTROSPECTION
        introspection::detail::register_frame(this);
#endif
    }

    AwaitableFrameBase::~AwaitableFrameBase()
    {
#ifdef UVENT_TASK_INTROSPECTION
        introspection::detail::unregister_frame(this);
#endif
    }

#ifdef UVENT_TASK_INTROSPECTION
    void AwaitableFrameBase::stamp_wait_time() noexcept { this->wait_since_ns_ = introspection::detail::now_ns(); }
#endif

    bool AwaitableFrameBase::await_ready() { return false; }

    void AwaitableFrameBase::push_frame_to_be_destroyed() { system::this_thread::detail::q_c.enqueue(this->coro_); }

    std::coroutine_handle<> AwaitableFrameBase::final_transfer() noexcept
    {
        if (this->task_) [[unlikely]]
            std::exchange(this->task_, nullptr)->complete(this);
        if (auto prev = std::exchange(this->prev_, std::coroutine_handle<>{}))
        {
            auto& pp = frame_of(prev);
            pp.set_next_coroutine(std::coroutine_handle<>{});
            if (system::stack_guard::stack_too_deep()) [[unlikely]]
            {
                pp.pending_destroy_ = this->coro_;
                push_frame_into_task_queue(prev);
                return std::noop_coroutine();
            }
            this->push_frame_to_be_destroyed();
            return prev;
        }
        this->push_frame_to_be_destroyed();
        return std::noop_coroutine();
    }

    std::coroutine_handle<> AwaitableFrameBase::yield_transfer() noexcept
    {
        if (this->prev_)
        {
            if (system::stack_guard::stack_too_deep()) [[unlikely]]
            {
                push_frame_into_task_queue(this->prev_);
                return std::noop_coroutine();
            }
            return this->prev_;
        }
        return std::noop_coroutine();
    }

    void AwaitableFrameBase::set_next_coroutine(std::coroutine_handle<> h) { this->next_ = h; }

    std::coroutine_handle<> AwaitableFrameBase::get_next_coroutine() { return this->next_; }

    AwaitableFrame<void>::~AwaitableFrame()
    {
#if UVENT_DEBUG
        spdlog::trace("Destroying coroutine {}", this->coro_.address());
#endif
    }

    std::suspend_always AwaitableFrame<void>::initial_suspend() noexcept { return {}; }

    FinalAwaiter AwaitableFrame<void>::final_suspend() noexcept
    {
#if UVENT_DEBUG
        spdlog::trace("Entering final_suspend for void coroutine {}", this->coro_.address());
#endif
        return {};
    }

    std::suspend_always AwaitableFrame<void>::yield_value() noexcept { return {}; }
} // namespace usub::uvent::detail
