# Signals (`uvent/signal/Signal.h`)

Process signals as awaitables, the way `tokio::signal` does it:

```cpp
#include "uvent/Uvent.h"
#include "uvent/signal/Signal.h"

task::Awaitable<void> serve(usub::Uvent* rt)
{
    signal::SignalSet stop{SIGINT, SIGTERM};
    const int s = co_await stop.recv();      // -1 when the task is cancelled
    spdlog::info("signal {} – shutting down", s);
    rt->stop();
}

int main()
{
    usub::Uvent rt(4);
    system::co_spawn_static(serve(&rt), 0);
    rt.run();
}
```

or, for the common k8s case, one line after constructing the runtime:

```cpp
usub::Uvent rt(4);
signal::stop_on(rt, {SIGINT, SIGTERM});   // 1st signal: rt.stop() (drain if enabled), 2nd: stop at once
rt.run();
```

## API

|                                                |                                                                                |
|------------------------------------------------|--------------------------------------------------------------------------------|
| `signal::SignalSet set{SIGUSR1, SIGUSR2};`     | subscribe to several numbers                                                   |
| `co_await set.recv()` → `int`                  | next pending number (lowest first), `-1` if the task was cancelled             |
| `set.try_recv()` → `int`                       | same without waiting, `-1` if nothing is pending                               |
| `signal::Signal s(SIGINT);`                    | one number; `co_await s.recv()` → `bool` (`false` = cancelled), `s.try_recv()` |
| `co_await signal::ctrl_c()`                    | one-shot: next SIGINT / Ctrl-C                                                 |
| `co_await signal::wait_any({SIGINT, SIGTERM})` | one-shot: which one came                                                       |
| `signal::stop_on(rt, {…})`                     | stop the runtime on the first delivery, hard-stop on the second                |
| `signal::ignore(sig)`                          | `SIG_IGN` (SIGPIPE is already ignored by the library at load time)             |
| `signal::reset_to_default(sig)`                | back to `SIG_DFL`, subscriptions to `sig` are dropped                          |

`recv()` is an ordinary awaitable: `co_await` it, `fiber::await` it, put it into `select()`, cancel it through a
`TaskScope` / `JoinHandle` or a runtime drain – it then returns `-1` / `false` instead of waiting forever.

## Semantics

- A receiver sees every delivery that happens **after** it was constructed. Several deliveries of the same number
  before `recv()` is called collapse into one (the kernel does not queue standard signals either).
- A delivery **before** any receiver for that number exists is lost – and the default disposition ran, so a SIGTERM
  before the first `SignalSet` still kills the process. Subscribe early: right after `Uvent rt(n)`.
- Every receiver subscribed to a number is notified (broadcast), not just one.
- Installing the handler replaces the default disposition for the rest of the process: after the first
  `Signal s(SIGINT)`, Ctrl-C no longer kills the program, even after `s` is destroyed. `reset_to_default()` undoes it.
- `SIGKILL` / `SIGSTOP` and numbers outside `1..127` throw `std::invalid_argument`.
- Constructing a receiver requires the runtime to exist (`Uvent rt(n)` before it); `run()` may start later.

## How it works

One `sigaction` handler per subscribed number. The handler does the single async-signal-safe thing it needs to: it
writes the signal number as one byte into a non-blocking self-pipe (`SA_RESTART`, `errno` preserved). The pipe's read
end is a `poll::EventSource` registered with **worker 0's** poller (see `docs/event_source.md`); when it becomes
readable the worker drains it and, under the registry mutex, marks the pending bit in every subscribed `SignalSet` and
sets its auto-reset `AsyncEvent`. Parked receivers resume on their own workers through the normal cross-worker wake
path. There is no dedicated thread and no `pthread_sigmask`: other threads and libraries in the process are unaffected,
and nothing has to run before the runtime is constructed.

Cost: zero on the hot path – one more descriptor in worker 0's poll set, touched only when a signal arrives.

`stop_on()` is spawned with `co_spawn_static`, not `task::spawn`, on purpose: a runtime drain cancels registered
tasks, and the stopper must survive the drain to escalate on the second signal.

## Platforms

- **Linux / BSD / macOS**: `sigaction` + pipe, identical on epoll, io_uring (multishot `POLL_ADD` on the pipe) and
  kqueue.
- **Windows**: `SetConsoleCtrlHandler`. `CTRL_C_EVENT` is reported as `SIGINT`, `CTRL_BREAK_EVENT` as `SIGBREAK`,
  `CTRL_CLOSE / LOGOFF / SHUTDOWN` as `SIGTERM`; the handler posts to worker 0's IOCP. Other numbers are accepted and
  never fire. Windows terminates the process a few seconds after a `CTRL_CLOSE` handler returns, so whatever
  `stop_on()` triggers must be quick. Compile-tested only.

## Tests

`tests/test_signal.cpp` (POSIX): raised from a coroutine and from a foreign thread, coalescing, a set over two numbers,
broadcast to three receivers, cancellation through `TaskScope`, delivery before `recv()`, `stop_on()` stopping a
2-worker runtime, rejected numbers.
