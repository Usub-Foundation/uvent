#ifndef SOCKETLINUX_IOURING_H
#define SOCKETLINUX_IOURING_H

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <expected>
#include <memory>
#include <variant>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include "AwaiterOperations.h"
#include "SocketMetadata.h"
#include "uvent/base/Predefines.h"
#include "uvent/poll/IOUringPoller.h"
#include "uvent/system/Defines.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"
#include "uvent/utils/buffer/DynamicBuffer.h"
#include "uvent/utils/errors/IOErrors.h"
#include "uvent/utils/net/net.h"
#include "uvent/utils/net/socket.h"

namespace usub::uvent::net
{
    namespace detail
    {
        extern void processSocketTimeout(std::any arg);

        using core::IOUringPoller;
        using core::detail::AcceptOp;
        using core::detail::ConnectOp;
        using core::detail::IoOpKind;
        using core::detail::RecvOp;
        using core::detail::SendFileOp;
        using core::detail::SendOp;

        struct RecvAwaiter
        {
            RecvOp op{};
            SocketHeader* header{nullptr};
            uint8_t* buf{nullptr};
            size_t len{0};

            bool await_ready() const noexcept { return false; }

            template <class Promise>
            void await_suspend(std::coroutine_handle<Promise> h)
            {
                op.kind = IoOpKind::Recv;
                op.header = header;
                op.coro = h;
                op.buf = buf;
                op.len = len;

                auto& pl = static_cast<IOUringPoller&>(system::this_thread::detail::pl);
                pl.submit_recv(&op, header->fd);
            }

            ssize_t await_resume() noexcept
            {
                if (op.res < 0)
                    return -op.err;
                return op.res;
            }
        };

        struct SendAwaiter
        {
            SendOp op{};
            SocketHeader* header{nullptr};
            const uint8_t* buf{nullptr};
            size_t len{0};

            bool await_ready() const noexcept { return false; }

            template <class Promise>
            void await_suspend(std::coroutine_handle<Promise> h)
            {
                op.kind = IoOpKind::Send;
                op.header = header;
                op.coro = h;
                op.buf = buf;
                op.len = len;

                auto& pl = static_cast<IOUringPoller&>(system::this_thread::detail::pl);
                pl.submit_send(&op, header->fd);
            }

            ssize_t await_resume() noexcept
            {
                if (op.res < 0)
                    return -op.err;
                return op.res;
            }
        };

        struct AcceptAwaiter
        {
            AcceptOp op{};
            SocketHeader* header{nullptr};

            bool await_ready() const noexcept { return false; }

            template <class Promise>
            void await_suspend(std::coroutine_handle<Promise> h)
            {
                op.kind = IoOpKind::Accept;
                op.header = header;
                op.coro = h;
                op.addrlen = sizeof(op.addr);

                auto& pl = static_cast<IOUringPoller&>(system::this_thread::detail::pl);
                pl.submit_accept(&op, header->fd);
            }

            int await_resume() noexcept
            {
                if (op.res < 0)
                    return -op.err;
                return static_cast<int>(op.res);
            }
        };

        struct SendFileAwaiter
        {
            SendFileOp op{};
            SocketHeader* header{nullptr};

            bool await_ready() const noexcept { return false; }

            template <class Promise>
            void await_suspend(std::coroutine_handle<Promise> h)
            {
                op.kind = IoOpKind::SendFile;
                op.header = header;
                op.coro = h;

                system::this_thread::detail::q->enqueue(h);
            }

            void await_resume() noexcept {}
        };

        struct ConnectAwaiter
        {
            ConnectOp op{};
            SocketHeader* header{nullptr};

            bool await_ready() const noexcept { return false; }

            template <class Promise>
            void await_suspend(std::coroutine_handle<Promise> h)
            {
                op.kind = IoOpKind::Connect;
                op.header = header;
                op.coro = h;

                auto& pl = static_cast<IOUringPoller&>(system::this_thread::detail::pl);
                pl.submit_connect(&op, header->fd);
            }

            int await_resume() noexcept
            {
                if (op.res < 0)
                    return -op.err;
                return 0;
            }
        };
    } // namespace detail

    template <Proto p, Role r>
    class Socket : usub::utils::sync::refc::RefCounted<Socket<p, r>>
    {
    public:
        friend class usub::utils::sync::refc::RefCounted<Socket<p, r>>;
        friend class core::IOUringPoller;
        friend void detail::processSocketTimeout(std::any arg);

