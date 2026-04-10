# Uvent

Cross-platform async I/O engine with native backends for:

| OS / family                                    | Backend primitives                                                           | Implementation                                                  |
|------------------------------------------------|------------------------------------------------------------------------------|-----------------------------------------------------------------|
| Linux                                          | `epoll` (edge-triggered), non-blocking sockets / **`io_uring` (optional)**   | `SocketLinux`, `SocketLinuxIOUring`, `EPoller`, `IOUringPoller` |
| macOS, FreeBSD, OpenBSD, NetBSD, DragonFly BSD | `kqueue`, non-blocking sockets (`accept` + `fcntl`)                          | `SocketBSD`, `KQueuePoller`                                     |
| Windows 10+ / Windows Server 2016+             | **IOCP** (`WSARecv` / `WSASend` / `AcceptEx` / `ConnectEx` / `TransmitFile`) | `SocketWindows`, `IocpPoller`                                   |

A single high-level API (`TCPServerSocket`, `TCPClientSocket`, `UDPSocket`) is used across all platforms.

### Requests per second (RPS)

| Threads | uvent   | Boost.Asio | libuv |
|---------|---------|------------|-------|
| 1       | 108,875 | 97,219     | 116   |
| 2       | 208,346 | 185,813    | 828   |
| 4       | 378,450 | 330,374    | 830   |
| 8       | 610,102 | 423,409    | 827   |

⚡ **Conclusion:** `uvent` delivers performance nearly on par with Boost.Asio and significantly outperforms libuv, while
keeping low latency (p99 around 2–3 ms).

👉 For more detailed and up-to-date benchmark results, see the dedicated
repository: [usub-foundation/io_perfomance](https://github.com/usub-foundation/io_perfomance)

# Quick start

Minimal TCP echo server:

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
        size_t wrsz = co_await socket.async_write(
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(httpResponse.data())),
            httpResponse.size());
        if (wrsz <= 0)
            break;
        socket.update_timeout(5000);
    }
    co_return;
}

task::Awaitable<void> listeningCoro()
{
    auto acceptor = new net::TCPServerSocket{"0.0.0.0", 45900};
    co_await acceptor->async_accept(clientCoro);
}

int main()
{
    settings::timeout_duration_ms = 5000;

    usub::Uvent uvent(4);
    uvent.for_each_thread([&](int threadIndex, thread::ThreadLocalStorage* tls) {
        system::co_spawn_static(listeningCoro(), threadIndex);
    });

    uvent.run();
    return 0;
}
```

`async_accept` accepts a coroutine function directly — no manual loop or `co_spawn` needed.
Each accepted connection is automatically spawned as a separate coroutine.

### Backend selection

Uvent automatically selects the best backend for your OS:

- **Linux** → `epoll` by default, or **io_uring** when explicitly enabled
- **Windows** → **IOCP** (always enabled, no flags required)
- **BSD / macOS** → `kqueue`

#### io_uring

To enable `io_uring` on Linux during build:

```bash
cmake -DUVENT_ENABLE_IO_URING=ON ..
make -j
```

or via CMake FetchContent:

```cmake
set(UVENT_ENABLE_IO_URING ON)
```

Requires Linux kernel **5.1+** and [liburing](https://github.com/axboe/liburing).

# Documentation

- [Getting started (installation)](https://usub-foundation.github.io/uvent/getting-started/)
- [Quick start](https://usub-foundation.github.io/uvent/quick-start/)
- [System primitives](https://usub-foundation.github.io/uvent/system_primitives/)
- [Settings](https://usub-foundation.github.io/uvent/settings/)
- [Awaitable](https://usub-foundation.github.io/uvent/awaitable/)
- [Awaitable frame](https://usub-foundation.github.io/uvent/awaitable_frame/)
- [Socket](https://usub-foundation.github.io/uvent/socket/)
- [Synchronization primitives & Channels](https://usub-foundation.github.io/uvent/synchronization/)

---

# Licence

Uvent is distributed under the [MIT license](LICENSE)