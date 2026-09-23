//
// Created by kirill on 11/20/24.
//

#ifndef UVENT_SYSTEMCONTEXT_H
#define UVENT_SYSTEMCONTEXT_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <uvent/pool/TLSRegistry.h>
#include "Settings.h"
#include "uvent/base/Predefines.h"
#include "uvent/poll/PollerBase.h"
#include "uvent/sync/CancelState.h"
#include "uvent/tasks/AwaitableFrame.h"
#include "uvent/tasks/SharedTasks.h"
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"
#include "uvent/utils/datastructures/queue/FastQueue.h"
#include "uvent/utils/sync/QSBR.h"
#include "uvent/utils/timer/TimerWheel.h"

namespace usub::uvent::system
{
#ifndef UVENT_ENABLE_REUSEADDR
    constexpr bool is_reuseaddr_enabled = false;
#else
    constexpr bool is_reuseaddr_enabled = true;
#endif

    namespace global::detail
    {
        inline std::unique_ptr<thread::TLSRegistry> tls_registry{nullptr};
        extern std::atomic<int> thread_count;
#ifdef UVENT_RUNTIME_DRAIN
        /// \brief Set by Uvent::stop(): workers cancel their registered tasks and keep
        /// running until every registry is idle or drain_deadline_ns passes.
        extern std::atomic<bool> draining;
        extern std::atomic<uint64_t> drain_deadline_ns;
        /// \brief Tasks still not done when the workers exited (filled at exit).
        extern std::atomic<std::size_t> drain_survivors;
        /// \brief Invoked by the drain coordinator to actually stop every worker.
        extern void (*request_stop_all)();
#endif
    } // namespace global::detail

    /// \brief Variables used internally within the system.
    /// \attention **Do not attempt to modify variables inside directly** unless explicitly instructed in the
    /// documentation.
    namespace this_thread::detail
    {
#ifndef UVENT_ENABLE_REUSEADDR
        /// \brief Wrapper over I/O notification mechanism provided by OS.
        extern core::PollerImpl pl;
        /// \brief Timer wheel used to handle multiple timers efficiently.
        extern utils::TimerWheel wh;
#else
        /// \brief Wrapper over I/O notification mechanism provided by OS.
        thread_local extern core::PollerImpl pl;
        /// \brief Timer wheel used to handle multiple timers efficiently.
        thread_local extern utils::TimerWheel wh;
#endif
        /// \brief Task queue available to all threads in the thread pool.
        /// Or available to a single thread if there is only one thread in the thread pool
        extern std::unique_ptr<task::SharedTasks> st;
        /// \brief Currently executing coroutine (cec).
        thread_local extern std::coroutine_handle<> cec;
        /// \brief Thread's index inside thread pool.
        thread_local extern int t_id;
        /// \brief Coroutines to be destroyed
        thread_local extern queue::single_thread::Queue<std::coroutine_handle<>> q_c;
        /// \brief Cancel state of the currently running task.
        thread_local extern sync::CancelState* current_cancel;

        /// \brief Address of a thread_local, optionally taken through an optimizer barrier.
        ///
        /// GCC's -fsanitize=null (>= -O1) re-materialises TLS addresses at each use
        /// and hands its check a folded 0 for a perfectly valid object, producing a
        /// false "reference binding to null pointer" report (clang and -O0 are clean).
        /// Reading through a pointer the optimizer cannot see through avoids it. The
        /// barrier is compiled in only when CMake detects GCC + -fsanitize=undefined
        /// (UVENT_GCC_UBSAN_TLS_WORKAROUND); every other build gets a plain identity.
        template <class T>
        inline T* tls_addr(T* p) noexcept
        {
#if defined(UVENT_GCC_UBSAN_TLS_WORKAROUND)
            asm volatile("" : "+r"(p));
#endif
            return p;
        }

        /// \brief current_cancel read via tls_addr (see there).
        inline sync::CancelState* current_cancel_ptr() noexcept { return *tls_addr(&current_cancel); }
        /// \brief Trace id of the currently running task.
        thread_local extern uint64_t current_trace;
        /// \brief Remaining cooperative budget for the current resume slice.
        thread_local extern int32_t coop_left;
#ifndef UVENT_ENABLE_REUSEADDR
        extern usub::utils::sync::QSBR g_qsbr;
#else
        /// \brief Sockets to be destroyed
        thread_local extern queue::single_thread::Queue<net::SocketHeader*> q_sh;
#endif

#ifndef UVENT_ENABLE_REUSEADDR
        extern std::atomic<bool> is_started;
#else
        extern bool is_started;
#endif
    } // namespace this_thread::detail

