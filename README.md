# uvent

**Cross-platform async I/O runtime for C++23 – coroutines end to end, structured concurrency, native backends, no Boost,
no dependencies beyond the standard library.**

[![Build](https://github.com/Usub-Foundation/uvent/actions/workflows/build.yml/badge.svg)](https://github.com/Usub-Foundation/uvent/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/tag/Usub-Foundation/uvent?label=release&sort=semver)](https://github.com/Usub-Foundation/uvent/tags)
[![Docs](https://img.shields.io/badge/docs-usub--foundation.github.io%2Fuvent-blue)](https://usub-foundation.github.io/uvent/)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus)](#requirements)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

```cpp
task::Awaitable<void> session(net::TCPClientSocket sock)
{
    utils::DynamicBuffer buf;
    for (;;)
    {
        buf.clear();
        ssize_t n = co_await sock.async_read(buf, 64 * 1024);
        if (n <= 0)
            break;
        co_await sock.async_write(buf.data(), buf.size());
    }
}

task::Awaitable<void> listen()
{
    net::TCPServerSocket acceptor{"0.0.0.0", 8080};
    for (;;)
        co_await acceptor.async_accept(session); // one coroutine per connection
}
```

## Why uvent

- **Coroutines are the API, not an add-on.** Every I/O operation is a `co_await`; there are no callback overloads, no
  handler types to erase, no manual state machines. Sync primitives, channels, timers and cancellation are all
  coroutine-native.
- **Structured concurrency, Go/Rust style.** `task::spawn` returns a `JoinHandle`, `TaskScope` is a nursery, and
  cancellation is a tree: cancel a parent and every sleep, socket read, channel wait and lock under it wakes up and
  returns. `sync::select` waits on channels, timers, tokens, events and semaphores at once.
- **Only the C++ standard library.** No Boost, no libuv, no third-party headers. One `FetchContent` line and you are in.
- **Native backend on every major OS** – `epoll` (edge-triggered) or `io_uring` on Linux, `kqueue` on macOS/BSD, IOCP on
  Windows – behind a single `TCPServerSocket` / `TCPClientSocket` / `UDPSocket` API.
- **Built for servers that stay up.** uvent runs the backend of a production: HTTP APIs, PostgreSQL,
  Redis and RPC traffic between ~10 services all flow through this runtime (see [Ecosystem](#ecosystem)). When something
  does hang, the opt-in [introspection](#introspection) layer dumps every live coroutine with its wait reason.

## Backends

| OS / family                                    | Backend primitives                                                           | Implementation                                                  |
|------------------------------------------------|------------------------------------------------------------------------------|-----------------------------------------------------------------|
| Linux                                          | `epoll` (edge-triggered), non-blocking sockets / **`io_uring` (optional)**   | `SocketLinux`, `SocketLinuxIOUring`, `EPoller`, `IOUringPoller` |
| macOS, FreeBSD, OpenBSD, NetBSD, DragonFly BSD | `kqueue`, non-blocking sockets (`accept` + `fcntl`)                          | `SocketBSD`, `KQueuePoller`                                     |
| Windows 10+ / Windows Server 2016+             | **IOCP** (`WSARecv` / `WSASend` / `AcceptEx` / `ConnectEx` / `TransmitFile`) | `SocketWindows`, `IocpPoller`                                   |

The backend is chosen automatically at build time: Linux → `epoll` (or `io_uring` with `-DUVENT_ENABLE_IO_URING=ON`,
kernel 5.1+ and [liburing](https://github.com/axboe/liburing)), Windows → IOCP, BSD/macOS → `kqueue`.

## Highlights

- **C++23 coroutines end to end** – `co_await` on accept/read/write/connect; `Awaitable<T>` with customizable frames.
- **Tasks & structured concurrency** – `task::spawn` → `JoinHandle<T>` (await the result, cancel the subtree, detach);
  `TaskScope` groups tasks so they are cancelled and joined together; plain `co_spawn` stays allocation-free for
  fire-and-forget.
- **Hierarchical cancellation** – `CancellationSource`/`CancellationToken` form a tree; a cancelled task's pending
  `sleep_for`, socket I/O, channel op, mutex or semaphore wait returns immediately with a clear result.
- **`select`** – Go-style `sync::select(ch.recv_op(), tok.on_cancel_op(), sync::sleep_op(5s))` over heterogeneous
  operations; one park, first branch wins, no double resumes.
- **Channels** – bounded MPMC `AsyncChannel<Ts...>` with back-pressure, and `AsyncUnboundedChannel<Ts...>` on a
  lock-free segmented queue for producers on non-runtime threads (logging callbacks, signal handlers).
- **Coroutine-native synchronization** – `AsyncMutex`, `AsyncSemaphore`, `AsyncEvent`, `WaitGroup`, `AsyncBarrier`;
  waiters suspend instead of blocking threads.
- **One acceptor per worker thread** – `async_accept(handler)` drains the whole kernel backlog per wakeup and spawns a
  coroutine per connection; with `SO_REUSEPORT` every worker owns its own listener, no accept lock, no lost edges in ET
  mode.
- **Async DNS + Happy Eyeballs (RFC 8305)** – `net::async_resolve` never blocks the loop; `net::connect_happy` races
  IPv6/IPv4 with staggered attempts and instant fallback.
- **Instant cross-thread wakeups** – parked pollers are signalled via `eventfd` / `EVFILT_USER` /
  `PostQueuedCompletionStatus`; a resume never waits for the idle tick.
- **Timer wheel** – millions of cheap one-shot timers, coroutine `sleep_for`, per-socket inactivity timeouts.
- **Cooperative budget** – a hot coroutine is forced through the scheduler after N fast-path completions, so one
  connection can't starve a worker.
- **Stackful fibers (opt-out)** – `fiber::run` hosts blocking-style code on its own stack; `fiber::await` parks the
  fiber
  on any awaitable, so legacy call stacks, deep recursion and third-party callbacks live next to coroutines with the
  same scheduler, cancellation and scopes (`docs/fibers.md`).
- **Runtime drain (opt-in)** – with `UVENT_RUNTIME_DRAIN`, `Uvent::stop()` cancels every live task and lets it unwind
  before the workers exit, for leak-free shutdown in tests and sanitizer runs (`docs/drain.md`).
- **Introspection (opt-in)** – `introspection::dump()` prints every live coroutine: name, wait reason, wait time, trace
  id, owning worker.
- **Lock-free internals** – QSBR and hazard-pointer reclamation, intrusive MPSC queues, sharded concurrent containers,
  pre-allocated operation pools.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(uvent
        GIT_REPOSITORY https://github.com/Usub-Foundation/uvent.git
        GIT_TAG v4.0.0)
FetchContent_MakeAvailable(uvent)
target_link_libraries(my_app PRIVATE usub::uvent)
```

Minimal HTTP keep-alive server, one acceptor per worker thread:

```cpp
#include "uvent/Uvent.h"

using namespace usub::uvent;

task::Awaitable<void> clientCoro(net::TCPClientSocket socket)
{
    static constexpr size_t max_read_size = 64 * 1024;
    static constexpr std::string_view response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "{\"status\":\"success\"}";

    utils::DynamicBuffer buffer;
    buffer.reserve(max_read_size);
    socket.set_timeout_ms(5000); // idle connections are closed by the timer wheel

    while (true)
    {
        buffer.clear();
        ssize_t rdsz = co_await socket.async_read(buffer, max_read_size);
        if (rdsz <= 0)
        {
            socket.shutdown();
            break;
        }
        socket.update_timeout(5000);
        ssize_t wrsz = co_await socket.async_write(
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(response.data())), response.size());
        if (wrsz <= 0)
            break;
    }
}

task::Awaitable<void> listeningCoro()
{
    net::TCPServerSocket acceptor{"0.0.0.0", 45900};
    for (;;)
        co_await acceptor.async_accept(clientCoro);
}

int main()
{
    usub::Uvent runtime(4); // 4 worker threads
    runtime.for_each_thread([&](int threadIndex, thread::ThreadLocalStorage*) {
        system::co_spawn_static(listeningCoro(), threadIndex);
    });
    runtime.run();
}
```

`async_accept` takes a coroutine function directly – every accepted connection is spawned as its own coroutine, no
manual
loop or `co_spawn` needed.

### Structured concurrency, `select` and cancellation

```cpp
sync::AsyncUnboundedChannel<Job> jobs;

task::Awaitable<uint64_t> worker(int id)
{
    system::this_coroutine::set_name("worker");
    uint64_t done = 0;
    for (;;)
    {
        auto r = co_await sync::select(jobs.recv_op(), sync::sleep_op(200ms));
        if (r.cancelled())          // our task (or an ancestor) was cancelled
            break;
        if (r.is<1>())              // 200 ms idle
            continue;
        auto& item = r.get<0>();    // std::optional<std::tuple<Job>>; nullopt = channel closed
        if (!item)
            break;
        done += process(std::get<0>(*item));
    }
    co_return done;
}

task::Awaitable<void> handler()
{
    task::TaskScope scope;                 // nursery, child of the current task
    scope.spawn(worker(0), /*thread*/ 0);
    scope.spawn(worker(1), /*thread*/ 1);
    auto producer = scope.spawn(produce(jobs));

    co_await producer;                     // JoinHandle: wait for one task
    co_await scope.cancel_and_join();      // cancel the rest, wait for all of them
}
```

Cancellation is cooperative and reaches every suspension point: a cancelled `sleep_for` returns `false`, socket ops
return `-1` with `errno == ECANCELED`, channel/select ops report `cancelled()`. Nothing is killed mid-flight.

### Connecting out: Happy Eyeballs

Dual-stack clients shouldn't hang on a broken IPv6 path. `connect_happy` implements RFC 8305: AAAA and A are resolved in
parallel and the addresses are raced – the first successful connect wins, a fast failure starts the next address
immediately:

```cpp
#include "uvent/net/HappyEyeballs.h"

task::Awaitable<void> fetch()
{
    auto res = co_await net::connect_happy("example.org", "443");
    if (!res)
        co_return; // res.error(): last ConnectError

    net::TCPClientSocket sock = std::move(*res);
    // sock.ipv says which family won; use async_write/async_read as usual
}
```

For a plain single-address connect, `TCPClientSocket::async_connect(host, port[, timeout])` resolves asynchronously and
never blocks the loop.

### Introspection

Build with `-DUVENT_TASK_INTROSPECTION=ON` and every live coroutine becomes visible – from a signal handler, an admin
endpoint or a watchdog coroutine:

```cpp
introspection::dump();   // frame, parent, name, wait reason ("socket.read", "channel.recv", "sleep"...), waiting_ns, trace id
auto tasks = introspection::snapshot();
```

`this_coroutine::set_name` / `set_trace_id` tag frames; trace ids are inherited by children. With the option off the
calls compile to no-ops, so call sites need no `#ifdef`.

See [`examples/`](examples) for structured concurrency, channels, `select`, timers, resolver and Happy Eyeballs demos.

## Benchmarks

HTTP/1.1 keep-alive echo (`wrk -c1000`, 30 s per run), the same 20-byte JSON answer from every server, no parsing –
this measures the event loop and the syscall path, nothing else. Each server runs one event loop per thread with its
own `SO_REUSEPORT` listener (the libuv server too), so all of them scale on the same terms.

| Threads | uvent (epoll) RPS | uvent (io_uring) RPS | Boost.Asio RPS | libuv RPS | uvent (epoll) p99 | uvent (io_uring) p99 | Boost.Asio p99 | libuv p99 |
|--------:|------------------:|---------------------:|---------------:|----------:|------------------:|---------------------:|---------------:|----------:|
|       1 |           112,594 |              149,538 |        124,480 |   112,182 |          10.86 ms |              8.44 ms |        9.46 ms |  11.13 ms |
|       2 |           220,534 |              307,174 |        222,923 |   232,590 |           5.43 ms |              3.93 ms |        5.41 ms |   5.42 ms |
|       4 |           370,127 |              364,552 |        373,909 |   356,399 |           2.73 ms |              2.76 ms |        2.84 ms |   4.38 ms |
|       8 |           549,151 |              562,466 |        485,974 |   559,106 |           2.01 ms |              1.96 ms |        2.30 ms |   2.41 ms |

Host: 1× Intel Xeon E5-2640 v4 (10 cores / 20 threads, 2.4 GHz), Linux 6.8, GCC 13.3, `-O3 -march=native` + LTO;
uvent 4.1.0, Boost 1.83, libuv 1.49.2, liburing 2.15. Measured 2026-09-23.
`wrk -t4 -c1000 -d30s --latency` (`-t8` for the 8-thread row), 3 s warm-up, server and `wrk` on the same host; for the
1/2/4-thread rows wrk is pinned to cores away from the workers and their SMT siblings, the 8-thread row cannot be
separated on 10 cores and runs with wrk free-floating.
Every cell is the **median of 3 runs on an idle host** (Boost.Asio at 2 threads: median of 5, the very first run of
the session – 186k RPS, p99 55 ms, no `wrk` errors – was discarded as a cold-start artefact); run-to-run spread was
within
±3 % for every cell and no run had a single `wrk` timeout. Treat differences under ~3 % as noise.

**What to take from this:**

- **1–2 threads: uvent + io_uring is the fastest server here** (150k / 307k RPS) – 20–38 % over Boost.Asio,
  32–33 % over libuv and 33–39 % over its own `epoll` build. The io_uring backend batches submissions and completions
  and skips the speculative `recv()`/`epoll_wait` round trips the readiness-based loops pay per request.
- **1–2 threads, `epoll` build:** level with libuv on one thread (113k vs 112k) and 10 % behind Asio; on two threads
  it is level with Asio (221k vs 223k) and 5 % behind libuv. The kernel path is identical (one `recv`, one `send` per
  request); the difference is a few hundred nanoseconds of user-space work per request – coroutine frames and the
  scheduler round trip, the price of `co_await`, not of the loop.
- **4 threads:** all four are within 5 % (356–374k); the loop stops mattering once four cores are busy.
- **8 threads** (where `wrk` competes for the same 10 cores): uvent io_uring, libuv and uvent epoll sit at 549–562k;
  Asio's single shared `io_context` falls behind (486k) with a visibly worse p99.

Sources, scripts and raw `wrk`
output: [Usub-Foundation/io_perfomance](https://github.com/Usub-Foundation/io_perfomance).
Numbers depend heavily on the host; run `scripts/run_all.sh` there to get yours.

## Ecosystem

uvent is the runtime under a family of C++23 libraries – same coroutine model, same thread pool, no glue code:

| Library                                                 | What it is                                                                             |
|---------------------------------------------------------|----------------------------------------------------------------------------------------|
| [unet](https://github.com/Usub-Foundation/unet)         | HTTP/1.1 server & client, TLS/ALPN, WebSocket, radix/regex routers, coroutine handlers |
| [upq](https://github.com/Usub-Foundation/upq)           | PostgreSQL client on libpq with pooling, `LISTEN/NOTIFY`, reflection-based row mapping |
| [uredis](https://github.com/Usub-Foundation/uredis)     | Redis client (RESP3, cluster, sentinel, connection pool)                               |
| [urpc](https://github.com/Usub-Foundation/urpc)         | Binary RPC framework with mTLS                                                         |
| [ulog](https://github.com/Usub-Foundation/ulog)         | Zero-allocation async logger flushed by a coroutine, not a thread                      |
| [ureflect](https://github.com/Usub-Foundation/ureflect) | Header-only compile-time reflection used by upq/ujson                                  |

## Requirements

- C++23 compiler: GCC 13+, Clang 17+ (Apple: `brew install llvm`), MSVC 2022
- CMake 3.22+
- Linux `io_uring` backend (optional): kernel 5.1+, liburing

Build options: `UVENT_ENABLE_IO_URING` (Linux, default OFF), `UVENT_TASK_INTROSPECTION` (default OFF),
`UVENT_ENABLE_REUSEADDR` (default ON), `UVENT_PIN_THREADS` (default ON), `UVENT_BUILD_EXAMPLES`.

## Documentation

- [Getting started (installation)](https://usub-foundation.github.io/uvent/getting-started/)
- [Quick start](https://usub-foundation.github.io/uvent/quick-start/) ·
  [Tutorial (step-by-step tour of everything)](https://usub-foundation.github.io/uvent/tutorial/)
- [System primitives](https://usub-foundation.github.io/uvent/system_primitives/) ·
  [Settings](https://usub-foundation.github.io/uvent/settings/) ·
  [Build flags](https://usub-foundation.github.io/uvent/build-flags/)
- [Awaitable](https://usub-foundation.github.io/uvent/awaitable/) ·
  [Awaitable frame](https://usub-foundation.github.io/uvent/awaitable_frame/)
- [Tasks & structured concurrency](https://usub-foundation.github.io/uvent/tasks/) ·
  [Cancellation](https://usub-foundation.github.io/uvent/cancellation/) ·
  [Introspection](https://usub-foundation.github.io/uvent/introspection/)
- [Fibers (stackful)](https://usub-foundation.github.io/uvent/fibers/) ·
  [Runtime drain (stop)](https://usub-foundation.github.io/uvent/drain/)
- [Socket](https://usub-foundation.github.io/uvent/socket/) ·
  [Name resolution & Happy Eyeballs](https://usub-foundation.github.io/uvent/resolver/)
- [Synchronization primitives](https://usub-foundation.github.io/uvent/synchronization/) ·
  [Channels](https://usub-foundation.github.io/uvent/channels/channels/) ·
  [Select](https://usub-foundation.github.io/uvent/channels/select/)
- [Timers](https://usub-foundation.github.io/uvent/timers/)

## Contributing

Issues and pull requests are welcome – see [contributing](https://usub-foundation.github.io/uvent/contributing/).
If uvent saves you a dependency, a ⭐ helps others find it.

## License

uvent is distributed under the [MIT license](LICENSE).
