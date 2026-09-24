#include "uvent/tasks/TaskState.h"

#include "uvent/pool/TLS.h"
#include "uvent/pool/TLSRegistry.h"
#include "uvent/sync/SyncCommon.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/AwaitableFrame.h"

namespace usub::uvent::sync
{
    namespace
    {
        void wake_all(WaitList& list, int32_t idx) noexcept
        {
            if (list.empty_relaxed())
                return;
            list.lock();
            while (Waiter* w = list.pop_front_locked())
                detail::fire_waiter(w, idx);
            list.unlock();
        }

        void wake_join_if_drained(CancelState* s) noexcept
        {
            if (s->join_waiters.empty_relaxed())
                return;
            s->join_waiters.lock();
            if (s->live_tasks.load(std::memory_order_acquire) == 0)
                while (Waiter* w = s->join_waiters.pop_front_locked())
                    detail::fire_waiter(w);
            s->join_waiters.unlock();
        }
    } // namespace

    void CancelState::release() noexcept
    {
        if (this->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
            return;
        this->unlink_from_parent();
        CancelState* p = this->parent;
        delete this;
        if (p)
            p->release();
    }

    void CancelState::request_cancel() noexcept
    {
        if (this->requested.exchange(true, std::memory_order_seq_cst))
            return;
        detail::notify_fence();
        wake_all(this->cancel_waiters, 0);
        if (this->is_task)
            static_cast<task::TaskStateBase*>(this)->kick();
        this->lock_tree();
        for (CancelState* c = this->first_child; c; c = c->next_sib)
            c->request_cancel();
        this->unlock_tree();
    }

    void CancelState::link(CancelState* parent, CancelState* child) noexcept
    {
        parent->add_ref();
        child->parent = parent;
        parent->lock_tree();
        child->next_sib = parent->first_child;
        child->prev_sib = nullptr;
        if (parent->first_child)
            parent->first_child->prev_sib = child;
        parent->first_child = child;
        if (parent->requested.load(std::memory_order_acquire))
            child->requested.store(true, std::memory_order_release);
        parent->unlock_tree();
    }

    void CancelState::unlink_from_parent() noexcept
    {
        CancelState* p = this->parent;
        if (!p)
            return;
        p->lock_tree();
        if (this->prev_sib)
            this->prev_sib->next_sib = this->next_sib;
        else
            p->first_child = this->next_sib;
        if (this->next_sib)
            this->next_sib->prev_sib = this->prev_sib;
        this->prev_sib = nullptr;
        this->next_sib = nullptr;
        p->unlock_tree();
    }

    void CancelState::retain_live_chain() noexcept
    {
        for (CancelState* s = this; s; s = s->parent)
            s->live_tasks.fetch_add(1, std::memory_order_seq_cst);
    }

    void CancelState::drop_live_chain() noexcept
    {
        for (CancelState* s = this; s; s = s->parent)
        {
            if (s->live_tasks.fetch_sub(1, std::memory_order_seq_cst) == 1)
            {
                sync::detail::notify_fence();
                wake_join_if_drained(s);
            }
        }
    }
} // namespace usub::uvent::sync

namespace usub::uvent::task
{
    void TaskStateBase::complete(detail::AwaitableFrameBase* frame) noexcept
    {
        if (this->store_result)
            this->store_result(this, frame);
        this->status.store(1, std::memory_order_seq_cst);
        sync::detail::notify_fence();
        if (!this->done_waiters.empty_relaxed())
        {
            this->done_waiters.lock();
            while (sync::Waiter* w = this->done_waiters.pop_front_locked())
                sync::detail::fire_waiter(w);
            this->done_waiters.unlock();
        }
        this->drop_live_chain();
        this->release();
    }

    void TaskStateBase::kick() noexcept
    {
#ifndef UVENT_ENABLE_REUSEADDR
        return;
#else
        const int tid = this->owner_tid.load(std::memory_order_seq_cst);
        if (tid < 0 || !system::global::detail::tls_registry ||
            tid >= system::global::detail::thread_count.load(std::memory_order_relaxed))
            return;
        if (this->kick_pending.exchange(true, std::memory_order_acq_rel))
            return;
        // A parent's request_cancel() walks its children under the tree lock. A child
        // that already completed (e.g. it polled cancel_requested() and saw the parent's
        // flag) may have dropped its last reference and be blocked in
        // unlink_from_parent() waiting for that very lock: it is still linked but its
        // refcount is zero. Resurrecting it here would enqueue an object that is deleted
        // as soon as the walk releases the lock (use-after-free in processCancelKicks).
        if (!this->try_add_ref())
        {
            this->kick_pending.store(false, std::memory_order_release);
            return;
        }
        system::global::detail::tls_registry->getStorage(tid)->push_cancel_kick(this);
#endif
    }

    void TaskStateBase::process_kick() noexcept
    {
        this->kick_pending.store(false, std::memory_order_release);
        if (!this->done())
        {
            auto* f = &detail::frame_of(this->root);
            while (auto next = f->next_handle())
                f = &detail::frame_of(next);
            f->run_cancel_hook();
        }
        this->release();
    }
} // namespace usub::uvent::task

#ifdef UVENT_RUNTIME_DRAIN
namespace usub::uvent::task::detail
{
    void register_task(TaskStateBase* ts) noexcept
    {
        auto* reg = system::global::detail::tls_registry.get();
        if (!reg)
            return;
        const int n = system::global::detail::thread_count.load(std::memory_order_relaxed);
        const int tid = system::this_thread::detail::t_id;
        ts->add_ref();
        if (tid >= 0 && tid < n)
            reg->getStorage(tid)->register_task(ts);
        else
            reg->getStorage(0)->register_task_external(ts);
    }
} // namespace usub::uvent::task::detail
#endif
