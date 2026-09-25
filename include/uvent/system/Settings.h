//
// Created by kirill on 12/2/24.
//

#ifndef UVENT_SETTINGS_H
#define UVENT_SETTINGS_H

#include <cstddef>
#include <cstdint>

namespace usub::uvent::settings
{
    /**
     * \brief Timer wheel levels.
     * This variable defines the number of hierarchical levels in the timer wheel.
     * Each level represents a range of time buckets for scheduling timers efficiently.
     */
    extern int tw_levels;

    /**
     * \brief Connection timeout duration.
     * This variable specifies the maximum duration (in milliseconds) that a client can remain connected.
     * If no activity is detected from the client within this time frame, the connection will be automatically closed.
     * The default value is set to 20,000 milliseconds (20 seconds).
     */
    extern uint64_t timeout_duration_ms;

    /**
     * \brief Maximum number of read retries on EINTR.
     * Defines how many consecutive EINTR errors are allowed during a read operation
     * before giving up. Prevents infinite loops caused by repeated signal interruptions.
     */
    extern int max_read_retries;

    /**
     * \brief Maximum number of write retries on EINTR.
     * Defines how many consecutive EINTR errors are allowed during a write operation
     * before the operation is aborted. Prevents hangs due to persistent signal interruptions.
     */
    extern int max_write_retries;

    /**
     * @brief Maximum number of pre-allocated operation items for the timer wheel.
     *
     * Defines how many timer operations (add/update/delete) can be batched
     * and processed per iteration to reduce allocation overhead.
     */
    extern int max_pre_allocated_timer_wheel_operations_items;

    /**
     * @brief Maximum number of task items fetched from the local task queue in one batch.
     *
     * Controls how many pending tasks are dequeued and executed at once
     * to balance throughput and scheduling latency.
     */
    extern int max_pre_allocated_tasks_items;

    /**
     * @brief Maximum number of socket items fetched in one batch for cleanup.
     *
     * Determines how many sockets are collected and processed together
     * during deferred socket cleanup cycles.
     */
    extern int max_pre_allocated_tmp_sockets_items;

    /**
     * @brief Maximum number of coroutine items fetched in one batch for cleanup.
     *
     * Specifies how many finished coroutine handles are grouped and destroyed
     * per cleanup iteration.
     */
    extern int max_pre_allocated_tmp_coroutines_items;

    /**
     * @brief Stack depth (bytes) past which symmetric coroutine transfers are
     * bounced through the scheduler instead of continued inline.
     *
     * Without optimisations (-O0/-O1, sanitizer builds) the compiler does not
     * turn `await_suspend`/`final_suspend` handle returns into tail calls, so
     * long chains of synchronously completing awaits nest the native stack.
     * When the current depth from the worker's stack base exceeds this value
     * the continuation is enqueued into the thread-local run queue instead.
     */
    extern std::size_t max_transfer_stack_depth;

    /**
     * @brief Cooperative budget: number of fast-path completions a coroutine may
     * take per scheduler resume before it is forced through the run queue.
     *
     * Bounds how long one hot coroutine (a pipelined socket, a busy channel
     * consumer) can monopolise its worker. Checked by channel operations and
     * socket reads/writes via system::coop::consume().
     */
    extern int32_t coop_budget;

    /**
     * @brief Idle wait duration in milliseconds for worker threads.
     *
     * Defines how often an idle worker thread wakes up to check for new tasks
     * when no work is currently available in its queue.
     */
    extern int idle_fallback_ms;

    /**
     * @brief Upper bound on coroutines a worker resumes from its run queue per
     * event-loop iteration. Tasks that re-queue themselves (yield loops, hot
     * channels) would otherwise keep the inner drain loop busy forever and
     * starve the poller, the timer wheel, the inbox and cancel kicks of that
     * worker. Leftover tasks are picked up on the next iteration; the poll in
     * between is non-blocking while the queue is not empty.
     */
    extern std::size_t loop_task_quantum;

    /**
     * @brief UVENT_RUNTIME_DRAIN only. How long Uvent::stop() lets live tasks
     * unwind cooperatively (every registered task gets request_cancel()) before
     * the workers exit anyway. 0 = stop immediately (legacy behaviour).
     */
    extern uint64_t stop_drain_timeout_ms;

    /**
     * @brief Number of blocking resolver threads serving net::async_resolve.
     *
     * Read once, lazily, when the first non-numeric resolve is submitted.
     */
    extern int resolver_threads;

    /**
     * @brief Default usable stack size (bytes) of a fiber created with
     * fiber::run() when Options::stack_size is 0. Rounded up to whole pages;
     * a guard page is added on top of it.
     */
    extern std::size_t fiber_stack_size;

    /**
     * @brief How many released fiber stacks each worker keeps for reuse.
     * 0 disables caching (every fiber maps and unmaps its own stack).
     */
    extern std::size_t fiber_stack_cache_per_thread;

    /**
     * @brief Upper bound on threads of the blocking pool (blocking::run, fs fallbacks).
     * 0 = min(512, 4 × hardware threads). Threads are created on demand.
     */
    extern std::size_t blocking_threads_max;

    /**
     * @brief A blocking-pool thread that has had no job for this long exits.
     */
    extern uint64_t blocking_idle_timeout_ms;

    /**
     * @brief fs::File::async_read_at first tries preadv2(RWF_NOWAIT) on the calling worker: a page-cache hit
     * completes inline (~1 µs) instead of paying an io_uring round trip or two thread hops. Linux only.
     */
    extern bool fs_inline_nowait_read;

    /**
     * @brief fs::File::async_write_at performs buffered (non-O_DIRECT, non-O_SYNC) writes inline with pwrite on
     * the calling worker. A buffered write normally lands in the page cache in ~1 µs; io_uring cannot do it
     * without blocking on ext4 and punts every such write to its kernel thread pool (measured ~7 µs), a
     * user-space pool costs ~10 µs. The price is a rare stall when the kernel throttles dirty pages; set to
     * false to route these writes through io_uring / the blocking pool instead.
     */
    extern bool fs_inline_buffered_write;
} // namespace usub::uvent::settings

#endif // UVENT_SETTINGS_H
