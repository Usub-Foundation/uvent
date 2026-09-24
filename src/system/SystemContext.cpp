//
// Created by kirill on 11/20/24.
//

#include "uvent/system/SystemContext.h"
#include "uvent/tasks/AwaitableFrame.h"

#ifdef OS_LINUX
#ifndef UVENT_ENABLE_IO_URING
#include "uvent/poll/EPoller.h"
#else
#include "uvent/poll/IOUringPoller.h"
#endif
#elif OS_BSD || OS_APPLE
#include "uvent/poll/KPoller.h"
#else
#include "uvent/poll/IocpPoller.h"
#endif


namespace usub::uvent::system
{
    namespace global::detail
    {
        std::atomic<int> thread_count = -1;
#ifdef UVENT_RUNTIME_DRAIN
        std::atomic<bool> draining{false};
        std::atomic<uint64_t> drain_deadline_ns{0};
        std::atomic<std::size_t> drain_survivors{0};
        void (*request_stop_all)() = nullptr;
#endif
    } // namespace global::detail
    namespace this_thread::detail
    {
#ifndef UVENT_ENABLE_REUSEADDR
        utils::TimerWheel wh = utils::TimerWheel();
        core::PollerImpl pl = core::PollerImpl{wh};
#else
        thread_local utils::TimerWheel wh = utils::TimerWheel();
        thread_local core::PollerImpl pl = core::PollerImpl{wh};
#endif
        std::unique_ptr<task::SharedTasks> st = std::make_unique<task::SharedTasks>();
        thread_local std::coroutine_handle<> cec{nullptr};
        thread_local queue::single_thread::Queue<std::coroutine_handle<>> q;
        thread_local int t_id{-1};
        thread_local sync::CancelState* current_cancel{nullptr};
        thread_local uint64_t current_trace{0};
        thread_local int32_t coop_left{INT32_MAX};
        thread_local queue::single_thread::Queue<std::coroutine_handle<>> q_c =
            queue::single_thread::Queue<std::coroutine_handle<>>();
#ifndef UVENT_ENABLE_REUSEADDR
        usub::utils::sync::QSBR g_qsbr;
#else
        thread_local queue::single_thread::Queue<net::SocketHeader*> q_sh =
            queue::single_thread::Queue<net::SocketHeader*>();
#endif
#ifndef UVENT_ENABLE_REUSEADDR
        std::atomic<bool> is_started{false};
#else
        bool is_started{false};
#endif
    } // namespace this_thread::detail

    namespace this_coroutine
    {
        void set_trace_id(uint64_t id) noexcept
        {
            this_thread::detail::current_trace = id;
            auto cec = this_thread::detail::cec;
            if (!cec)
                return;
            auto* f = &uvent::detail::frame_of(cec);
            for (auto* up = f;;)
            {
                up->set_trace_id(id);
                auto prev = up->prev_handle();
                if (!prev)
                    break;
                up = &uvent::detail::frame_of(prev);
            }
            for (auto next = f->next_handle(); next;)
            {
                auto* down = &uvent::detail::frame_of(next);
                down->set_trace_id(id);
                next = down->next_handle();
            }
        }

        void set_name(const char* n) noexcept
        {
            auto cec = this_thread::detail::cec;
            if (!cec)
                return;
            auto* f = &uvent::detail::frame_of(cec);
            while (auto next = f->next_handle())
                f = &uvent::detail::frame_of(next);
            f->set_name(n);
        }
    } // namespace this_coroutine
} // namespace usub::uvent::system
