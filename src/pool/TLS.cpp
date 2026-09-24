//
// Created by root on 10/21/25.
//

#include <uvent/pool/TLS.h>

#include <algorithm>
#include <thread>

#ifdef OS_LINUX
#ifndef UVENT_ENABLE_IO_URING
#include <uvent/poll/EPoller.h>
#else
#include <uvent/poll/IOUringPoller.h>
#endif
#elif defined(OS_BSD) || defined(OS_APPLE)
#include <uvent/poll/KPoller.h>
#else
#include <uvent/poll/IocpPoller.h>
#endif

namespace usub::uvent::thread
{
    void ThreadLocalStorage::push_task_inbox(std::coroutine_handle<> task)
    {
        if (!task)
            return;
        auto* frame = &std::coroutine_handle<detail::AwaitableFrameBase>::from_address(task.address()).promise();
        this->inbox_q_.push(frame);

        this->is_added_new_.store(true, std::memory_order_release);

        this->kick_poller();
    }

    void ThreadLocalStorage::wake_poller() noexcept { this->kick_poller(); }

    void ThreadLocalStorage::kick_poller() noexcept
    {
        this->wake_inflight_.fetch_add(1, std::memory_order_acq_rel);
        if (auto* p = this->poller_.load(std::memory_order_acquire))
            p->wake();
        this->wake_inflight_.fetch_sub(1, std::memory_order_acq_rel);
    }

    void ThreadLocalStorage::unset_poller() noexcept
    {
        this->poller_.store(nullptr, std::memory_order_release);
        while (this->wake_inflight_.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();
    }

    void ThreadLocalStorage::push_cancel_kick(uvent::task::TaskStateBase* t)
    {
        this->kick_q_.push(t);

        this->has_kicks_.store(true, std::memory_order_release);

        this->kick_poller();
    }

#ifdef UVENT_SOCKET_OWNER_FORWARDING
    void ThreadLocalStorage::push_socket_op(const SocketOp& op)
    {
        this->sock_ops_q_.enqueue(op);

        this->has_sock_ops_.store(true, std::memory_order_release);

        this->kick_poller();
    }
#endif
} // namespace usub::uvent::thread

#ifdef UVENT_RUNTIME_DRAIN
namespace usub::uvent::thread
{
    void ThreadLocalStorage::register_task(uvent::task::TaskStateBase* t)
    {
        this->tasks_.push_back(t);
        if (this->tasks_.size() >= this->sweep_at_) [[unlikely]]
            this->sweep_tasks();
    }

    void ThreadLocalStorage::register_task_external(uvent::task::TaskStateBase* t)
    {
        std::lock_guard lk(this->ext_mtx_);
        this->ext_tasks_.push_back(t);
    }

    void ThreadLocalStorage::take_external_tasks()
    {
        std::lock_guard lk(this->ext_mtx_);
        if (this->ext_tasks_.empty())
            return;
        this->tasks_.insert(this->tasks_.end(), this->ext_tasks_.begin(), this->ext_tasks_.end());
        this->ext_tasks_.clear();
    }

    std::size_t ThreadLocalStorage::sweep_tasks()
    {
        this->take_external_tasks();
        std::size_t live = 0, w = 0;
        for (std::size_t r = 0; r < this->tasks_.size(); ++r)
        {
            auto* t = this->tasks_[r];
            if (t->done())
            {
                t->release();
                continue;
            }
            this->tasks_[w++] = t;
            ++live;
        }
        this->tasks_.resize(w);
        this->sweep_at_ = std::max<std::size_t>(64, w * 2);
        return live;
    }

    void ThreadLocalStorage::cancel_registered_tasks()
    {
        this->take_external_tasks();
        for (auto* t : this->tasks_)
            if (!t->done())
                t->request_cancel();
    }

    void ThreadLocalStorage::release_registered_tasks()
    {
        this->take_external_tasks();
        for (auto* t : this->tasks_)
            t->release();
        this->tasks_.clear();
    }
} // namespace usub::uvent::thread
#endif
