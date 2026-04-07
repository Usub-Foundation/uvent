//
// Created by root on 11/27/25.
//

#ifndef SOCKETLINUX_H
#define SOCKETLINUX_H

#include <coroutine>
#include <expected>
#include <netinet/tcp.h>

#include <uvent/poll/EPoller.h>
#include "AwaiterOperations.h"
#include "SocketMetadata.h"
#include "uvent/base/Predefines.h"
#include "uvent/system/Defines.h"
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
    }

    template <Proto p, Role r>
    class Socket : usub::utils::sync::refc::RefCounted<Socket<p, r>>
    {
    public:
        friend class usub::utils::sync::refc::RefCounted<Socket<p, r>>;
        friend class core::EPoller;

        friend void detail::processSocketTimeout(std::any arg);

        /**
         * \brief Default constructor.
         * Creates an uninitialized socket object with no file descriptor.
         */
        Socket() noexcept;

        /**
         * \brief Constructs a socket from an existing file descriptor.
         *
         * Initializes the socket object to wrap the given file descriptor.
         * The descriptor must be valid and owned by the caller.
         */
        explicit Socket(int fd) noexcept;

        /**
         * \brief Constructs a passive TCP socket bound to given address/port (lvalue ip).
         * Used for listening sockets (bind + listen).
         */
        explicit Socket(std::string& ip_addr, int port = 8080, int backlog = 50, utils::net::IPV ipv = utils::net::IPV4,
                        utils::net::SocketAddressType socketAddressType = utils::net::TCP) noexcept
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Constructs a passive TCP socket bound to given address/port (rvalue ip).
         * Used for listening sockets (bind + listen).
         */
        explicit Socket(std::string&& ip_addr, int port = 8080, int backlog = 50,
                        utils::net::IPV ipv = utils::net::IPV4,
                        utils::net::SocketAddressType socketAddressType = utils::net::TCP) noexcept
            requires(p == Proto::TCP && r == Role::PASSIVE);

        explicit Socket(SocketHeader* header) noexcept;

        /**
         * \brief Copy constructor.
         * Duplicates the socket object header (but not the underlying FD).
         */
        Socket(const Socket& o) noexcept;

        /**
         * \brief Move constructor.
         * Transfers ownership of the socket header and FD from another socket.
         */
        Socket(Socket&& o) noexcept;

        /**
         * \brief Copy assignment operator.
         */
        Socket& operator=(const Socket& o) noexcept;

        /**
         * \brief Move assignment operator.
         * Transfers ownership of the socket header and FD.
         */
        Socket& operator=(Socket&& o) noexcept;

        /**
         * \brief Destructor.
         * Releases resources and closes the underlying FD if owned.
         */
        ~Socket();

        /**
         * \brief Wraps an existing SocketHeader into a Socket object.
         * Used for constructing Socket from raw header pointer.
         */
        static Socket from_existing(SocketHeader* header);

        /**
         * \brief Returns the raw header pointer associated with this socket.
         */
        SocketHeader* get_raw_header();

        /// \deprecated Use async_accept(F on_accept) to drain all pending connections per ET wakeup.
        [[nodiscard, deprecated("Use async_accept(F on_accept) to drain all pending connections per ET wakeup")]] task::
            Awaitable<std::optional<TCPClientSocket>, uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
            async_accept()
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Asynchronously accepts all pending connections in one ET wakeup.
         *
         * Drains the kernel accept queue completely before suspending. In ET mode,
         * epoll fires once per edge — stopping after the first connection would silently
         * lose all others buffered since the last wakeup. This overload fixes that by
         * looping until EAGAIN and invoking \p on_accept for every accepted socket.
         *
         * Usage:
         * \code
         * for (;;)
         *     co_await acceptor->async_accept([](net::TCPClientSocket socket) {
         *         system::co_spawn(clientCoro(std::move(socket)));
         *     });
         * \endcode
         *
         * \tparam F Callable with signature \c void(TCPClientSocket).
         * \param on_accept Invoked once per accepted connection, in accept order.
         */
        template <typename F>
            requires std::invocable<F, TCPClientSocket>
        [[nodiscard]] task::Awaitable<void> async_accept(F on_accept)
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Asynchronously reads data into the buffer.
         * Waits for EPOLLIN event and reads up to max_read_size bytes into the given buffer.
         */
        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
        async_read(utils::DynamicBuffer& buffer, size_t max_read_size)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Asynchronously reads data into the buffer.
         * Waits for EPOLLIN event and reads up to max_read_size bytes into the given buffer.
         */
        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
        async_read(uint8_t* dst, size_t max_read_size)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Asynchronously writes data from the buffer.
         * Waits for EPOLLOUT event and attempts to write sz bytes from buf.
         */
        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> async_write(uint8_t* buf,
                                                                                                     size_t sz)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Synchronously reads data into the buffer.
         * Performs a blocking read up to max_read_size bytes.
         */
        [[nodiscard]] ssize_t read(utils::DynamicBuffer& buffer, size_t max_read_size)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Synchronously writes data from the buffer.
         * Performs a blocking write of sz bytes from buf.
         */
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

        /**
         * \brief Asynchronously sends data with chunking.
         * Sends data in chunks of chunkSize up to maxSize total. Waits for EPOLLOUT readiness.
         */
        task::Awaitable<std::expected<size_t, usub::utils::errors::SendError>,
                        uvent::detail::AwaitableIOFrame<std::expected<size_t, usub::utils::errors::SendError>>>
        async_send(uint8_t* buf, size_t sz)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Synchronously sends data with chunking.
         * Sends data in chunks of chunkSize up to maxSize total.
         */
        [[nodiscard]] std::expected<std::string, usub::utils::errors::SendError>
        send(uint8_t* buf, size_t sz, size_t chunkSize = 16384, size_t maxSize = 65536)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Asynchronously sends file contents over the socket.
         * Waits for EPOLLOUT readiness, then sends data from in_fd using sendfile.
         */
        [[nodiscard]] task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
        async_sendfile(int in_fd, off_t* offset, size_t count)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Synchronously sends file contents over the socket.
         * Wrapper over the sendfile syscall.
         */
        [[nodiscard]] ssize_t sendfile(int in_fd, off_t* offset, size_t count)
            requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP));

        /**
         * \brief Updates the socket's timeout in the timer subsystem.
         */
        void update_timeout(timer_duration_t new_duration) const;

        /**
         * \brief Gracefully shuts down the socket.
         * Calls shutdown() on underlying FD.
         */
        void shutdown();

        /**
         * \brief Sets timeout to associated socket.
         * \warning Method doesn't check if socket was initialized. Please use it only after socket
         * initialisation.
         */
        void set_timeout_ms(timeout_t timeout = settings::timeout_duration_ms) const
            requires(p == Proto::TCP && r == Role::ACTIVE);

        std::expected<std::string, usub::utils::errors::SendError> receive(size_t chunk_size, size_t maxSize);

        /**
         * \brief Returns the client network address (IPv4 or IPv6) associated with this socket.
         *
         * The type alias \c client_addr_t is defined as:
         * \code
         * typedef std::variant<sockaddr_in, sockaddr_in6> client_addr_t;
         * \endcode
         * allowing the caller to handle both IPv4 and IPv6 endpoints transparently.
         *
         * \return The client address variant.
         */
        [[nodiscard]] client_addr_t get_client_addr() const
            requires(p == Proto::TCP && r == Role::ACTIVE);

        /**
         * \brief Returns the client network address (IPv4 or IPv6) associated with this socket.
         *
         * Non-const overload allowing modifications to the returned structure if necessary.
         *
         * \return The client address variant.
         */
        [[nodiscard]] client_addr_t get_client_addr()
            requires(p == Proto::TCP && r == Role::ACTIVE);

        /**
         * \brief Returns the IP version (IPv4 or IPv6) of the connected peer.
         *
         * Determines whether the underlying active TCP socket is using an IPv4 or IPv6 address
         * family.
         *
         * \return utils::net::IPV enum value indicating the IP version.
         */
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
    Socket<p, r>::Socket(std::string& ip_addr, int port, int backlog, utils::net::IPV ipv,
                         utils::net::SocketAddressType socketAddressType) noexcept
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        this->header_ =
            new SocketHeader{.fd = utils::socket::createSocket(port, ip_addr, backlog, ipv, socketAddressType),
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
    Socket<p, r>::Socket(std::string&& ip_addr, int port, int backlog, utils::net::IPV ipv,
                         utils::net::SocketAddressType socketAddressType) noexcept
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        this->header_ =
            new SocketHeader{.fd = utils::socket::createSocket(port, ip_addr, backlog, ipv, socketAddressType),
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
    Socket<p, r>::~Socket()
    {
        if (this->header_)
        {
            this->release();
#if UVENT_DEBUG
            spdlog::warn("Socket counter: {}, fd: {}", (this->header_->state & usub::utils::sync::refc::COUNT_MASK),
                         this->header_->fd);
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

            int cfd;
            cfd = ::accept4(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (cfd >= 0)
            {
                auto* h = new SocketHeader{.fd = cfd,
                                           .socket_info = uint8_t(Proto::TCP) | uint8_t(Role::ACTIVE),
                                           .state = (1ull & usub::utils::sync::refc::COUNT_MASK)};
                system::this_thread::detail::pl.addEvent(h, core::OperationType::READ);

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

            switch (errno)
            {
            case EINTR:
            case ECONNABORTED:
            case EPROTO:
#if defined(EPROTO) && (EPROTO != ECONNABORTED)
                continue;
#else
                continue;
#endif
            case EAGAIN: // same for EWOULDBLOCK (EWOULDBLOCK = EAGAIN = 11)
                co_await detail::AwaiterAccept{this->header_};
                continue;

            case ENOBUFS:
            case ENOMEM:
#if defined(ENFILE)
            case ENFILE:
#endif
#if defined(EMFILE)
            case EMFILE:
#endif
                co_await detail::AwaiterAccept{this->header_};
                continue;

            case EBADF:
            case ENOTSOCK:
            case EOPNOTSUPP:
            case EINVAL:
            case EACCES:
            case EFAULT:
            default:
                co_return std::nullopt;
            }
        }
    }

    template <Proto p, Role r>
    template <typename F>
        requires std::invocable<F, TCPClientSocket>
    task::Awaitable<void> Socket<p, r>::async_accept(F on_accept)
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        for (;;)
        {
            // Drain the accept queue completely before suspending.
            // In ET mode, epoll fires once per edge — if we stop at the first
            // EAGAIN we've consumed all available connections for this wakeup.
            for (;;)
            {
                sockaddr_storage ss{};
                socklen_t sl = sizeof(ss);

                int cfd =
                    ::accept4(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd >= 0)
                {
                    auto* h = new SocketHeader{.fd = cfd,
                                               .socket_info = uint8_t(Proto::TCP) | uint8_t(Role::ACTIVE),
                                               .state = (1ull & usub::utils::sync::refc::COUNT_MASK)};
                    system::this_thread::detail::pl.addEvent(h, core::OperationType::READ);

                    TCPClientSocket sc(h);
                    if (ss.ss_family == AF_INET)
                        sc.address = *reinterpret_cast<sockaddr_in*>(&ss);
                    else if (ss.ss_family == AF_INET6)
                    {
                        sc.address = *reinterpret_cast<sockaddr_in6*>(&ss);
                        sc.ipv = utils::net::IPV6;
                    }

                    if constexpr (std::is_void_v<std::invoke_result_t<F, TCPClientSocket>>)
                        on_accept(std::move(sc)); // лямбда возвращает void — как раньше
                    else
                        system::co_spawn(on_accept(std::move(sc))); // корутина — спавним сразу
                    continue;
                }

                switch (errno)
                {
                case EINTR:
                case ECONNABORTED:
                case EPROTO:
                    continue; // transient — retry immediately

                case ENOBUFS:
                case ENOMEM:
#if defined(ENFILE)
                case ENFILE:
#endif
#if defined(EMFILE)
                case EMFILE:
#endif
                    // Resource pressure — yield to the scheduler and retry on next wakeup.
                    goto suspend;

                case EAGAIN: // == EWOULDBLOCK
                    goto suspend; // queue empty — suspend until next ET edge

                default:
                    co_return; // unrecoverable error
                }
            }

        suspend:
            co_await detail::AwaiterAccept{this->header_};
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
    Socket<p, r>::async_read(utils::DynamicBuffer& buffer, size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        if (max_read_size == 0)
            co_return 0;

        int retries = 0;

        if constexpr (p == Proto::UDP)
        {
            for (;;)
            {
                uint8_t* dst = buffer.reserve_tail(max_read_size);

                ssize_t res = ::recvfrom(this->header_->fd, dst, max_read_size, 0, nullptr, nullptr);

                if (res > 0)
                {
                    buffer.commit(static_cast<size_t>(res));
#ifndef UVENT_ENABLE_REUSEADDR
                    this->header_->timeout_epoch_bump();
#endif
                    co_return res;
                }

                if (res == 0)
                    co_return 0;

                if (errno == EINTR)
                {
                    if (++retries >= settings::max_read_retries)
                    {
                        this->remove();
                        co_return -1;
                    }
                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    co_await detail::AwaiterRead{this->header_};
                    continue;
                }

                this->remove();
                co_return -1;
            }
        }
        else
        {
            ssize_t total_read = 0;

            for (;;)
            {
                while (total_read < static_cast<ssize_t>(max_read_size))
                {
                    const size_t remaining = max_read_size - static_cast<size_t>(total_read);

                    const size_t to_read = remaining > 16384 ? 16384 : remaining;

                    uint8_t* dst = buffer.reserve_tail(to_read);

                    ssize_t res = ::recv(this->header_->fd, dst, to_read, 0);

                    if (res > 0)
                    {
                        buffer.commit(static_cast<size_t>(res));
                        total_read += res;
                        retries = 0;
                        continue;
                    }

                    if (res == 0)
                    {
#ifndef UVENT_ENABLE_REUSEADDR
                        if (total_read > 0)
                            this->header_->timeout_epoch_bump();
#endif
                        co_return total_read;
                    }

                    if (errno == EINTR)
                    {
                        if (++retries >= settings::max_read_retries)
                        {
#ifndef UVENT_ENABLE_REUSEADDR
                            if (total_read > 0)
                                this->header_->timeout_epoch_bump();
#endif
                            co_return -1;
                        }
                        continue;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        if (total_read > 0)
                        {
#ifndef UVENT_ENABLE_REUSEADDR
                            this->header_->timeout_epoch_bump();
#endif
                            co_return total_read;
                        }

                        co_await detail::AwaiterRead{this->header_};
                        break;
                    }

#ifndef UVENT_ENABLE_REUSEADDR
                    if (total_read > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return -1;
                }

                if (total_read >= static_cast<ssize_t>(max_read_size))
                {
#ifndef UVENT_ENABLE_REUSEADDR
                    if (total_read > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return total_read;
                }
            }
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_read(uint8_t* dst,
                                                                                                size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        if (!dst || max_read_size == 0)
            co_return 0;

        int retries = 0;

        if constexpr (p == Proto::UDP)
        {
            for (;;)
            {
                ssize_t res = ::recvfrom(this->header_->fd, dst, max_read_size, 0, nullptr, nullptr);

                if (res > 0)
                {
#ifndef UVENT_ENABLE_REUSEADDR
                    this->header_->timeout_epoch_bump();
#endif
                    co_return res;
                }

                if (res == 0)
                    co_return 0;

                if (errno == EINTR)
                {
                    if (++retries >= settings::max_read_retries)
                    {
                        this->remove();
                        co_return -1;
                    }
                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    co_await detail::AwaiterRead{this->header_};
                    continue;
                }

                this->remove();
                co_return -1;
            }
        }
        else
        {
            ssize_t total_read = 0;
            uint8_t* out = dst;

            for (;;)
            {
                while (total_read < static_cast<ssize_t>(max_read_size))
                {
                    const size_t remaining = max_read_size - static_cast<size_t>(total_read);

                    ssize_t res = ::recv(this->header_->fd, out, remaining, 0);

                    if (res > 0)
                    {
                        out += static_cast<size_t>(res);
                        total_read += res;
                        retries = 0;
                        continue;
                    }

                    if (res == 0)
                    {
#ifndef UVENT_ENABLE_REUSEADDR
                        if (total_read > 0)
                            this->header_->timeout_epoch_bump();
#endif
                        co_return total_read;
                    }

                    if (errno == EINTR)
                    {
                        if (++retries >= settings::max_read_retries)
                        {
#ifndef UVENT_ENABLE_REUSEADDR
                            if (total_read > 0)
                                this->header_->timeout_epoch_bump();
#endif
                            co_return -1;
                        }
                        continue;
                    }

                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        if (total_read > 0)
                        {
#ifndef UVENT_ENABLE_REUSEADDR
                            this->header_->timeout_epoch_bump();
#endif
                            co_return total_read;
                        }

                        co_await detail::AwaiterRead{this->header_};
                        break;
                    }

#ifndef UVENT_ENABLE_REUSEADDR
                    if (total_read > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return -1;
                }

                if (total_read >= static_cast<ssize_t>(max_read_size))
                {
#ifndef UVENT_ENABLE_REUSEADDR
                    if (total_read > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return total_read;
                }
            }
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_write(uint8_t* buf,
                                                                                                 size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("Entered into write coroutine: fd={}, sz={}", this->header_->fd, sz);
#endif
        if (!buf || sz == 0)
        {
            co_return 0;
        }

        if constexpr (p == Proto::UDP)
        {
            int retries = 0;
            for (;;)
            {
                ssize_t res = ::send(this->header_->fd, buf, sz, MSG_DONTWAIT);
                if (res >= 0)
                {
#ifndef UVENT_ENABLE_REUSEADDR
                    if (res > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return res;
                }
                if (errno == EINTR)
                {
                    if (++retries >= settings::max_write_retries)
                        co_return -1;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    co_await detail::AwaiterWrite{this->header_};
                    continue;
                }
                co_return -1;
            }
        }
        else
        {
            ssize_t total_written = 0;
            int retries_eintr = 0;

            while (total_written < static_cast<ssize_t>(sz))
            {
                ssize_t res = ::send(this->header_->fd, buf + total_written, sz - static_cast<size_t>(total_written),
                                     MSG_DONTWAIT);
                if (res > 0)
                {
                    total_written += res;
                    retries_eintr = 0;
                    if (total_written >= sz)
                    {
#ifndef UVENT_ENABLE_REUSEADDR
                        if (total_written > 0)
                            this->header_->timeout_epoch_bump();
#endif
                        co_return total_written;
                    }
                    continue;
                }
                if (res == 0)
                {
                    co_return -1;
                }
                if (errno == EINTR)
                {
                    if (++retries_eintr >= settings::max_write_retries)
                    {
                        co_return -1;
                    }
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
#if UVENT_DEBUG
                    spdlog::info("async_write: EAGAIN, waiting for EPOLLOUT: fd={}", this->header_->fd);
#endif
                    co_await detail::AwaiterWrite{this->header_};
                    continue;
                }
                co_return -1;
            }
        }
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
                {
                    break;
                }
                else if (errno == EINTR)
                {
                    if (++retries >= settings::max_read_retries)
                    {
                        return -1;
                    }
                    continue;
                }
                else
                {
                    return -1;
                }
            }

            if (buffer.size() >= max_read_size)
            {
                break;
            }
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

        while (total_written < sz)
        {
            ssize_t res =
                ::send(this->header_->fd, buf_internal.get() + total_written, sz - total_written, MSG_DONTWAIT);
            if (res > 0)
            {
                total_written += res;
                retries = 0;
                continue;
            }
            else if (res == -1)
            {
                if (errno == EINTR)
                {
                    if (++retries >= settings::max_write_retries)
                    {
                        return -1;
                    }
                    continue;
                }
                else if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                else
                {
                    return -1;
                }
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

        int ret = ::connect(this->header_->fd, res->ai_addr, res->ai_addrlen);
        if (ret < 0 && errno != EINPROGRESS)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);

        co_await detail::AwaiterWrite{this->header_};

        freeaddrinfo(res);

        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::CONNECTION_FAILED))
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::Timeout;
        }

        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(this->header_->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        if (err != 0)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            if (err == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::wh.removeTimer(this->header_->timer_id);

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

        int ret = ::connect(this->header_->fd, res->ai_addr, res->ai_addrlen);
        if (ret < 0 && errno != EINPROGRESS)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);

        co_await detail::AwaiterWrite{this->header_};

        freeaddrinfo(res);

        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::CONNECTION_FAILED))
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::Timeout;
        }

        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(this->header_->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        if (err != 0)
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            if (err == ETIMEDOUT)
                co_return usub::utils::errors::ConnectError::Timeout;
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::wh.removeTimer(this->header_->timer_id);

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
        auto buf_internal = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
        std::memcpy(buf_internal.get(), buf, sz);

        ssize_t total_written = 0;
        int retries = 0;

        while (total_written < static_cast<ssize_t>(sz))
        {
            for (;;)
            {
                if (this->is_disconnected_now())
                    co_return std::unexpected(usub::utils::errors::SendError::Closed);

                ssize_t res = 0;

                if constexpr (p == Proto::TCP)
                {
                    res = ::send(this->header_->fd, buf_internal.get() + total_written,
                                 sz - static_cast<size_t>(total_written), MSG_DONTWAIT);
                }
                else
                {
                    try
                    {
                        if (std::holds_alternative<sockaddr_in>(this->address))
                        {
                            auto& addr = std::get<sockaddr_in>(this->address);
                            socklen_t addr_len = sizeof(sockaddr_in);
                            res = ::sendto(this->header_->fd, buf_internal.get() + total_written,
                                           sz - static_cast<size_t>(total_written), MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr*>(&addr), addr_len);
                        }
                        else if (std::holds_alternative<sockaddr_in6>(this->address))
                        {
                            auto& addr = std::get<sockaddr_in6>(this->address);
                            socklen_t addr_len = sizeof(sockaddr_in6);
                            res = ::sendto(this->header_->fd, buf_internal.get() + total_written,
                                           sz - static_cast<size_t>(total_written), MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr*>(&addr), addr_len);
                        }
                        else
                        {
                            co_return std::unexpected(usub::utils::errors::SendError::InvalidAddressVariant);
                        }
                    }
                    catch (const std::bad_variant_access&)
                    {
                        co_return std::unexpected(usub::utils::errors::SendError::InvalidAddressVariant);
                    }
                }

                if (res > 0)
                {
                    total_written += res;
                    retries = 0;

                    if (total_written >= static_cast<ssize_t>(sz))
                        goto send_done;

                    continue;
                }

                if (res == 0)
                {
                    this->remove();
                    co_return std::unexpected(usub::utils::errors::SendError::SendFailed);
                }

                if (errno == EINTR)
                {
                    if (++retries >= settings::max_write_retries)
                    {
                        this->remove();
                        co_return std::unexpected(usub::utils::errors::SendError::SendFailed);
                    }
                    continue;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }

                this->remove();
                co_return std::unexpected(usub::utils::errors::SendError::SendFailed);
            }

            co_await detail::AwaiterWrite{this->header_};
        }

    send_done:
#ifndef UVENT_ENABLE_REUSEADDR
        if (total_written > 0)
            this->header_->timeout_epoch_bump();
#endif
        co_return static_cast<size_t>(total_written);
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
        co_await detail::AwaiterWrite{this->header_};
        if (this->is_disconnected_now())
            co_return -3;

        ssize_t sent = -1;

        off_t* offp = offset;
        ssize_t res = ::sendfile(this->header_->fd, in_fd, offp, count);
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
        ::shutdown(this->header_->fd, SHUT_RDWR);
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
        spdlog::debug("set_timeout_ms: {}", this->header_->get_counter());
#endif
        auto* timer = new utils::Timer(timeout);
        timer->addFunction(detail::processSocketTimeout, this->header_);
        this->header_->timer_id = system::this_thread::detail::wh.addTimer(timer);
    }

    template <Proto p, Role r>
    void Socket<p, r>::destroy() noexcept
    {
        this->header_->close_for_new_refs();
        system::this_thread::detail::pl.removeEvent(this->header_);
#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.retire(static_cast<void*>(this->header_), &delete_header);
#else
        system::this_thread::detail::q_sh.enqueue(this->header_);
#endif
    }

    template <Proto p, Role r>
    void Socket<p, r>::remove()
    {
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

    template <Proto p, Role r>
    Socket<p, r>::Socket(SocketHeader* header) noexcept : header_(header)
    {
    }
} // namespace usub::uvent::net

#endif // SOCKETLINUX_H
