# uvent Settings

This document describes all configurable runtime settings available in **uvent**.
All settings are declared in `usub::uvent::settings` and can be modified at application startup.
Default values come from the current implementation.

---

## Timer Wheel

### `tw_levels`

**Type:** `int`
**Default:** `4`

Defines the number of hierarchical levels in the timer wheel.
Each level expands the timer resolution exponentially.

Maximum representable timeout:

```
max_timeout = 256^(tw_levels - 1)
```

With the default `tw_levels = 4`, the maximum range becomes:

```
256^(4 - 1) = 256^3 = 16,777,216 ticks
```

---

## Connection Handling

### `timeout_duration_ms`

**Type:** `uint64_t`
**Default:** `20000` ms (20 seconds)

Specifies the maximum allowed inactivity time for a client connection.
When this time elapses without activity, the connection is closed.

---

## EINTR Retry Behavior

### `max_read_retries`

**Type:** `int`
**Default:** `100`

Maximum number of consecutive `EINTR` interruptions allowed during a read operation before it is considered failed.

### `max_write_retries`

**Type:** `int`
**Default:** `100`

Maximum number of consecutive `EINTR` interruptions tolerated during a write operation.

---

## Timer Wheel Internal Buffers

### `max_pre_allocated_timer_wheel_operations_items`

**Type:** `int`
**Default:** `256`

Defines how many timer operations (add/update/delete) can be batched and processed per iteration.

---

## Task Scheduling Buffers

### `max_pre_allocated_tasks_items`

**Type:** `int`
**Default:** `1024`

Maximum number of queued tasks pulled and executed in a single batch.

---

## Socket Cleanup Buffers

### `max_pre_allocated_tmp_sockets_items`

**Type:** `int`
**Default:** `1024`

Specifies how many sockets are batched together during deferred cleanup cycles.

---

## Coroutine Cleanup Buffers

### `max_pre_allocated_tmp_coroutines_items`

**Type:** `int`
**Default:** `256`

Defines the maximum number of completed coroutine handles destroyed in one cleanup pass.

---

## Coroutine Hand-off Stack Guard

### `max_transfer_stack_depth`

**Type:** `std::size_t`
**Default:** `512 * 1024` bytes

Coroutine hand-offs (`co_await` into a child, return to the awaiting parent
on completion, `co_yield`) use symmetric transfer. Optimising compilers turn
that into a tail call, so a long chain of synchronously completing awaits runs
on a flat native stack. Without optimisations (`-O0`/`-O1`, sanitizer builds)
every hand-off nests a `resume()` call instead, and such chains overflow the
stack.

Worker threads record their stack base at start-up and compare it against the
current stack pointer at each hand-off. Past this depth the continuation is
pushed to the worker's local run queue and resumed from the scheduler with a
fresh stack. The check is a thread-local load and a subtraction; on optimised
builds it effectively never triggers.

Only runtime workers perform the check – threads outside the runtime have no
run queue to fall back to.

---

## Cooperative Scheduling

### `coop_budget`

* **Type:** `int32_t`
* **Default:** `128`

Number of immediately-ready fast-path completions (channel receives/sends, socket reads/writes) a coroutine may take
per scheduler resume before it is forced through the run queue via `this_coroutine::yield()`. Bounds how long one hot
coroutine can monopolise its worker; lower values improve tail latency of neighbours at a small throughput cost.

```cpp
usub::uvent::settings::coop_budget = 64;
```

---

## Worker Thread Idle Behavior

### `idle_fallback_ms`

**Type:** `int`
**Default:** `50` ms

Idle worker threads wake up at this interval to check for new tasks when their local queues are empty.

This is a fallback, not the wake latency for cross-thread work: resolver
completions and inbox pushes signal the parked poller directly through its
wake channel (`eventfd` / `EVFILT_USER` / `PostQueuedCompletionStatus`, see
[Poller wake](resolver.md#poller-wake)) and are picked up near-instantly.

---

## DNS Resolver

### `resolver_threads`

**Type:** `int`
**Default:** `2`

Size of the blocking `getaddrinfo` worker pool used by `net::async_resolve`
(see [Name Resolution & Happy Eyeballs](resolver.md)).

The pool is created lazily on the first non-numeric lookup; IP literals are
resolved inline and never touch it. Note that `net::connect_happy` issues two
lookups per call (AAAA and A in parallel), so under a burst of concurrent
`connect_happy` calls against a slow DNS server the default pool of 2 becomes
the bottleneck – raise this value if that is a real workload for you.

---

## Scheduler Fairness

### `loop_task_quantum`

**Type:** `std::size_t`
**Default:** `4096`

Upper bound on coroutines a worker resumes from its run queue per event-loop
iteration. Tasks that re-queue themselves (a `yield()` loop, a hot channel)
would otherwise keep the inner drain loop busy forever and starve the poller,
the timer wheel, the inbox and cancel kicks of that worker. Leftover tasks are
picked up on the next iteration; the poll in between is non-blocking while the
queue is not empty.

```cpp
usub::uvent::settings::loop_task_quantum = 1024; // more frequent poll/timer ticks under load
```

---

## Fibers

### `fiber_stack_size`

**Type:** `std::size_t`
**Default:** `256 * 1024`

Usable stack bytes of a fiber created with `fiber::run()` when
`fiber::Options::stack_size` is `0`. Rounded up to whole pages; a guard page is
added on top. Deep recursion or large locals need more (see `docs/fibers.md`).

### `fiber_stack_cache_per_thread`

**Type:** `std::size_t`
**Default:** `16`

How many released fiber stacks each worker keeps for reuse. `0` disables the
cache (every fiber maps and unmaps its own stack).

---

## Runtime Drain

### `stop_drain_timeout_ms`

**Type:** `uint64_t`
**Default:** `5000`

Only with `-DUVENT_RUNTIME_DRAIN=ON`. How long `Uvent::stop()` lets live tasks
unwind cooperatively (every registered task gets `request_cancel()`) before the
workers exit anyway. `0` restores the legacy immediate stop. See `docs/drain.md`.