        Socket() noexcept;
        explicit Socket(int fd) noexcept;

        explicit Socket(std::string& ip_addr, int port = 8080, int backlog = 50, utils::net::IPV ipv = utils::net::IPV4,
                        utils::net::SocketAddressType socketAddressType = utils::net::TCP) noexcept
            requires(p == Proto::TCP && r == Role::PASSIVE);

        explicit Socket(std::string&& ip_addr, int port = 8080, int backlog = 50,
                        utils::net::IPV ipv = utils::net::IPV4,
                        utils::net::SocketAddressType socketAddressType = utils::net::TCP) noexcept
            requires(p == Proto::TCP && r == Role::PASSIVE);

        explicit Socket(SocketHeader* header) noexcept;

        Socket(const Socket& o) noexcept;
        Socket(Socket&& o) noexcept;

        Socket& operator=(const Socket& o) noexcept;
        Socket& operator=(Socket&& o) noexcept;

        ~Socket();

        static Socket from_existing(SocketHeader* header);
        SocketHeader* get_raw_header();

        [[nodiscard]] task::Awaitable<std::optional<TCPClientSocket>,
                                      uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
        async_accept()
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Asynchronously accepts one connection and invokes \p on_accept.
         *
         * \tparam F    Callable with signature \c void(TCPClientSocket, Args...).
         * \tparam Args Types of additional arguments forwarded to \p on_accept.
         *
         * \param on_accept Invoked with the accepted socket after completion.
         * \param args      Zero or more additional arguments forwarded to \p on_accept.
         */
        template <typename F, typename... Args>
            requires std::invocable<F, TCPClientSocket, Args...>
        [[nodiscard]] task::Awaitable<void> async_accept(F on_accept, Args&&... args)
            requires(p == Proto::TCP && r == Role::PASSIVE);

        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
        async_read(utils::DynamicBuffer& buffer, size_t max_read_size)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
        async_read(uint8_t* buffer, size_t max_read_size)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> async_write(uint8_t* buf,
                                                                                                     size_t sz)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] ssize_t read(utils::DynamicBuffer& buffer, size_t max_read_size)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] ssize_t write(uint8_t* buf, size_t sz)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Asynchronously connects to the specified host and port (lvalue refs).
         * Waits for the socket to become writable and checks for connection success.
         */
        [[nodiscard]] task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                                      uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
        async_connect(std::string& host, std::string& port,
                      std::chrono::milliseconds connect_timeout = std::chrono::milliseconds{0})
            requires(p == Proto::TCP && r == Role::ACTIVE);

        /**
         * \brief Asynchronously connects to the specified host and port (lvalue refs).
         * Waits for the socket to become writable and checks for connection success. Move strings.
         */
        [[nodiscard]] task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                                      uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
        async_connect(std::string&& host, std::string&& port,
                      std::chrono::milliseconds connect_timeout = std::chrono::milliseconds{0})
            requires(p == Proto::TCP && r == Role::ACTIVE);

        task::Awaitable<std::expected<size_t, usub::utils::errors::SendError>,
                        uvent::detail::AwaitableIOFrame<std::expected<size_t, usub::utils::errors::SendError>>>
        async_send(uint8_t* buf, size_t sz)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] std::expected<std::string, usub::utils::errors::SendError>
        send(uint8_t* buf, size_t sz, size_t chunkSize = 16384, size_t maxSize = 65536)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
        async_sendfile(int in_fd, off_t* offset, size_t count)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        [[nodiscard]] ssize_t sendfile(int in_fd, off_t* offset, size_t count)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        void update_timeout(timer_duration_t new_duration) const;
        void shutdown();

        void set_timeout_ms(timeout_t timeout = settings::timeout_duration_ms) const
            requires(p == Proto::TCP && r == Role::ACTIVE);

        std::expected<std::string, usub::utils::errors::SendError> receive(size_t chunk_size, size_t maxSize);

        [[nodiscard]] client_addr_t get_client_addr() const
            requires(p == Proto::TCP && r == Role::ACTIVE);

        [[nodiscard]] client_addr_t get_client_addr()
            requires(p == Proto::TCP && r == Role::ACTIVE);

        [[nodiscard]] utils::net::IPV get_client_ipv() const
            requires(p == Proto::TCP && r == Role::ACTIVE);

        void remove();

    protected:
        void destroy() noexcept override;

    private:
        size_t send_aux(uint8_t* buf, size_t size);

    public:
        client_addr_t address;
        utils::net::IPV ipv{utils::net::IPV4};

    private:
        SocketHeader* header_{nullptr};
    };

    template <Proto p, Role r>
    Socket<p, r>::Socket() noexcept
    {
        this->header_ = new SocketHeader{
            .socket_info = (static_cast<uint8_t>(Proto::TCP) | static_cast<uint8_t>(Role::ACTIVE) |
                            static_cast<uint8_t>(AdditionalState::CONNECTION_PENDING)),
            .state = (1 & usub::utils::sync::refc::COUNT_MASK) | (false ? usub::utils::sync::refc::CLOSED_MASK : 0)};
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(int fd) noexcept
    {
        this->header_ = new SocketHeader{
            .fd = fd,
            .socket_info = (static_cast<uint8_t>(Proto::TCP) | static_cast<uint8_t>(Role::ACTIVE) |
                            static_cast<uint8_t>(AdditionalState::CONNECTION_PENDING)),
            .state = (1 & usub::utils::sync::refc::COUNT_MASK) | (false ? usub::utils::sync::refc::CLOSED_MASK : 0)};
        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(std::string& ip_addr, int port, int backlog, utils::net::IPV ipv_,
                         utils::net::SocketAddressType socketAddressType) noexcept
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        this->header_ =
            new SocketHeader{.fd = utils::socket::createSocket(port, ip_addr, backlog, ipv_, socketAddressType),
                             .socket_info = (uint8_t(p) | uint8_t(r)),
#ifndef UVENT_ENABLE_REUSEADDR
                             .state = std::atomic<uint64_t>((1ull & usub::utils::sync::refc::COUNT_MASK))
#else
                             .state = (1ull & usub::utils::sync::refc::COUNT_MASK)
#endif
            };
        utils::socket::makeSocketNonBlocking(this->header_->fd);
        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::READ);
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(std::string&& ip_addr, int port, int backlog, utils::net::IPV ipv_,
                         utils::net::SocketAddressType socketAddressType) noexcept
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        this->header_ =
            new SocketHeader{.fd = utils::socket::createSocket(port, ip_addr, backlog, ipv_, socketAddressType),
                             .socket_info = (static_cast<uint8_t>(p) | static_cast<uint8_t>(r)),
#ifndef UVENT_ENABLE_REUSEADDR
                             .state = std::atomic<uint64_t>((1ull & usub::utils::sync::refc::COUNT_MASK))
#else
                             .state = (1ull & usub::utils::sync::refc::COUNT_MASK)
#endif
            };
        utils::socket::makeSocketNonBlocking(this->header_->fd);
        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::READ);
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(SocketHeader* header) noexcept : header_(header)
    {
    }

    template <Proto p, Role r>
    Socket<p, r>::~Socket()
    {
        if (this->header_)
        {
#if UVENT_DEBUG
            const auto cnt = (this->header_->state & usub::utils::sync::refc::COUNT_MASK);
            const auto fd = this->header_->fd;
#endif

            this->release();

#if UVENT_DEBUG
            spdlog::warn("Socket dtor: counter={}, fd={}", cnt, fd);
#endif
        }
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(const Socket& o) noexcept : header_(o.header_)
    {
        if (this->header_)
            this->add_ref();
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(Socket&& o) noexcept : header_(o.header_)
    {
        o.header_ = nullptr;
    }

    template <Proto p, Role r>
    Socket<p, r>& Socket<p, r>::operator=(const Socket& o) noexcept
    {
        if (this == &o)
            return *this;
        Socket tmp(o);
        std::swap(this->header_, tmp.header_);
        return *this;
    }

    template <Proto p, Role r>
    Socket<p, r>& Socket<p, r>::operator=(Socket&& o) noexcept
    {
        if (this == &o)
            return *this;
        Socket tmp(std::move(o));
        std::swap(this->header_, tmp.header_);
        return *this;
    }

    template <Proto p, Role r>
    Socket<p, r> Socket<p, r>::from_existing(SocketHeader* header)
    {
        return Socket(header);
    }

    template <Proto p, Role r>
    SocketHeader* Socket<p, r>::get_raw_header()
    {
        return this->header_;
    }

    template <Proto p, Role r>
    task::Awaitable<std::optional<TCPClientSocket>, uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
    Socket<p, r>::async_accept()
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        for (;;)
        {
            sockaddr_storage ss{};
            socklen_t sl = sizeof(ss);

            int cfd = ::accept4(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (cfd >= 0)
            {
                auto* h = new SocketHeader{.fd = cfd,
                                           .socket_info = uint8_t(Proto::TCP) | uint8_t(Role::ACTIVE),
                                           .state = (1ull & usub::utils::sync::refc::COUNT_MASK)};

                TCPClientSocket sc(h);
                if (ss.ss_family == AF_INET)
                    sc.address = *reinterpret_cast<sockaddr_in*>(&ss);
                else if (ss.ss_family == AF_INET6)
                {
                    sc.address = *reinterpret_cast<sockaddr_in6*>(&ss);
                    sc.ipv = utils::net::IPV6;
                }
                co_return sc;
            }

            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                switch (errno)
                {
                case EINTR:
                case ECONNABORTED:
#if defined(EPROTO) && (EPROTO != ECONNABORTED)
                case EPROTO:
#endif
                    continue;
                default:
                    co_return std::nullopt;
                }
            }

            detail::AcceptAwaiter aw{
                .op = {},
                .header = this->header_,
            };

            int new_fd = co_await aw;
            if (new_fd < 0)
                co_return std::nullopt;

            auto* h = new SocketHeader{.fd = new_fd,
                                       .socket_info = uint8_t(Proto::TCP) | uint8_t(Role::ACTIVE),
                                       .state = (1ull & usub::utils::sync::refc::COUNT_MASK)};

            TCPClientSocket sc(h);
            if (aw.op.addr.ss_family == AF_INET)
                sc.address = *reinterpret_cast<sockaddr_in*>(&aw.op.addr);
            else if (aw.op.addr.ss_family == AF_INET6)
            {
                sc.address = *reinterpret_cast<sockaddr_in6*>(&aw.op.addr);
                sc.ipv = utils::net::IPV6;
            }

            co_return sc;
        }
    }

    template <Proto p, Role r>
    template <typename F, typename... Args>
        requires std::invocable<F, TCPClientSocket, Args...>
    task::Awaitable<void> Socket<p, r>::async_accept(F on_accept, Args&&... args)
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        for (;;)
        {
            sockaddr_storage ss{};
            socklen_t sl = sizeof(ss);

            int cfd = ::accept4(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd >= 0)
            {
                auto* h = new SocketHeader{.fd = cfd,
                                           .socket_info = uint8_t(Proto::TCP) | uint8_t(Role::ACTIVE),
                                           .state = (1ull & usub::utils::sync::refc::COUNT_MASK)};

                TCPClientSocket sc(h);
                if (ss.ss_family == AF_INET)
                    sc.address = *reinterpret_cast<sockaddr_in*>(&ss);
                else if (ss.ss_family == AF_INET6)
                {
                    sc.address = *reinterpret_cast<sockaddr_in6*>(&ss);
                    sc.ipv = utils::net::IPV6;
                }

                if constexpr (std::is_void_v<std::invoke_result_t<F, TCPClientSocket>>)
                    on_accept(std::move(sc, std::forward<Args>(args)...));
                else
                    system::co_spawn(on_accept(std::move(sc), std::forward<Args>(args)...));
                continue;
            }

            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                switch (errno)
                {
                case EINTR:
                case ECONNABORTED:
#if defined(EPROTO) && (EPROTO != ECONNABORTED)
                case EPROTO:
#endif
                    continue;
                default:
                    co_return;
                }
            }

            detail::AcceptAwaiter aw{.op = {}, .header = this->header_};
            int new_fd = co_await aw;
            if (new_fd < 0)
                co_return;

            auto* h = new SocketHeader{.fd = new_fd,
                                       .socket_info = uint8_t(Proto::TCP) | uint8_t(Role::ACTIVE),
                                       .state = (1ull & usub::utils::sync::refc::COUNT_MASK)};

            TCPClientSocket sc(h);
            if (aw.op.addr.ss_family == AF_INET)
                sc.address = *reinterpret_cast<sockaddr_in*>(&aw.op.addr);
            else if (aw.op.addr.ss_family == AF_INET6)
            {
                sc.address = *reinterpret_cast<sockaddr_in6*>(&aw.op.addr);
                sc.ipv = utils::net::IPV6;
            }

            if constexpr (std::is_void_v<std::invoke_result_t<F, TCPClientSocket>>)
                on_accept(std::move(sc));
            else
                system::co_spawn(on_accept(std::move(sc)));
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_read(uint8_t* dst,
                                                                                                size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_read(raw, io_uring): fd={}", this->header_->fd);
#endif
        if (!dst || max_read_size == 0)
            co_return 0;

        detail::RecvAwaiter aw{
            .op = {},
            .header = this->header_,
            .buf = dst,
            .len = max_read_size,
        };

        ssize_t res = co_await aw;

        if (res == 0)
        {
#ifndef UVENT_ENABLE_REUSEADDR
            this->header_->timeout_epoch_bump();
#endif
            co_return 0;
        }

        if (res < 0)
            co_return res;

#ifndef UVENT_ENABLE_REUSEADDR
        this->header_->timeout_epoch_bump();
#endif
        co_return res;
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
    Socket<p, r>::async_read(utils::DynamicBuffer& buffer, size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_read(DynamicBuffer, io_uring): fd={}", this->header_->fd);
#endif
        if (max_read_size == 0)
            co_return 0;

        auto tmp = std::unique_ptr<uint8_t[]>(new uint8_t[max_read_size]);

        detail::RecvAwaiter aw{
            .op = {},
            .header = this->header_,
            .buf = tmp.get(),
            .len = max_read_size,
        };

        ssize_t res = co_await aw;

        if (res <= 0)
        {
#ifndef UVENT_ENABLE_REUSEADDR
            this->header_->timeout_epoch_bump();
#endif
            co_return res;
        }

        buffer.append(tmp.get(), static_cast<size_t>(res));

#ifndef UVENT_ENABLE_REUSEADDR
        this->header_->timeout_epoch_bump();
#endif
        co_return res;
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_write(uint8_t* buf,
                                                                                                 size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_write(io_uring): fd={}, sz={}", this->header_->fd, sz);
#endif
        if (!buf || sz == 0)
            co_return 0;

        auto internal = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
        std::memcpy(internal.get(), buf, sz);

        size_t total_written = 0;

        while (total_written < sz)
        {
            detail::SendAwaiter aw{
                .op = {},
                .header = this->header_,
                .buf = internal.get() + total_written,
                .len = sz - total_written,
            };

            ssize_t res = co_await aw;

            if (res <= 0)
            {
                co_return (total_written > 0) ? static_cast<ssize_t>(total_written) : res;
            }

            total_written += static_cast<size_t>(res);
        }

#ifndef UVENT_ENABLE_REUSEADDR
        if (total_written > 0)
            this->header_->timeout_epoch_bump();
#endif

        co_return static_cast<ssize_t>(total_written);
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::read(utils::DynamicBuffer& buffer, size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        ssize_t total_read = 0;
        int retries = 0;
        while (true)
        {
            uint8_t temp[16384];
            size_t to_read = std::min(sizeof(temp), max_read_size - buffer.size());

            ssize_t res = ::recv(this->header_->fd, temp, to_read, MSG_DONTWAIT);

            if (res > 0)
            {
                buffer.append(temp, res);
                total_read += res;
                retries = 0;
            }
            else if (res == 0)
            {
                return total_read > 0 ? total_read : 0;
            }
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                {
                    if (++retries >= settings::max_read_retries)
                        return -1;
                    continue;
                }
                return -1;
            }

            if (buffer.size() >= max_read_size)
                break;
        }
        return total_read;
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::write(uint8_t* buf, size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        auto buf_internal = std::unique_ptr<uint8_t[]>(new uint8_t[sz], std::default_delete<uint8_t[]>());
        std::copy_n(buf, sz, buf_internal.get());

        ssize_t total_written = 0;
        int retries = 0;

        while (total_written < static_cast<ssize_t>(sz))
        {
            ssize_t res =
                ::send(this->header_->fd, buf_internal.get() + total_written, sz - total_written, MSG_DONTWAIT);
            if (res > 0)
            {
                total_written += res;
                retries = 0;
                continue;
            }
            if (res == -1)
            {
                if (errno == EINTR)
                {
                    if (++retries >= settings::max_write_retries)
                        return -1;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return -1;
            }
        }
        return total_written;
    }

    template <Proto p, Role r>
    task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                    uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
    Socket<p, r>::async_connect(std::string& host, std::string& port, std::chrono::milliseconds connect_timeout)
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        {
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::GetAddrInfoFailed;
        }

        this->header_->fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (this->header_->fd < 0)
        {
            freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::SocketCreationFailed;
        }

        if (connect_timeout.count() > 0)
        {
            int ms = static_cast<int>(connect_timeout.count());
            ::setsockopt(this->header_->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, static_cast<socklen_t>(sizeof(ms)));

            this->set_timeout_ms(static_cast<timeout_t>(ms));
        }

        int s_flags = ::fcntl(this->header_->fd, F_GETFL, 0);
        ::fcntl(this->header_->fd, F_SETFL, s_flags | O_NONBLOCK);

        if (res->ai_family == AF_INET)
            this->address = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        else
            this->address = *reinterpret_cast<sockaddr_in6*>(res->ai_addr);

        detail::ConnectAwaiter aw{
            .op = {},
            .header = this->header_,
        };
        std::memcpy(&aw.op.addr, res->ai_addr, res->ai_addrlen);
        aw.op.addrlen = res->ai_addrlen;

        int c = co_await aw;
        freeaddrinfo(res);

        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::CONNECTION_FAILED))
            co_return usub::utils::errors::ConnectError::Timeout;

        system::this_thread::detail::wh.removeTimer(this->header_->timer_id);

#ifndef UVENT_ENABLE_REUSEADDR
        if (connect_timeout.count() > 0)
            this->header_->state.fetch_sub(1, std::memory_order_acq_rel);
#else
        if (connect_timeout.count() > 0)
        {
            using namespace usub::utils::sync::refc;
            uint64_t& st = this->header_->state;
            const uint64_t cnt = st & COUNT_MASK;
            if (cnt > 0)
                st = (st & ~COUNT_MASK) | ((cnt - 1) & COUNT_MASK);
        }
#endif

        if (c < 0)
        {
            int err = -c;
            ::close(this->header_->fd);
            this->header_->fd = -1;

            if (err == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;

            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (::getsockopt(this->header_->fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) < 0)
        {
            int err = errno;
            ::close(this->header_->fd);
            this->header_->fd = -1;

            if (err == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;

            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        if (so_error != 0)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;

            if (so_error == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;

            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

#ifndef UVENT_ENABLE_REUSEADDR
        this->header_->timeout_epoch_bump();
#endif

        co_return std::nullopt;
    }

    template <Proto p, Role r>
    task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                    uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
    Socket<p, r>::async_connect(std::string&& host, std::string&& port, std::chrono::milliseconds connect_timeout)
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        {
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::GetAddrInfoFailed;
        }

        this->header_->fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (this->header_->fd < 0)
        {
            freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::SocketCreationFailed;
        }

        if (connect_timeout.count() > 0)
        {
            int ms = static_cast<int>(connect_timeout.count());
            ::setsockopt(this->header_->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, static_cast<socklen_t>(sizeof(ms)));

            this->set_timeout_ms(static_cast<timeout_t>(ms));
        }

        int s_flags = ::fcntl(this->header_->fd, F_GETFL, 0);
        ::fcntl(this->header_->fd, F_SETFL, s_flags | O_NONBLOCK);

        if (res->ai_family == AF_INET)
            this->address = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        else
            this->address = *reinterpret_cast<sockaddr_in6*>(res->ai_addr);

        detail::ConnectAwaiter aw{
            .op = {},
            .header = this->header_,
        };
        std::memcpy(&aw.op.addr, res->ai_addr, res->ai_addrlen);
        aw.op.addrlen = res->ai_addrlen;

        int c = co_await aw;
        freeaddrinfo(res);

        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::CONNECTION_FAILED))
            co_return usub::utils::errors::ConnectError::Timeout;

        system::this_thread::detail::wh.removeTimer(this->header_->timer_id);

#ifndef UVENT_ENABLE_REUSEADDR
        if (connect_timeout.count() > 0)
            this->header_->state.fetch_sub(1, std::memory_order_acq_rel);
#else
        if (connect_timeout.count() > 0)
        {
            using namespace usub::utils::sync::refc;
            uint64_t& st = this->header_->state;
            const uint64_t cnt = st & COUNT_MASK;
            if (cnt > 0)
                st = (st & ~COUNT_MASK) | ((cnt - 1) & COUNT_MASK);
        }
#endif

        if (c < 0)
        {
            int err = -c;
            ::close(this->header_->fd);
            this->header_->fd = -1;

            if (err == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;

            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (::getsockopt(this->header_->fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) < 0)
        {
            int err = errno;
            ::close(this->header_->fd);
            this->header_->fd = -1;

            if (err == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;

            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        if (so_error != 0)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;

            if (so_error == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;

            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

#ifndef UVENT_ENABLE_REUSEADDR
        this->header_->timeout_epoch_bump();
#endif

        co_return std::nullopt;
    }

    template <Proto p, Role r>
    task::Awaitable<std::expected<size_t, usub::utils::errors::SendError>,
                    uvent::detail::AwaitableIOFrame<std::expected<size_t, usub::utils::errors::SendError>>>
    Socket<p, r>::async_send(uint8_t* buf, size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        if (!buf || sz == 0)
            co_return static_cast<size_t>(0);

        ssize_t written = co_await this->async_write(buf, sz);
        if (written < 0)
            co_return std::unexpected(usub::utils::errors::SendError::SendFailed);

        co_return static_cast<size_t>(written);
    }

    template <Proto p, Role r>
    std::expected<std::string, usub::utils::errors::SendError> Socket<p, r>::send(uint8_t* buf, size_t sz,
                                                                                  size_t chunkSize, size_t maxSize)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        auto buf_internal = std::unique_ptr<uint8_t[]>(new uint8_t[sz], std::default_delete<uint8_t[]>());
        std::copy_n(buf, sz, buf_internal.get());
        auto sendRes = this->send_aux(buf_internal.get(), sz);
        if (sendRes != -1)
            return std::move(this->receive(chunkSize, maxSize));
        return std::unexpected(usub::utils::errors::SendError::InvalidSocketFd);
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
    Socket<p, r>::async_sendfile(int in_fd, off_t* offset, size_t count)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        detail::SendFileAwaiter aw{
            .op = {},
            .header = this->header_,
        };

        co_await aw;

        if (this->is_disconnected_now())
            co_return -3;

        ssize_t res = ::sendfile(this->header_->fd, in_fd, offset, count);
        if (res == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                this->remove();
#ifdef UVENT_DEBUG
                spdlog::debug("sendfile(linux) EAGAIN: {}", strerror(errno));
#endif
                co_return -1;
            }
            co_return -1;
        }
        if (res > 0)
            this->header_->timeout_epoch_bump();
        co_return res;
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::sendfile(int in_fd, off_t* offset, size_t count)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        ssize_t res = ::sendfile(this->header_->fd, in_fd, offset, count);
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            this->remove();
#ifdef UVENT_DEBUG
            spdlog::debug("sendfile(linux) EAGAIN/EWOULDBLOCK: {}", strerror(errno));
#endif
            return -1;
        }
        return res;
    }

    template <Proto p, Role r>
    void Socket<p, r>::update_timeout(timer_duration_t new_duration) const
    {
        system::this_thread::detail::wh.updateTimer(this->header_->timer_id, new_duration);
    }

    template <Proto p, Role r>
    void Socket<p, r>::shutdown()
    {
        if (!this->header_ || this->header_->fd < 0)
            return;

        ::shutdown(this->header_->fd, SHUT_RDWR);
        this->header_->mark_disconnected();
    }

    template <Proto p, Role r>
    void Socket<p, r>::set_timeout_ms(timeout_t timeout) const
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
#ifndef UVENT_ENABLE_REUSEADDR
        {
            uint64_t s = this->header_->state.load(std::memory_order_relaxed);
            for (;;)
            {
                if (s & usub::utils::sync::refc::CLOSED_MASK)
                    break;

                const uint64_t cnt = (s & usub::utils::sync::refc::COUNT_MASK);
                if (cnt == usub::utils::sync::refc::COUNT_MASK)
                    break;
                const uint64_t ns = (s & ~usub::utils::sync::refc::COUNT_MASK) | (cnt + 1);

                if (this->header_->state.compare_exchange_weak(s, ns, std::memory_order_acq_rel,
                                                               std::memory_order_relaxed))
                    break;
                cpu_relax();
            }
        }
#else
        {
            uint64_t& st = this->header_->state;

            if ((st & usub::utils::sync::refc::CLOSED_MASK) == 0)
            {
                const uint64_t cnt = st & usub::utils::sync::refc::COUNT_MASK;
                if (cnt != usub::utils::sync::refc::COUNT_MASK)
                {
                    st =
                        (st & ~usub::utils::sync::refc::COUNT_MASK) | ((cnt + 1) & usub::utils::sync::refc::COUNT_MASK);
                }
            }
        }
#endif
#if UVENT_DEBUG
        spdlog::debug("set_timeout_ms(io_uring): {}", this->header_->get_counter());
#endif
        auto* timer = new utils::Timer(timeout);
        timer->addFunction(detail::processSocketTimeout, this->header_);
        this->header_->timer_id = system::this_thread::detail::wh.addTimer(timer);
    }

    template <Proto p, Role r>
    void Socket<p, r>::destroy() noexcept
    {
        if (!this->header_)
            return;

        this->header_->close_for_new_refs();

        system::this_thread::detail::pl.removeEvent(this->header_);

        if (this->header_->fd >= 0)
        {
            ::shutdown(this->header_->fd, SHUT_RDWR);
            ::close(this->header_->fd);
            this->header_->fd = -1;
        }

#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.retire(static_cast<void*>(this->header_), &delete_header);
#else
        system::this_thread::detail::q_sh.enqueue(this->header_);
#endif

        this->header_ = nullptr;
    }

    template <Proto p, Role r>
    void Socket<p, r>::remove()
    {
        if (!this->header_)
            return;

        system::this_thread::detail::pl.removeEvent(this->header_);
        this->header_->close_for_new_refs();
    }

    template <Proto p, Role r>
    std::expected<std::string, usub::utils::errors::SendError> Socket<p, r>::receive(size_t chunk_size, size_t maxSize)
    {
        std::string result;
        result.reserve(chunk_size * 2);

        size_t totalReceive{0};
        auto recv_loop = [&](auto&& recv_fn) -> std::expected<std::string, usub::utils::errors::SendError>
        {
            char buffer[chunk_size];
            while (true)
            {
                ssize_t received = recv_fn(buffer, chunk_size);
                totalReceive += received;
                if (totalReceive >= maxSize)
                    break;
                if (received < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    return std::unexpected(usub::utils::errors::SendError::RecvFailed);
                }
                if (received == 0)
                    break;
                result.append(buffer, received);
                if (received < static_cast<ssize_t>(chunk_size))
                    break;
            }
            return result;
        };

        if constexpr (p == Proto::TCP)
        {
            return recv_loop([&](char* buf, size_t sz) { return ::recv(this->header_->fd, buf, sz, 0); });
        }

        try
        {
            return std::visit(
                [&](auto&& addr) -> std::expected<std::string, usub::utils::errors::SendError>
                {
                    using T = std::decay_t<decltype(addr)>;
                    socklen_t addr_len = sizeof(T);
                    return recv_loop(
                        [&](char* buf, size_t sz)
                        {
                            return ::recvfrom(this->header_->fd, buf, sz, 0, reinterpret_cast<sockaddr*>(&addr),
                                              &addr_len);
                        });
                },
                this->address);
        }
        catch (const std::bad_variant_access&)
        {
            return std::unexpected(usub::utils::errors::SendError::InvalidAddressVariant);
        }
    }

    template <Proto p, Role r>
    client_addr_t Socket<p, r>::get_client_addr() const
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        return this->address;
    }

    template <Proto p, Role r>
    client_addr_t Socket<p, r>::get_client_addr()
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        return this->address;
    }

    template <Proto p, Role r>
    utils::net::IPV Socket<p, r>::get_client_ipv() const
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        return this->ipv;
    }

    template <Proto p, Role r>
    size_t Socket<p, r>::send_aux(uint8_t* buf, size_t size)
    {
        if (this->header_->fd < 0)
            return -1;

        if constexpr (p == Proto::TCP)
            return ::send(this->header_->fd, buf, size, 0);

        try
        {
            return std::visit(
                [&](auto&& addr) -> size_t
                {
                    using T = std::decay_t<decltype(addr)>;
                    socklen_t addr_len = sizeof(T);
                    return ::sendto(this->header_->fd, buf, size, 0, reinterpret_cast<sockaddr*>(&addr), addr_len);
                },
                this->address);
        }
        catch (const std::bad_variant_access&)
        {
            return -1;
        }
    }
} // namespace usub::uvent::net

#endif // SOCKETLINUX_IOURING_H
