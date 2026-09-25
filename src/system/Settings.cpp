//
// Created by kirill on 12/2/24.
//

#include "uvent/system/Settings.h"

namespace usub::uvent::settings
{
    int tw_levels = 4;
    uint64_t timeout_duration_ms = 20000;
    int max_read_retries = 100;
    int max_write_retries = 100;
    int max_pre_allocated_timer_wheel_operations_items = 256;
    int max_pre_allocated_tasks_items = 1024;
    int max_pre_allocated_tmp_sockets_items = 1024;
    int max_pre_allocated_tmp_coroutines_items = 256;
    std::size_t max_transfer_stack_depth = 512 * 1024;
    int32_t coop_budget = 128;
    int idle_fallback_ms = 50;
    std::size_t loop_task_quantum = 4096;
    uint64_t stop_drain_timeout_ms = 5000;
    int resolver_threads = 2;
    std::size_t fiber_stack_size = 256 * 1024;
    std::size_t fiber_stack_cache_per_thread = 16;
    std::size_t blocking_threads_max = 0;
    uint64_t blocking_idle_timeout_ms = 10000;
    bool fs_inline_nowait_read = true;
    bool fs_inline_buffered_write = true;
} // namespace usub::uvent::settings
