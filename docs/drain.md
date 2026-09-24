# Runtime drain (`UVENT_RUNTIME_DRAIN`)

By default `Uvent::stop()` only asks the workers to leave their loops. A task
that is parked at that moment — on a timer, a socket, a mutex, a channel, a
`JoinHandle` — is never resumed and never destroyed: its coroutine frame leaks,
and for a fiber (`fiber::run`) so does the fiber stack, and no destructor on
that stack runs. This is harmless when `stop()` is followed by process exit,
and it is the cheapest possible hot path, so it stays the default.

`-DUVENT_RUNTIME_DRAIN=ON` adds a cooperative drain:

1. `Uvent::stop()` sets a deadline (`settings::stop_drain_timeout_ms`, default
   5000; `0` = legacy immediate stop) and wakes every worker. It returns at
   once — it is safe to call from inside a task.
2. Each worker calls `request_cancel()` on every task it spawned. Cancellation
   works exactly as for `JoinHandle::cancel()`: the kick wakes tasks parked on
   timers and sockets on any worker, `sleep_for` returns `false`, socket
   operations fail with `ECANCELED`, `cancel_requested()` turns `true`; scoped
   children are cancelled through the `TaskScope` tree; a fiber unwinds through
   its pending `fiber::await` like any other cancelled task.
3. The workers keep serving their loops while the tasks unwind. Worker 0 stops
   the runtime as soon as every registry is empty or the deadline passes.
4. Tasks that ignored cancellation until the deadline are left parked exactly
   as without the option; their number is available from
   `usub::Uvent::drain_survivors()` after `run()` returns.

## Cost

Every `task::spawn` / `TaskScope::spawn` appends the task's state pointer to a
plain `std::vector` owned by the spawning worker (one extra reference on the
freshly allocated state — no locks, no clocks, no shared cache lines). Done
entries are swept in place when the vector doubles, so the amortised cost is a
few nanoseconds per spawn. A spawn from a non-worker thread goes through a
mutex into worker 0's list (rare path). The `draining` flag checked once per
loop iteration is a relaxed load of a never-written-until-shutdown global.

Measured on `examples/main.cpp` (keep-alive HTTP, 4 workers, `wrk -t8 -c500`):
within run-to-run noise (< 0.5%). Compare `UVENT_TASK_INTROSPECTION`, which
registers every awaitable frame with a timestamp and a sharded spinlock and
costs about 2.4% on the same benchmark.

## Rules

* The drain relies on the worker loop staying fair: `settings::loop_task_quantum`
  bounds how many coroutines a worker resumes from its run queue per iteration,
  so a task that re-queues itself in a tight `yield` loop cannot starve the
  timer wheel, the inbox and the cancel kicks of that worker (found by
  `tests/test_drain.cpp`, covered by `test_coop_budget`).
* Tasks must be cancellation-aware to be drained: check
  `system::this_coroutine::cancel_requested()` in loops, treat a `false` from
  `sleep_for` and an error from a socket operation as "shut down".
* A fiber body that never awaits cannot be interrupted (see `docs/fibers.md`).
* Hard destruction of stragglers is intentionally not done: a frame whose leaf
  awaiter is still registered in a timer wheel or poller cannot be destroyed
  safely from outside.
* Structured shutdown is still the recommended pattern for services: keep a
  root `TaskScope`, `cancel_and_join()` it, then `stop()`. The drain is a safety
  net (and a leak-free shutdown for tests and sanitizer runs), not a substitute.