    namespace detail
    {
        using AwaitableFrameBase = uvent::detail::AwaitableFrameBase;

        inline bool cancel_timer_now(uint64_t id) noexcept
        {
#ifdef UVENT_ENABLE_REUSEADDR
            return this_thread::detail::wh.cancelTimerDetach(id);
#else
            return this_thread::detail::wh.cancelTimerSyncDetach(id);
#endif
        }
    } // namespace detail

    namespace coop
    {
        inline bool consume() noexcept
        {
            auto& b = this_thread::detail::coop_left;
            if (b > 0)
            {
                --b;
                return true;
            }
            return false;
        }

        inline bool exhausted() noexcept { return this_thread::detail::coop_left <= 0; }
    } // namespace coop

    namespace this_coroutine
    {
        struct YieldAwaiter
        {
            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) const noexcept { this_thread::detail::q.enqueue(h); }

            void await_resume() const noexcept {}
        };

        inline YieldAwaiter yield() noexcept { return {}; }

        inline bool cancel_requested() noexcept
        {
            auto* s = this_thread::detail::current_cancel_ptr();
            return s && s->requested.load(std::memory_order_relaxed);
        }

        inline sync::CancelState* cancel_state() noexcept { return this_thread::detail::current_cancel_ptr(); }

        inline uint64_t trace_id() noexcept { return this_thread::detail::current_trace; }

        void set_trace_id(uint64_t id) noexcept;

        void set_name(const char* n) noexcept;

        struct SleepAwaiter
        {
            utils::Timer* t;
            uint64_t id{0};
            bool cancelled{false};

            bool await_ready() const noexcept { return false; }

            bool await_suspend(std::coroutine_handle<> h) noexcept
            {
                auto* f = &uvent::detail::frame_of(h);
                if (f->cancel_requested())
                {
                    this->cancelled = true;
                    delete this->t;
                    return false;
                }
                this->t->bind(h);
                this->id = this_thread::detail::wh.addTimer(this->t);
                if (f->arm_cancel(&SleepAwaiter::on_cancel, this, "sleep"))
                {
                    if (system::detail::cancel_timer_now(this->id))
                    {
                        f->disarm_cancel();
                        this->cancelled = true;
                        delete this->t;
                        return false;
                    }
                }
                return true;
            }

            bool await_resume() const noexcept { return !this->cancelled; }

            static void on_cancel(uvent::detail::AwaitableFrameBase* f, void* arg) noexcept
            {
                auto* a = static_cast<SleepAwaiter*>(arg);
                if (system::detail::cancel_timer_now(a->id))
                {
                    a->cancelled = true;
                    delete a->t;
                    this_thread::detail::q.enqueue(f->get_coroutine_handle());
                }
            }
        };

