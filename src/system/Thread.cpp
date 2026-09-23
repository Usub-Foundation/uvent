//
// Created by Kirill Zhukov on 07.11.2024.
//

#include "uvent/system/Thread.h"
#include <algorithm>
#include <utility>
#include "uvent/net/Socket.h"
#include "uvent/system/StackGuard.h"
#include "uvent/tasks/TaskState.h"

namespace usub::uvent::system
{
    Thread::Thread(std::barrier<>* barrier, int index, thread::ThreadLocalStorage* thread_local_storage,
                   ThreadLaunchMode tlm) :
        barrier(barrier), index_(index), thread_local_storage_(thread_local_storage), tlm(tlm)
    {
#if UVENT_DEBUG
        spdlog::info("Thread #{} started", index);
#endif
        this->tmp_tasks_.resize(settings::max_pre_allocated_tasks_items);
        this->tmp_sockets_.resize(settings::max_pre_allocated_tmp_sockets_items);
        this->tmp_coroutines_.resize(settings::max_pre_allocated_tmp_coroutines_items);

        if (tlm == NEW)
            this->thread_ = std::jthread([this](std::stop_token token) { this->threadFunction(token); });
    }

    void Thread::threadFunction(std::stop_token token)
    {
        this_thread::detail::t_id = this->index_;
        system::stack_guard::set_stack_base_here();
        namespace tls = system::this_thread::detail;
        auto& local_pl = *tls::tls_addr(&tls::pl);
        this->thread_local_storage_->set_poller(&local_pl);
        auto& local_wh = *tls::tls_addr(&tls::wh);
        auto& local_q = *tls::tls_addr(&tls::q);
        auto& local_q_c = *tls::tls_addr(&tls::q_c);
#ifndef UVENT_ENABLE_REUSEADDR
        auto& local_g_qsbr = tls::g_qsbr;
#else
        auto& local_q_sh = *tls::tls_addr(&tls::q_sh);
#endif
#if defined(OS_LINUX) && defined(UVENT_PIN_THREADS)
        pthread_t self = pthread_self();
        pin_thread_to_core(this->index_);
        set_thread_name(std::string("uvent_worker_" + std::to_string(this->index_)), self);
#endif
        this->barrier->arrive_and_wait();
        this->processInboxQueue();
        using namespace system::this_thread::detail;
#ifndef UVENT_ENABLE_REUSEADDR
        local_g_qsbr.attach_current_thread();
#endif
        while (!token.stop_requested())
        {
#ifndef UVENT_ENABLE_REUSEADDR
            // The poller is shared and taken under a lock: while one worker sleeps
            // in it every other worker is parked in lock_poll() and cannot tick its
            // own wheel or run its queue. Never sleep longer than idle_fallback_ms,
            // otherwise a single far timer (e.g. a 1 h sleep) on the polling worker
            // stalls the whole runtime, including a stop() issued by another worker.
            // Work handed to this worker through its inbox (spawns, cross-worker
            // wake-ups) must be visible before deciding to block on the poller
            // lock, or a worker whose only pending work sits in the inbox parks in
            // lock_poll() while the polling worker re-takes the lock every time it
            // wakes: nothing runs, the runtime is stuck at 0 % CPU.
            this->processInboxQueue();
            const auto shared_poll_wait = [&]() -> int
            {
                if (!local_q.empty())
                    return 0;
                const auto next_timeout = local_wh.getNextTimeout();
                if (next_timeout > 0 && next_timeout < settings::idle_fallback_ms)
                    return static_cast<int>(next_timeout);
                return settings::idle_fallback_ms;
            };
            if (local_pl.try_lock())
            {
                local_pl.poll(shared_poll_wait());
                local_pl.unlock();
            }
            else if (local_q.empty() && local_q_c.empty())
            {
                local_pl.lock_poll(shared_poll_wait());
            }
#else
            auto next_timeout = local_wh.getNextTimeout();
            local_pl.poll(local_q.empty() ? (next_timeout > 0) ? next_timeout : settings::idle_fallback_ms : 0);
#endif
            size_t n;
            for (size_t quantum = settings::loop_task_quantum; quantum > 0 &&
                 (n = local_q.dequeue_bulk(this->tmp_tasks_.data(), std::min(quantum, this->tmp_tasks_.size()))) > 0;
                 quantum -= n)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    auto& elem = this->tmp_tasks_[i];
                    if (!elem)
                        continue;

                    auto c = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(elem.address());
                    if (c)
                    {
                        this_thread::detail::cec = c;
#if UVENT_DEBUG
                        spdlog::debug("Prev address: {}", static_cast<void*>(c.address()));
#endif
                        if (!c.done())
                        {
#if UVENT_DEBUG
                            spdlog::info("Coroutine resumed: {}", c.address());
#endif
                            auto& pr = c.promise();
                            pr.on_loop_resume();
                            this_thread::detail::current_cancel = pr.cancel_state();
                            this_thread::detail::current_trace = pr.trace_id();
                            this_thread::detail::coop_left = settings::coop_budget;
                            const auto pending = pr.take_pending_destroy();
                            c.resume();
                            if (pending) [[unlikely]]
                                local_q_c.enqueue(pending); // child read by the resume above
                        }
                    }
                }
            }
#ifndef UVENT_ENABLE_REUSEADDR
            if (local_wh.mtx.try_lock())
            {
                local_wh.tick();
                local_wh.mtx.unlock();
            }
#else
            local_wh.tick();
#endif
            if (st->getSize() > 0)
            {
                if (std::coroutine_handle<> task; st->dequeue(task))
                {
                    auto& pr =
                        std::coroutine_handle<detail::AwaitableFrameBase>::from_address(task.address()).promise();
                    pr.set_thread_id(this->index_);
                    if (auto* ts = pr.task_state())
                    {
                        ts->owner_tid.store(this->index_, std::memory_order_seq_cst);
                        if (ts->requested.load(std::memory_order_seq_cst))
                            ts->kick();
                    }
                    local_q.enqueue(task);
                }
            }

