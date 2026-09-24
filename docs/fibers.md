# Fibers (stackful)

`uvent/fiber/Fiber.h` adds **stackful fibers** next to the C++20 coroutines.
A fiber is plain blocking-style code running on its own stack; it is hosted
by an ordinary `task::Awaitable`, so the scheduler, sockets, timers and
synchronisation primitives do not know fibers exist. Nothing in the existing
API changes — fibers are one more way to write a task.

```cpp
#include "uvent/Uvent.h"   // includes uvent/fiber/Fiber.h when UVENT_ENABLE_FIBERS is on

using namespace usub::uvent;

task::spawn(fiber::run([&] {
    net::TCPServerSocket acceptor{"0.0.0.0", 8080};
    auto client = fiber::await(acceptor.async_accept());      // blocks the fiber only
    uint8_t buf[512];
    auto n = fiber::await(client->async_read(buf, sizeof buf));
    fiber::await(system::this_coroutine::sleep_for(100ms));
    legacy_code_with_deep_call_stack();                        // no co_await anywhere
}));
```

## API

| Function                           | Meaning                                                                                                                                                                                                                               |
|------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `fiber::run(F body, Options = {})` | Returns `task::Awaitable<R>` where `R` is `body()`'s result. `co_await` it, `task::spawn` it, put it into a `TaskScope` — it is a normal task. The body's return value / exception becomes the task's.                                |
| `fiber::await(awaitable)`          | Inside a fiber (any call depth): suspends the fiber on any uvent awaitable — `Awaitable<T>`, `JoinHandle`, socket / timer / mutex / channel awaiters — and returns what `co_await` would return. Exceptions propagate into the fiber. |
| `fiber::yield()`                   | Lets other tasks on the worker run.                                                                                                                                                                                                   |
| `fiber::in_fiber()`                | `true` when called from a fiber body.                                                                                                                                                                                                 |
| `Options::stack_size`              | Usable stack bytes for this fiber; `0` = `settings::fiber_stack_size` (256 KiB).                                                                                                                                                      |

Because the host is a regular task, `JoinHandle::cancel()`, `TaskScope`,
`system::this_coroutine::cancel_requested()`, trace ids, names and
introspection all work unchanged from inside the fiber.

## How it works

`fiber::run` is a coroutine that owns the fiber's stack. It switches into the
fiber; when the fiber calls `fiber::await(x)` it parks a type-erased view of
`x`'s awaiter (living on the fiber stack) and switches back. The host then
`co_await`s that awaiter on the worker's native stack and, once resumed by the
scheduler, switches back into the fiber. One await costs two context switches
and no heap allocation.

Platform back-ends:

* **Linux, *BSD, macOS — x86_64 and aarch64**: hand-written switch
  (`src/fiber/switch_*.S`), `mmap`ed stack with a guard page, per-thread stack
  cache (`settings::fiber_stack_cache_per_thread`).
* **Windows — x64 and ARM64**: Win32 fibers (`CreateFiberEx` /
  `SwitchToFiber`).
* **ASan / TSan** builds annotate every switch
  (`__sanitizer_start_switch_fiber`, `__tsan_switch_to_fiber`), so sanitized
  tests run clean.

Disable with `-DUVENT_ENABLE_FIBERS=OFF` (the header and sources are then not
built; `UVENT_ENABLE_FIBERS` is not defined).

## Rules and limitations

* `fiber::await` outside a fiber throws `std::logic_error`.
* Do **not** `fiber::await` inside a `catch` handler or a destructor. The C++
  runtime keeps per-thread exception state that is not fiber-aware
  (coroutines forbid `co_await` there at compile time; fibers cannot).
* If the host task is destroyed while the fiber is suspended, the pending
  `fiber::await` throws `fiber::ForcedUnwind` so RAII objects on the fiber
  stack are destroyed. Never swallow it (a `catch (...)` that continues will
  get it re-thrown on the next `await`).
* Stack overflow hits the guard page and kills the process. Deep recursion or
  large locals need a bigger `Options::stack_size`.
* Fibers, like coroutines, stay on the worker that resumed them unless an
  awaiter explicitly hops threads. Do not cache addresses of `thread_local`
  objects across an `await`.

## Cancellation and shutdown

A fiber is cancelled exactly like a coroutine task: `JoinHandle::cancel()` or
`TaskScope::cancel_and_join()` sets the flag and kicks the worker. The pending
`fiber::await` then returns the cancelled result of its awaiter (`sleep_for`
returns `false`, socket operations return `-1` / `ECANCELED`, channels return
empty), and `system::this_coroutine::cancel_requested()` is `true` inside the
fiber body. Two rules follow:

* A loop must look at the result or at `cancel_requested()`. After
  cancellation `sleep_for` returns `false` **without suspending**, so a loop
  that ignores it spins the worker.
* `fiber::yield()` always suspends and re-queues the fiber, which makes it the
  right way to poll for cancellation from CPU-bound code.

`Uvent::stop()` built with `UVENT_RUNTIME_DRAIN` cancels every live task; a
fiber parked in `fiber::await` unwinds normally and the destructors on its
stack run. Without the option a parked fiber is abandoned together with its
stack (harmless at process exit). See `docs/drain.md`.

## Examples and stress test

* `examples/main_fibers.cpp` – a walkthrough: fiber echo server and clients,
  `AsyncMutex` from fibers on several workers, deep recursion on a 1 MiB
  stack, cancellation of a parked fiber, shutdown with a parked fiber.
* `examples/main_fiber_soak.cpp` – the soak used to validate the runtime:
  thousands of fibers per round on 8 workers mixing timers, `yield`, mutexes,
  channels, nested fibers, cross-worker joins, exceptions, cancellation storms,
  a loopback TCP echo with fibers on both ends and stack churn. Environment:
  `UVENT_SOAK_SECONDS`, `UVENT_SOAK_WORKERS`, `UVENT_SOAK_FIBERS`,
  `UVENT_SOAK_PORT`, `UVENT_SOAK_SHUTDOWN_PARKED=1`, `UVENT_SOAK_ONLY=cancel`,
  `UVENT_SOAK_CANCEL_CORO=1`. Run it under ASan and TSan before touching the
  scheduler or the fiber switch (see `docs/contributing.md`).
* `tests/test_fiber.cpp` – the unit tests (basics, exceptions, structured
  concurrency, cancellation, deep recursion, stress, forced unwind, misuse,
  sockets).
