# Uvent

High-performance asynchronous I/O library for C++23.

[![GitHub](https://img.shields.io/badge/GitHub-Usub--Foundation%2Fuvent-blue?logo=github)](https://github.com/Usub-Foundation/uvent)

---

## Features

- **C++23**: modern standard with coroutines and atomic primitives.
- **Event loop**: non-blocking, scalable, based on `epoll/io_uring/kqueue/iocp`.
- **Linux io_uring support** (optional): full async accept/read/write backend.
- **Windows IOCP support**: automatically selected when building on Windows.
- **Sockets**: TCP/UDP (client and server).
- **Timers**: custom `TimerWheel` with minimal overhead.
- **Queues & synchronization**: lock-free data structures, QSBR, and reference counting.
- **Custom frames**: ability to override coroutine promise frames (`AwaitableFrameBase`), enabling custom scheduling and
  integration.
- **SO_REUSEPORT support**: enables truly parallel TCP servers by binding separate listeners in each worker thread.
- **Structured concurrency**: `task::spawn` → `JoinHandle`, `TaskScope` nurseries, hierarchical cancellation delivered
  into sleeps, socket I/O, channels and locks.
- **`select`**: wait on channels, timers, tokens, events and semaphores at once, Go-style.
- **Cooperative budget**: hot coroutines are forced through the scheduler after a configurable number of fast-path
  completions, so one connection can't starve a worker.
- **Introspection** (opt-in): dump every live coroutine with its wait reason, wait time and trace id.
- **Stackful fibers**: `fiber::run` / `fiber::await` put blocking-style code (deep recursion, legacy callbacks) on the
  same scheduler, with the same cancellation and scopes as coroutines.
- **Runtime drain** (opt-in): `Uvent::stop()` cancels and unwinds every live task before the workers exit.

---

## Build & Installation

```bash
git clone https://github.com/Usub-Foundation/uvent.git
cd uvent
mkdir build && cd build
cmake ..
make -j
```

---

## Quick example

```cpp
#include "uvent/Uvent.h"

using namespace usub::uvent;

task::Awaitable<void> clientCoro(net::TCPClientSocket socket)
{
    static constexpr size_t max_read_size = 64 * 1024;
    utils::DynamicBuffer buffer;
    buffer.reserve(max_read_size);

    static const std::string_view httpResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "{\"status\":\"success\"}";

    socket.set_timeout_ms(5000);
    while (true)
    {
        buffer.clear();
        ssize_t rdsz = co_await socket.async_read(buffer, max_read_size);
        socket.update_timeout(5000);
        if (rdsz <= 0)
        {
            socket.shutdown();
            break;
        }
        auto buf = std::make_unique<uint8_t[]>(1024);
        size_t wrsz = co_await socket.async_write(
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(httpResponse.data())),
            httpResponse.size()
        );
        if (wrsz <= 0)
        {
            break;
        }
        socket.update_timeout(5000);
    }
    co_return;
}

task::Awaitable<void> test_coro()
{
    using namespace std::chrono_literals;
    std::cout << "test_coro()" << std::endl;
    co_await system::this_coroutine::sleep_for(2000ms);
    std::cout << "test_coro() 2" << std::endl;
    co_return;
}

task::Awaitable<void> listeningCoro()
{
    auto acceptor = new net::TCPServerSocket{"0.0.0.0", 45900};
    for (;;)
    {
        auto soc = co_await acceptor->async_accept();
        co_await test_coro();
        if (soc) system::co_spawn(clientCoro(std::move(soc.value())));
    }
}

task::Awaitable<void> sendingCoro()
{
    auto socket = net::TCPClientSocket{};
    auto res = co_await socket.async_connect("example.com", "80");
    if (res.has_value()) co_return;
    uint8_t buffer[] =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test-client\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n";
    size_t size = sizeof(buffer) - 1;

    for (int i = 0; i < 2; i++)
    {
        auto result = co_await socket.async_send(buffer, size);
        if (result.has_value())
        {
            std::cout << result.value() << std::endl;
        }
        else
        {
            std::cout << toString(result.error()) << std::endl;
        }
    }

    co_return;
}

int main()
{
    settings::timeout_duration_ms = 5000;
    if constexpr (!system::is_reuseaddr_enabled)
    {
        system::co_spawn(std::move(listeningCoro()));
    }
    usub::Uvent uvent(4);
    if constexpr (system::is_reuseaddr_enabled)
    {
        uvent.for_each_thread([&](int threadIndex, thread::ThreadLocalStorage* tls)
        {
            system::co_spawn_static(listeningCoro(), threadIndex);
        });
    }
    uvent.run();
    return 0;
}
```

---

## Notes

* When compiled **with `SO_REUSEPORT` support** (`UVENT_ENABLE_REUSEADDR` defined),
  each thread can bind its own listener socket via `for_each_thread`, providing optimal load balancing across cores.
  This pattern is preferred for high-connection-rate servers.

* Without `SO_REUSEPORT`, a single global listener is used — still asynchronous and non-blocking, but accepts
  connections sequentially.

* Both modes are fully coroutine-driven and compatible with the same I/O APIs.