            for (size_t n_coroutines; (n_coroutines = local_q_c.dequeue_bulk(this->tmp_coroutines_.data(),
                                                                             this->tmp_coroutines_.size())) > 0;)
            {
                for (size_t i = 0; i < n_coroutines; i++)
                {
                    auto c_temp = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(
                        this->tmp_coroutines_[i].address());
#ifdef UVENT_DEBUG
                    spdlog::info("Coroutine destroyed in auxiliary loop: {}", this->tmp_coroutines_[i].address());
#endif
                    c_temp.destroy();
                }
            }
#ifndef UVENT_ENABLE_REUSEADDR
            local_g_qsbr.quiesce_tick();
#else
            const size_t n_sockets = local_q_sh.dequeue_bulk(this->tmp_sockets_.data(), this->tmp_sockets_.size());
            for (size_t i = 0; i < n_sockets; ++i)
                delete this->tmp_sockets_[i];
#endif
            this->processInboxQueue();
            this->processCancelKicks();
#ifdef UVENT_SOCKET_OWNER_FORWARDING
            this->processSocketOps();
#endif
#ifdef UVENT_RUNTIME_DRAIN
            if (system::global::detail::draining.load(std::memory_order_relaxed)) [[unlikely]]
                this->drainStep();
#endif
        }

#ifdef UVENT_RUNTIME_DRAIN
        {
            auto* tls = this->thread_local_storage_;
            const std::size_t left = tls->sweep_tasks();
            if (left)
                system::global::detail::drain_survivors.fetch_add(left, std::memory_order_relaxed);
            tls->release_registered_tasks();
        }