        template <class Rep, class Period>
        task::Awaitable<bool> sleep_for(std::chrono::duration<Rep, Period> d)
        {
            using namespace std::chrono;
            auto ms = duration_cast<milliseconds>(d + milliseconds(1) - milliseconds(0));
            auto ms_count = std::max<int64_t>(1, ms.count());

            const bool completed = co_await SleepAwaiter{new utils::Timer(static_cast<timer_duration_t>(ms_count))};
            co_return completed;
        }
    } // namespace this_coroutine

    /**
     * @brief Spawns a coroutine for execution in the global thread context.
     *
     * Retrieves the coroutine promise from the given function object and, if valid,
     * enqueues its coroutine handle into the global task queue.
     *
     * @tparam F Coroutine function type providing `get_promise()`.
     * @param f Coroutine function to be spawned.
     *
     * @warning Method doesn't check if the coroutine is valid beyond `get_promise()`.
     *          Ensure the coroutine object remains valid until scheduled.
     */
    template <class F>
    concept SharedSpawnable = !uvent::detail::LocalFrame<typename std::remove_cvref_t<F>::promise_type>;

    template <typename F>
        requires SharedSpawnable<F>
    void co_spawn(F&& f)
    {
        auto promise = f.get_promise();
        if (promise)
        {
            static_cast<detail::AwaitableFrameBase*>(promise)->detach_from_task();
            this_thread::detail::st->enqueue(promise->get_coroutine_handle());
        }
    }

    inline void co_spawn(std::coroutine_handle<> h) { this_thread::detail::st->enqueue(h); }

    /**
     * @brief Enqueues a coroutine into the inbox of a specific thread.
     *
     * Registers a coroutine for execution in the context of the given thread.
     * The coroutine handle is placed into the target thread’s inbox queue.
     *
     * This function is safe to call at any time (both before and after the event loop starts).
     * If the target thread/event-loop is already running, the task will be picked up by the runtime
     * according to the inbox processing rules of that thread.
     *
     * @tparam F Type providing `get_promise()` returning an optional-like pointer to a promise.
     *           The promise must provide `get_coroutine_handle()`.
     * @param f Coroutine function/object to be enqueued.
     * @param threadIndex Index of the target thread whose inbox receives the coroutine.
     *
     * @warning This function stores only the coroutine handle; it does not extend coroutine lifetime.
     *          The coroutine must remain valid until executed/destroyed by the runtime.
     */
    template <typename F>
    void co_spawn_static(F&& f, int threadIndex)
    {
        auto promise = f.get_promise();
        if (promise)
        {
            auto* base = static_cast<detail::AwaitableFrameBase*>(promise);
            base->set_thread_id(threadIndex);
            base->detach_from_task();
            global::detail::tls_registry->getStorage(threadIndex)->push_task_inbox(promise->get_coroutine_handle());
        }
    }

    /**
     * @brief Enqueues an existing coroutine handle into the inbox of a specific thread.
     *
     * Places the provided coroutine handle into the target thread’s inbox queue for later execution.
     *
     * This function is safe to call at any time (both before and after the event loop starts).
     * If the target thread/event-loop is already running, the task will be picked up by the runtime
     * according to the inbox processing rules of that thread.
     *
     * @param h Coroutine handle to be enqueued.
     * @param threadIndex Index of the target thread whose inbox receives the coroutine.
     *
     * @warning This function stores only the coroutine handle; it does not extend coroutine lifetime.
     *          The coroutine must remain valid until executed/destroyed by the runtime.
     */
    inline void co_spawn_static(std::coroutine_handle<> h, int threadIndex)
    {
        auto typed = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(h.address());
        typed.promise().set_thread_id(threadIndex);
        global::detail::tls_registry->getStorage(threadIndex)->push_task_inbox(h);
    }

    /**
     * @brief Resumes a coroutine woken by I/O (poller event, socket timeout,
     *        cancel hook) on the worker that coroutine belongs to.
     *
     * A socket may be driven from a worker other than the one that registered
     * it (owner forwarding); its poller and timer wheel then wake the parked
     * coroutine on the owner. Resuming it there races with the parent, which may
     * still be inside await_suspend on its own thread: the I/O frame is eager and
     * parks before the parent has attached (prev_/cancel state/trace id are
     * written after publication). Handing the handle back through the target
     * worker's inbox keeps every frame on one thread, so the parent's writes are
     * ordered by that thread's loop. Same-worker wakes take the local queue as
     * before; frames without a worker (created outside the runtime) run locally.
     */
    inline void resume_waiter(std::coroutine_handle<> h) noexcept
    {
#ifdef UVENT_SOCKET_OWNER_FORWARDING
        const int tid = uvent::detail::frame_of(h).get_thread_id();
        if (tid == this_thread::detail::t_id || tid < 0)
            this_thread::detail::q.enqueue(h);
        else
            co_spawn_static(h, tid);
#else
        // Legacy layout (UVENT_ENABLE_REUSEADDR=OFF): the poller is shared, so
        // whichever worker polls resumes the waiter itself. Routing through the
        // inbox here would make every I/O wake-up wait for that worker's next
        // turn at the poller lock.
        this_thread::detail::q.enqueue(h);
#endif
    }

    /**
     * @brief Enqueues a coroutine into the inbox of a specific thread, optionally setting thread id in the promise.
     *
     * Registers a coroutine for execution in the context of the given thread.
     * The coroutine handle is placed into the target thread’s inbox queue.
     *
     * This function is safe to call at any time (both before and after the event loop starts).
     * If the target thread/event-loop is already running, the task will be picked up by the runtime
     * according to the inbox processing rules of that thread.
     *
     * @tparam F Type providing `get_promise()`.
     * @tparam is_thread_id_set Controls whether the coroutine promise gets `threadIndex` written into it.
     *         - `true`: the coroutine promise is treated as `detail::AwaitableFrameBase` and
     *           `promise().set_thread_id(threadIndex)` is called before enqueue.
     *           This requires that `F` is a coroutine whose promise type is compatible with
     *           `detail::AwaitableFrameBase` (i.e. `from_promise(f.get_promise())` is valid and the
     *           promise implements `set_thread_id(int)`).
     *         - `false`: no promise mutation is performed; the handle is obtained via
     *           `get_promise()` (optional-like) and `promise->get_coroutine_handle()`.
     *
     * @param f Coroutine function/object to be enqueued.
     * @param threadIndex Index of the target thread whose inbox receives the coroutine.
     *
     * @warning This function stores only the coroutine handle; it does not extend coroutine lifetime.
     *          The coroutine must remain valid until executed/destroyed by the runtime.
     */
    template <typename F, bool is_thread_id_set>
    void co_spawn_static(F&& f, int threadIndex)
    {
        if constexpr (is_thread_id_set)
        {
            auto handle = std::coroutine_handle<detail::AwaitableFrameBase>::from_promise(f.get_promise());
            handle.promise().set_thread_id(threadIndex);
            if (handle)
                global::detail::tls_registry->getStorage(threadIndex)->push_task_inbox(handle);
        }
        else if (auto promise = f.get_promise(); promise)
            global::detail::tls_registry->getStorage(threadIndex)->push_task_inbox(promise->get_coroutine_handle());
    }

    /**
     * @brief Enqueues an existing coroutine handle into the inbox of a specific thread,
     *        optionally setting thread id in the promise.
     *
     * Places the provided coroutine handle into the target thread’s inbox queue for later execution.
     *
     * This function is safe to call at any time (both before and after the event loop starts).
     * If the target thread/event-loop is already running, the task will be picked up by the runtime
     * according to the inbox processing rules of that thread.
     *
     * @tparam is_thread_id_set Controls whether the coroutine promise gets `threadIndex` written into it.
     *         - `true`: `h` is assumed to reference a coroutine whose promise type is compatible with
     *           `detail::AwaitableFrameBase` and implements `set_thread_id(int)`. The function will
     *           reinterpret the handle address as `std::coroutine_handle<detail::AwaitableFrameBase>`
     *           and call `promise().set_thread_id(threadIndex)`.
     *         - `false`: no promise mutation is performed; `h` is enqueued as-is.
     *
     * @param h Coroutine handle to be enqueued.
     * @param threadIndex Index of the target thread whose inbox receives the coroutine.
     *
     * @warning If `is_thread_id_set == true` and `h` does not actually refer to a coroutine with a
     *          compatible promise type, behavior is undefined.
     *
     * @warning This function stores only the coroutine handle; it does not extend coroutine lifetime.
     *          The coroutine must remain valid until executed/destroyed by the runtime.
     */
    template <bool is_thread_id_set>
    inline void co_spawn_static(std::coroutine_handle<> h, int threadIndex)
    {
        if (!h)
            return;
        if constexpr (is_thread_id_set)
        {
            auto handle = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(h.address());
            handle.promise().set_thread_id(threadIndex);
        }
        global::detail::tls_registry->getStorage(threadIndex)->push_task_inbox(h);
    }

    /**
     * @brief Schedules a timer in timer wheel.
     *
     * Adds the given timer instance into timer wheel handler,
     * allowing it to be triggered after its configured expiry.
     *
     * @param timer Pointer to a valid timer object.
     *
     * @note If the timer type is set to TIMEOUT, it will fire once;
     *       otherwise, it will repeat indefinitely.
     *
     * @warning This method does not check whether the timer is initialized
     *          or already active. Use only with properly constructed and inactive timers.
     */
    inline void spawn_timer(utils::Timer* timer) { this_thread::detail::wh.addTimer(timer); }
} // namespace usub::uvent::system

#endif // UVENT_SYSTEMCONTEXT_H
