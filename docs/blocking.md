# Blocking pool (`uvent/blocking/Blocking.h`)

`co_await blocking::run(f)` runs `f` on a separate thread pool and resumes the coroutine on **its own worker** with
the result — the `tokio::task::spawn_blocking` shape, as an awaiter.

```cpp
#include "uvent/blocking/Blocking.h"

task::Awaitable<std::string> load(std::string path)
{
    // anything that blocks: a legacy client library, getaddrinfo, a synchronous DB driver, a slow syscall
    co_return co_await blocking::run([p = std::move(path)] { return legacy::read_whole_file(p); });
}
```

`f` may return a value (moved out to the caller), `void`, or throw — the exception is rethrown by `co_await`. The
call must be made from a runtime worker (a coroutine, or a fiber through `fiber::await(blocking::run(...))`).

## Semantics

|              |                                                                                                                                                                                                                                                                                            |
|--------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Threads      | process-wide, lazy: created when a job arrives and nobody is idle, up to `settings::blocking_threads_max` (0 = `min(512, 4 × hardware threads)`); a thread idle for `settings::blocking_idle_timeout_ms` (10 s) exits                                                                      |
| Queue        | FIFO, unbounded; check `this_coroutine::cancel_requested()` before submitting long work                                                                                                                                                                                                    |
| Resume       | through the owning worker's inbox (`co_spawn_static(h, origin_tid)`), the same path as a cross-worker wake-up — the pool thread never touches the coroutine                                                                                                                                |
| Cancellation | a running job **cannot be interrupted**: cancelling the awaiting task does not stop `f`, and `co_await` returns only when `f` has finished, because the closure may reference the caller's frame. The awaiter is armed with a wait reason (`"blocking"`) so introspection and drain see it |
| Shutdown     | `Uvent::stop()` does not wait for jobs; the pool's static destructor does (running jobs finish, then threads exit)                                                                                                                                                                         |

## Cost — and why it is the fallback, not the engine

Two cross-thread hops per call (push + futex wake on the pool side, inbox + `eventfd` wake on the worker side),
plus a context switch on each end. On this host (Xeon E5-2640 v4, `io_perfomance/fs_bench`):

| operation, 1 caller, queue depth 1 | inline on the worker | through the pool                              |
|------------------------------------|----------------------|-----------------------------------------------|
| cached 4 KiB `pread`               | 1.2 µs, 1.3 CPU-µs   | 11 µs, 12 CPU-µs (9× slower, 9× the CPU)      |
| cold 4 KiB read (NVMe, ~111 µs)    | —                    | device + 15–30 µs wake-up latency, 2× the CPU |

That is why `uvent/fs` uses the pool only for what cannot be done inline or through io_uring (`open`, `stat`,
`close`, `ftruncate`, directory work, and every file operation on macOS / Windows / Linux without io_uring), see
`fs.md`. For plain CPU work prefer a fiber or a coroutine on the runtime; for I/O that has a non-blocking form,
use it.

Tests: `tests/test_blocking.cpp` (value / void / exception, 900 jobs from three workers each resuming on its own
worker, lazy creation and idle retirement, `fiber::await`).
