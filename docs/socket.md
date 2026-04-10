# Socket API

Templated socket wrapper with coroutine-friendly I/O and explicit role/proto typing.

```cpp
template <Proto P, Role R>
class Socket;
using TCPServerSocket = Socket<Proto::TCP, Role::PASSIVE>;
using TCPClientSocket = Socket<Proto::TCP, Role::ACTIVE>;
using UDPBoundSocket  = Socket<Proto::UDP, Role::ACTIVE>;
using UDPSocket       = Socket<Proto::UDP, Role::PASSIVE>;
```

## Header layout (internal)

`SocketHeader` stores FD, timer id, state flags, and two coroutine slots (for reading and for writing operations).
State flags (bitmask): `CLOSED_MASK`, `BUSY_MASK`, `READING_MASK`, `WRITING_MASK`.

---

## Constructors & Assignment

```cpp
Socket() noexcept;
explicit Socket(SocketHeader* header) noexcept;          // wrap existing header (no FD dup)
static Socket from_existing(SocketHeader* header);       // initializes refcount state

// TCP PASSIVE only (bind+listen)
explicit Socket(std::string&  ip, int port=8080, int backlog=50,
                utils::net::IPV ipv=utils::net::IPV4,
                utils::net::SocketAddressType sat=utils::net::TCP) noexcept requires(P==Proto::TCP && R==Role::PASSIVE);

explicit Socket(std::string&& ip, int port=8080, int backlog=50,
                utils::net::IPV ipv=utils::net::IPV4,
                utils::net::SocketAddressType sat=utils::net::TCP) noexcept requires(P==Proto::TCP && R==Role::PASSIVE);

// copy/move
Socket(const Socket&) noexcept;
Socket(Socket&&) noexcept;
Socket& operator=(const Socket&) noexcept;
Socket& operator=(Socket&&) noexcept;

~Socket();
```

**Notes**

* TCP passive ctor: creates non-blocking FD, registers READ with poller.
* Copy increases internal refcount; move steals header.

---

## Introspection

```cpp
SocketHeader* get_raw_header();
```

---

## Accept (TCP server)

### Legacy overload (deprecated)

```cpp
[[deprecated]]
task::Awaitable<std::optional<TCPClientSocket>,
  uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
async_accept() requires(P==Proto::TCP && R==Role::PASSIVE);
```

Returns a ready-to-use `TCPClientSocket` (non-blocking; READ registered) or `std::nullopt` on failure.

> **Deprecated.** Use `async_accept(F, Args...)` instead. In edge-triggered mode (epoll ET / kqueue EV_CLEAR)
> the kernel notifies once per batch of incoming connections — accepting a single socket per wakeup silently
> loses all others buffered since the last edge. The new overload drains the queue completely before suspending.

### Preferred overload

```cpp
template <typename F, typename... Args>
    requires std::invocable<F, TCPClientSocket, Args...>
[[nodiscard]] task::Awaitable<void>
async_accept(F on_accept, Args&&... args)
    requires(P==Proto::TCP && R==Role::PASSIVE);
```

Loops forever, accepting all pending connections before each suspend point, and invoking `on_accept` for every
accepted socket. Additional arguments `args` are forwarded to `on_accept` on each call.

`F` may be:

* A **plain callable** (lambda, function pointer) returning `void` — called directly.
* A **coroutine function** returning any `Awaitable` — automatically spawned via `system::co_spawn`.

**Platform behaviour**

| Backend            | Strategy                                                                                                  |
|--------------------|-----------------------------------------------------------------------------------------------------------|
| Linux epoll (ET)   | Drains `accept4` in a tight loop until `EAGAIN`, then suspends on `AwaiterAccept`. One suspend per batch. |
| Linux io_uring     | Posts `AcceptAwaiter` SQE when queue is empty; kernel delivers fd via CQE. One suspend per connection.    |
| BSD / macOS kqueue | Same drain strategy as epoll using `accept` + manual `fcntl(O_NONBLOCK\|FD_CLOEXEC)`.                     |
| Windows IOCP       | Posts `AcceptEx` overlapped operation; suspends on `AwaiterRead` until completion packet arrives.         |

**Usage**

```cpp
// Coroutine directly — co_spawn called automatically
task::Awaitable<void> listeningCoro()
{
    auto acceptor = new net::TCPServerSocket{"0.0.0.0", 45900};
    co_await acceptor->async_accept(clientCoro);
}

// Lambda — useful when extra setup is needed before spawn
task::Awaitable<void> listeningCoro()
{
    auto acceptor = new net::TCPServerSocket{"0.0.0.0", 45900};
    co_await acceptor->async_accept([](net::TCPClientSocket socket) {
        socket.set_timeout_ms(5000);
        system::co_spawn(clientCoro(std::move(socket)));
    });
}

// With extra arguments forwarded to on_accept
co_await acceptor->async_accept(myHandler, config, logger);
```

**Notes**

* The call never returns under normal operation — it runs for the lifetime of the acceptor.
* `co_return` (exit) only on unrecoverable errors (bad FD, AcceptEx failure, etc.).
* Transient errors (`EINTR`, `ECONNABORTED`, `EPROTO`) are retried immediately without suspending.
* Resource pressure errors (`ENFILE`, `EMFILE`, `ENOBUFS`) yield to the scheduler and retry on the next wakeup.

---

## Async I/O