#endif
        this->thread_local_storage_->unset_poller();

        this->processCancelKicks();

        for (;;)
        {
            const size_t n_drain = local_q_c.dequeue_bulk(this->tmp_coroutines_.data(), this->tmp_coroutines_.size());
            if (n_drain == 0)
                break;
            for (size_t i = 0; i < n_drain; i++)
            {
                auto c_temp =
                    std::coroutine_handle<detail::AwaitableFrameBase>::from_address(this->tmp_coroutines_[i].address());
                c_temp.destroy();
                this->tmp_coroutines_[i] = nullptr;
            }
        }
#ifdef UVENT_ENABLE_REUSEADDR
#ifdef UVENT_SOCKET_OWNER_FORWARDING
        // ops forwarded by other workers may still be pending; apply them so their
        // headers land in q_sh and get freed below
        this->processSocketOps();
#endif
        for (;;)
        {
            const size_t n_sockets = local_q_sh.dequeue_bulk(this->tmp_sockets_.data(), this->tmp_sockets_.size());
            if (n_sockets == 0)
                break;
            for (size_t i = 0; i < n_sockets; ++i)
                delete this->tmp_sockets_[i];
        }
#endif

#ifndef UVENT_ENABLE_REUSEADDR
        local_g_qsbr.detach_current_thread();
#endif
    }

    void Thread::processInboxQueue()
    {
        auto* tls = this->thread_local_storage_;

        if (!tls->is_added_new_.exchange(false, std::memory_order_acq_rel))
            return;

        while (auto* frame = tls->inbox_q_.pop())
            system::this_thread::detail::q.enqueue(frame->get_coroutine_handle());
    }

#ifdef UVENT_RUNTIME_DRAIN
    void Thread::drainStep()
    {
        auto* tls = this->thread_local_storage_;
        if (!this->drain_started_)
        {
            this->drain_started_ = true;
            tls->cancel_registered_tasks();
        }
        const std::size_t live_now = tls->sweep_tasks();
        tls->set_drain_idle(live_now == 0);
        if (this->index_ != 0)
            return;
        const int n = system::global::detail::thread_count.load(std::memory_order_relaxed);
        bool all_idle = true;
        for (int i = 0; i < n && all_idle; ++i)
            all_idle = system::global::detail::tls_registry->getStorage(i)->drain_idle();
        const auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (all_idle || now >= system::global::detail::drain_deadline_ns.load(std::memory_order_relaxed))
            if (auto fn = system::global::detail::request_stop_all)
                fn();
    }
#endif

    void Thread::processCancelKicks()
    {
        auto* tls = this->thread_local_storage_;

        if (!tls->has_kicks_.exchange(false, std::memory_order_acq_rel))
            return;

        while (auto* t = tls->kick_q_.pop())
            t->process_kick();
    }

#ifdef UVENT_SOCKET_OWNER_FORWARDING
    void Thread::processSocketOps()
    {
        auto* tls = this->thread_local_storage_;

        if (!tls->has_sock_ops_.exchange(false, std::memory_order_acq_rel))
            return;

        constexpr size_t BATCH = 64;
        thread::SocketOp buf[BATCH];

        for (;;)
        {
            const size_t n = tls->sock_ops_q_.try_dequeue_bulk(buf, BATCH);
            if (n == 0)
                break;
            for (size_t i = 0; i < n; ++i)
                net::detail::apply_socket_op(buf[i]);
        }
    }
#endif

    Thread::~Thread()
    {
        if (this->thread_.joinable())
        {
            this->thread_.request_stop();
            this->thread_.join();
        }
    }

    void Thread::run_current() { threadFunction(this->stop_source_.get_token()); }

    bool Thread::stop()
    {
        bool a = false;
        if (this->thread_.joinable())
            a = this->thread_.request_stop();

        bool b = this->stop_source_.request_stop();
        if (this->thread_local_storage_)
            this->thread_local_storage_->wake_poller();
        return a || b;
    }
} // namespace usub::uvent::system
