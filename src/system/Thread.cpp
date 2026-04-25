//
// Created by Kirill Zhukov on 07.11.2024.
//

#include "uvent/system/Thread.h"
#include <utility>
#include "uvent/net/Socket.h"

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
        auto& local_pl = system::this_thread::detail::pl;
        auto& local_wh = system::this_thread::detail::wh;
        auto& local_q = system::this_thread::detail::q;
        auto& local_q_c = system::this_thread::detail::q_c;
#ifndef UVENT_ENABLE_REUSEADDR
        auto& local_g_qsbr = system::this_thread::detail::g_qsbr;
#else
        auto& local_q_sh = system::this_thread::detail::q_sh;
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
            if (local_pl.try_lock())
            {
                auto next_timeout = local_wh.getNextTimeout();
                local_pl.poll((local_q->empty()) ? (next_timeout > 0) ? next_timeout : settings::idle_fallback_ms : 0);
                local_pl.unlock();
            }
            else if (local_q->empty() && local_q_c.empty())
            {
                auto next_timeout = local_wh.getNextTimeout();
                local_pl.lock_poll((local_q->empty()) ? (next_timeout > 0) ? next_timeout : settings::idle_fallback_ms
                                                      : 0);
            }
#else
            auto next_timeout = local_wh.getNextTimeout();
            local_pl.poll(local_q->empty() ? (next_timeout > 0) ? next_timeout : settings::idle_fallback_ms : 0);
#endif
            size_t n;
            while ((n = local_q->dequeue_bulk(this->tmp_tasks_.data(), this->tmp_tasks_.size())) > 0)
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
                            c.resume();
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
                    local_q->enqueue(task);
            }

            const size_t n_coroutines =
                local_q_c.dequeue_bulk(this->tmp_coroutines_.data(), this->tmp_coroutines_.size());
            for (size_t i = 0; i < n_coroutines; i++)
            {
                auto c_temp =
                    std::coroutine_handle<detail::AwaitableFrameBase>::from_address(this->tmp_coroutines_[i].address());
#ifdef UVENT_DEBUG
                spdlog::info("Coroutine destroyed in auxiliary loop: {}", this->tmp_coroutines_[i].address());
#endif
                c_temp.destroy();
            }
#ifndef UVENT_ENABLE_REUSEADDR
            local_g_qsbr.quiesce_tick();
#else
            const size_t n_sockets = local_q_sh.dequeue_bulk(this->tmp_sockets_.data(), this->tmp_sockets_.size());
            for (size_t i = 0; i < n_sockets; ++i)
                delete this->tmp_sockets_[i];
#endif
            this->processInboxQueue();
        }

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
            }
        }
#ifdef UVENT_ENABLE_REUSEADDR
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

        constexpr size_t BATCH = 64;
        std::coroutine_handle<> buf[BATCH];

        for (;;)
        {
            size_t n = tls->inbox_q_.try_dequeue_bulk(buf, BATCH);
            if (n == 0)
                break;

            for (size_t i = 0; i < n; ++i)
            {
                if (buf[i])
                    system::this_thread::detail::q->enqueue(buf[i]);
            }
        }
    }

    Thread::~Thread()
    {
        for (auto& c : this->tmp_coroutines_)
            if (c)
                c.destroy();
        for (auto s : this->tmp_sockets_)
            if (s)
                delete s;
    }

    void Thread::run_current() { threadFunction(this->stop_source_.get_token()); }

    bool Thread::stop()
    {
        bool a = false;
        if (this->thread_.joinable())
            a = this->thread_.request_stop();

        bool b = this->stop_source_.request_stop();
        return a || b;
    }
} // namespace usub::uvent::system
