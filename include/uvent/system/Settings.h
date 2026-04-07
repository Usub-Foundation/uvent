//
// Created by kirill on 12/2/24.
//

#ifndef UVENT_SETTINGS_H
#define UVENT_SETTINGS_H

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
     * @brief Idle wait duration in milliseconds for worker threads.
     *
     * Defines how often an idle worker thread wakes up to check for new tasks
     * when no work is currently available in its queue.
     */
    extern int idle_fallback_ms;

    /**
     * @brief Per-thread socket header pool capacity.
     *
     * Must be set before Uvent::run() — each worker thread reads this value
     * once at startup via SocketHeaderPool::init(). Changes after run() have
     * no effect. Set to 0 to disable pooling (all allocations go to new/delete).
     */
    extern int per_thread_socket_header_pool_size;
} // namespace usub::uvent::settings

#endif // UVENT_SETTINGS_H
