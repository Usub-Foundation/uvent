#ifndef UVENT_BLOCKING_BLOCKING_H
#define UVENT_BLOCKING_BLOCKING_H

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include "uvent/system/SystemContext.h"
#include "uvent/tasks/AwaitableFrame.h"

/**
 * \file Blocking.h
 * \brief `co_await blocking::run(f)` — run a blocking function on a separate thread pool and resume the coroutine
 *        on its own worker with the result.
 *
 * The pool is process-wide and lazy: threads are created when a job arrives and no thread is idle, up to
 * `settings::blocking_threads_max`, and a thread that stays idle for `settings::blocking_idle_timeout_ms` exits.
 * Completion is handed back through the owning worker's inbox (`co_spawn_static`), the same path a cross-worker
 * wake-up takes; the pool thread never touches the coroutine.
 *
 * A running job cannot be interrupted: cancellation of the awaiting task does not stop the function, and
 * `co_await` returns only when it has finished (the closure may reference the caller's frame). Check
 * `this_coroutine::cancel_requested()` before submitting long work. Exceptions thrown by `f` are rethrown in
 * the awaiting coroutine. Must be awaited from a runtime worker (a coroutine or a fiber on a worker).
 *
 * Cost: two cross-thread hops per call (~10 µs of latency and CPU on an idle pool, see docs/blocking.md), so
 * this is the fallback for work that genuinely blocks, not a way to make cheap syscalls asynchronous.
 */
namespace usub::uvent::blocking
{
    namespace detail
    {
        struct Job
        {
            Job* next{nullptr};
            std::coroutine_handle<> waiter{};
            int origin_tid{-1};

            virtual void run() noexcept = 0;

        protected:
            ~Job() = default;
        };

        /// Submit a job; the pool calls `job->run()` on one of its threads and then resumes `job->waiter`.
        void submit(Job* job);

        /// Wait until no job is queued or running (used when a runtime goes away under the pool).
        void wait_idle();

        /// Current number of pool threads (tests / introspection).
        std::size_t thread_count() noexcept;
    } // namespace detail

    template <class F>
    class RunAwaiter final : public detail::Job
    {
        using R = std::invoke_result_t<F&>;
        static constexpr bool kVoid = std::is_void_v<R>;
        using Stored = std::conditional_t<kVoid, char, R>;

        F fn_;
        std::optional<Stored> result_;
        std::exception_ptr exc_{};

    public:
        explicit RunAwaiter(F fn) : fn_(std::move(fn)) {}

        RunAwaiter(const RunAwaiter&) = delete;
        RunAwaiter& operator=(const RunAwaiter&) = delete;
        RunAwaiter(RunAwaiter&&) = delete;
        RunAwaiter& operator=(RunAwaiter&&) = delete;

        bool await_ready() const noexcept { return false; }

        template <class Promise>
        void await_suspend(std::coroutine_handle<Promise> h)
        {
            this->waiter = h;
            this->origin_tid = system::this_thread::detail::t_id;
            uvent::detail::frame_of(h).arm_cancel(nullptr, nullptr, "blocking"); // wait reason for introspection
            detail::submit(this);
        }

        R await_resume()
        {
            uvent::detail::frame_of(this->waiter).disarm_cancel();
            if (this->exc_)
                std::rethrow_exception(this->exc_);
            if constexpr (!kVoid)
                return std::move(*this->result_);
        }

        void run() noexcept override
        {
            try
            {
                if constexpr (kVoid)
                {
                    this->fn_();
                    this->result_.emplace(0);
                }
                else
                    this->result_.emplace(this->fn_());
            }
            catch (...)
            {
                this->exc_ = std::current_exception();
            }
        }
    };

    /// \brief `co_await blocking::run([&] { return ::pread(...); })`.
    template <class F>
    [[nodiscard]] RunAwaiter<std::decay_t<F>> run(F&& f)
    {
        return RunAwaiter<std::decay_t<F>>(std::forward<F>(f));
    }
} // namespace usub::uvent::blocking

#endif // UVENT_BLOCKING_BLOCKING_H
