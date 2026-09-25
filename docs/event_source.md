# EventSource (`uvent/poll/EventSource.h`)

A readiness source that is not a socket – signal pipe, `eventfd`, `timerfd`, `pidfd`, `inotify`, a pipe to a child –
watched by a worker's poller and delivered through a plain callback on that worker.

```cpp
struct alignas(8) EventSource
{
    enum Ready : uint32_t { READABLE = 1, WRITABLE = 2, HUP = 4, ERR = 8 };
    using Callback = void (*)(EventSource* self, uint32_t ready) noexcept;

    socket_fd_t fd;         // the descriptor (unused on Windows)
    Callback    on_ready;   // invoked inside PollerImpl::poll() on the registering worker
    void*       user;       // yours
    bool        edge;       // EPOLLET / EV_CLEAR instead of level-triggered
};

// on the worker whose poller should watch it:
system::this_thread::detail::pl.addSource(&src, core::READ);   // READ | WRITE | ALL
system::this_thread::detail::pl.removeSource(&src);            // never closes src.fd
```

The callback runs on the polling worker; typically it drains the descriptor and wakes a parked coroutine (an
`AsyncEvent::set()`, a `resume_waiter`), which then continues on *its* worker through the usual cross-worker path.
`tests/test_event_source.cpp` shows the patterns; `src/signal/Signal.cpp` is the first real user.

## Readiness contract

The callback is invoked at least once after every new readiness of the descriptor and must drain what it wants
(read until `EAGAIN`). Whether **stale** readiness is reported again on the next poll is backend-specific:

| backend  | behaviour                                                                                                                                           |
|----------|-----------------------------------------------------------------------------------------------------------------------------------------------------|
| epoll    | level-triggered by default (reported on every poll while unread); `edge = true` → `EPOLLET`                                                         |
| kqueue   | same, `edge = true` → `EV_CLEAR`                                                                                                                    |
| io_uring | multishot `POLL_ADD`: fires once per wake-up of the descriptor, i.e. edge-like regardless of `edge`                                                 |
| IOCP     | no readiness at all: `addSource` / `removeSource` are no-ops, events are pushed with `IocpPoller::post(&src, value)` and `value` arrives as `ready` |

Portable callbacks therefore always drain.

## Lifetime

Register and remove on the owning worker (the poller is `thread_local` in the default `UVENT_ENABLE_REUSEADDR`
layout – hand the call to the right worker with `co_spawn_static(coro, tid)` when you are elsewhere). Remove before
destroying the source. On io_uring the cancellation completes asynchronously: the source must still exist when the
`-ECANCELED` CQE is processed, in practice until the next `poll()` on that worker – keep sources long-lived, or
destroy them one loop iteration after `removeSource`.

## Cost

The poller tells a source from a socket header by the low bit of the pointer it stores in `epoll_data` /
`kevent.udata` / the IOCP completion key (`SocketHeader` is `alignas(32)`, `EventSource` `alignas(8)`). That is one
predictable branch per dispatched event and nothing else. Measured with the `io_perfomance` echo benchmark
(1000 keep-alive connections, 30 s, 3 runs per cell, 6 for epoll/1 thread, same host and method as the README table)
against the same tree without
the change:

|           | uvent epoll                                                                                                                    | uvent io_uring             |
|-----------|--------------------------------------------------------------------------------------------------------------------------------|----------------------------|
| 1 thread  | 114,074 → 113,526 RPS (−0.5 %, median of 6 interleaved pairs; the very first run of the session was a 106k cold-start outlier) | 141,086 → 141,546 (+0.3 %) |
| 2 threads | 220,367 → 221,134 (+0.3 %)                                                                                                     | 290,424 → 289,851 (−0.2 %) |

Within the ±3 % run-to-run noise of that benchmark.