```cpp
// READ: TCP ACTIVE or any UDP
task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
async_read(utils::DynamicBuffer& buf, size_t max_read_size)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));

// WRITE: TCP ACTIVE or any UDP
task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
async_write(uint8_t* data, size_t size)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));

// sendfile: TCP ACTIVE or any UDP
task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
async_sendfile(int in_fd, off_t* offset, size_t count)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));
```

**Behavior**

* `async_read` waits for EPOLLIN and pulls chunks into `DynamicBuffer` until `max_read_size` or would-block.
    * Returns `>0` bytes read, `0` on EOF, `-1` on error, `-2` if `max_read_size` hit.
* `async_write` waits for EPOLLOUT and sends until would-block or done. Returns bytes written or `-1` on error.
* `async_sendfile` waits for EPOLLOUT, then calls `sendfile`. Returns bytes sent or `-1` on error.

---

## Sync I/O

```cpp
ssize_t read(utils::DynamicBuffer& buf, size_t max_read_size)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));

ssize_t write(uint8_t* data, size_t size)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));

ssize_t sendfile(int in_fd, off_t* offset, size_t count)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));
```

Return conventions mirror the async versions (`>0`, `0`, `-1`, `-2` for `read`).

---

## Connect (TCP client)

```cpp
task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
  uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
async_connect(std::string&  host, std::string&  port)
requires(P==Proto::TCP && R==Role::ACTIVE);

task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
  uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
async_connect(std::string&& host, std::string&& port)
requires(P==Proto::TCP && R==Role::ACTIVE);
```

Resolves address, creates non-blocking socket, initiates `connect`, waits for EPOLLOUT, checks error.

* Returns `std::nullopt` on success, or a specific `ConnectError` (`GetAddrInfoFailed`, `SocketCreationFailed`,
  `ConnectFailed` or `Unknown`).

### Connect timeout support

`async_connect()` supports an optional timeout parameter:

```cpp
async_connect(host, port, std::chrono::milliseconds timeout);
```

* If the timeout expires before the socket becomes writable, the coroutine completes with `ConnectError::Timeout`.
* Works uniformly on all platforms (`epoll`, `kqueue`, `IOCP`, `io_uring`).
* Timeout is implemented via the socket timer system (`set_timeout_ms` + timer wheel).

```cpp
auto ec = co_await sock.async_connect("1.2.3.4", "443", std::chrono::seconds{3});
if (ec && *ec == ConnectError::Timeout) { /* handle timeout */ }
```

---

## High-level send/receive helpers

```cpp
task::Awaitable<std::expected<std::string, usub::utils::errors::SendError>,
  uvent::detail::AwaitableIOFrame<std::expected<std::string, usub::utils::errors::SendError>>>
async_send(uint8_t* data, size_t size, size_t chunkSize=16384, size_t maxSize=65536)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));

std::expected<std::string, usub::utils::errors::SendError>
send(uint8_t* data, size_t size, size_t chunkSize=16384, size_t maxSize=65536)
requires((P==Proto::TCP && R==Role::ACTIVE) || (P==Proto::UDP));
```

* On success returns response payload (up to `maxSize`).
* On failure returns `SendError` (`InvalidSocketFd`).

---

## Timers & Lifecycle

```cpp
void update_timeout(timer_duration_t new_duration) const;
void shutdown();
void set_timeout_ms(timeout_t timeout = settings::timeout_duration_ms) const
    requires(P == Proto::TCP && R == Role::ACTIVE);
```

* `update_timeout` — refreshes the timer wheel entry with a new duration.
* `shutdown` — closes both directions with `SHUT_RDWR`.
* `set_timeout_ms` — overrides the timeout for this TCP client socket.
    * Default is `settings::timeout_duration_ms` (20 000 ms).
    * Must be called after socket initialization.

Destruction path (internal):

* `destroy()` → mark `CLOSED_MASK`, unregister from poller, retire header via QSBR.
* `remove()` → unregister and mark closed (without QSBR retire).

---

## Thread-safety & State

* The header's `state` uses atomic bitmasks:
    * `try_mark_busy() / clear_busy()`
    * `try_mark_reading() / clear_reading()`
    * `try_mark_writing() / clear_writing()`
    * `close_for_new_refs()` prevents new references before retirement.
* Designed for a multi-threaded event loop; avoid concurrent conflicting ops on the same socket unless you use the
  provided state guards.

---

## Client addr

```cpp
client_addr_t get_client_addr() const;
client_addr_t get_client_addr();
```

Both return `typedef std::variant<sockaddr_in, sockaddr_in6> client_addr_t`.

```cpp
[[nodiscard]] utils::net::IPV get_client_ipv() const
    requires(P == Proto::TCP && R == Role::ACTIVE);
```

Returns:

```cpp
enum IPV { IPV4 = 0x0, IPV6 = 0x1 };
```

---

## Return/Error Summary

| Type                           | Meaning                            |
|--------------------------------|------------------------------------|
| `ssize_t > 0`                  | Bytes transferred                  |
| `ssize_t == 0`                 | EOF (read only)                    |
| `ssize_t == -1`                | I/O error                          |
| `ssize_t == -2`                | Read cap (`max_read_size`) reached |
| `std::optional<T>` — `nullopt` | Success / value absent             |
| `std::optional<T>` — set       | Error / value present              |
| `std::expected<T,E>` — value   | Success                            |
| `std::expected<T,E>` — error   | Failure enum                       |