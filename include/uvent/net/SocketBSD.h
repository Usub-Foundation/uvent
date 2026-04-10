//
// Created by root on 11/27/25.
//

#ifndef SOCKETBSD_H
#define SOCKETBSD_H

#include <coroutine>
#include <expected>

#include <uvent/poll/KPoller.h>
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
        friend class core::KQueuePoller;

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

        [[nodiscard]] task::Awaitable<std::optional<TCPClientSocket>,
                                      uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
        async_accept()
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Asynchronously accepts all pending connections in one kqueue wakeup.
         *
         * kqueue uses EV_CLEAR (edge-triggered) semantics for listen sockets — the
         * kernel notifies once per batch of incoming connections. This overload drains
         * the accept queue completely before suspending, so no buffered connection is
         * silently dropped between wakeups. \p on_accept is invoked for every accepted
         * socket, forwarding \p args on each call.
         *
         * \tparam F    Callable with signature \c void(TCPClientSocket, Args...).
         * \tparam Args Types of additional arguments forwarded to each \p on_accept call.
         *
         * \param on_accept Invoked once per accepted connection, in accept order.
         * \param args      Zero or more additional arguments forwarded to each \p on_accept call.
         */
        template <typename F, typename... Args>
            requires std::invocable<F, TCPClientSocket, Args...>
        [[nodiscard]] task::Awaitable<void> async_accept(F on_accept, Args&&... args)
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
        async_read(uint8_t* buffer, size_t max_read_size)
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

            int cfd = ::accept(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl);
            if (cfd >= 0)
            {
                int flags = fcntl(cfd, F_GETFL, 0);
                if (flags == -1 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) == -1)
                {
                    ::close(cfd);
                    errno = EAGAIN;
                    cfd = -1;
                }
                else
                {
                    int fdflags = fcntl(cfd, F_GETFD, 0);
                    if (fdflags == -1 || fcntl(cfd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
                    {
                        ::close(cfd);
                        errno = EAGAIN;
                        cfd = -1;
                    }
                }
            }

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
            case EAGAIN:
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
    template <typename F, typename... Args>
        requires std::invocable<F, TCPClientSocket, Args...>
    task::Awaitable<void> Socket<p, r>::async_accept(F on_accept, Args&&... args)
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        for (;;)
        {
            for (;;)
            {
                sockaddr_storage ss{};
                socklen_t sl = sizeof(ss);

                int cfd = ::accept(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl);
                if (cfd >= 0)
                {
                    int flags = fcntl(cfd, F_GETFL, 0);
                    if (flags == -1 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) == -1)
                    {
                        ::close(cfd);
                        continue;
                    }
                    int fdflags = fcntl(cfd, F_GETFD, 0);
                    if (fdflags == -1 || fcntl(cfd, F_SETFD, fdflags | FD_CLOEXEC) == -1)
                    {
                        ::close(cfd);
                        continue;
                    }

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
                        on_accept(std::move(sc), std::forward<Args>(args)...);
                    else
                        system::co_spawn(on_accept(std::move(sc), std::forward<Args>(args)...));
                    continue;
                }

                switch (errno)
                {
                case EINTR:
                case ECONNABORTED:
#if defined(EPROTO) && (EPROTO != ECONNABORTED)
                case EPROTO:
#endif
                    continue;

                case ENOBUFS:
                case ENOMEM:
#if defined(ENFILE)
                case ENFILE:
#endif
#if defined(EMFILE)
                case EMFILE:
#endif
                    goto suspend;

                case EAGAIN:
                    goto suspend;

                default:
                    co_return;
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
#if UVENT_DEBUG
        spdlog::info("Entered into read coroutine: fd={}", this->header_ ? this->header_->fd : -1);
#endif

        if (!this->header_ || this->header_->fd < 0)
        {
#if UVENT_DEBUG
            spdlog::error("async_read(buffer): invalid header/fd at start (header={}, fd={})",
                          static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
            co_return -1;
        }

        int retries = 0;
        ssize_t total_read = 0;

        for (;;)
        {
            while (total_read < static_cast<ssize_t>(max_read_size))
            {
                if (!this->header_ || this->header_->fd < 0)
                {
#if UVENT_DEBUG
                    spdlog::error("async_read(buffer): fd became invalid inside loop (header={}, fd={})",
                                  static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
                    co_return (total_read > 0) ? total_read : -1;
                }

                int fd = this->header_->fd;
                uint8_t temp[16384];

                size_t remaining = max_read_size - buffer.size();
                if (remaining == 0)
                    break;

                size_t to_read = std::min(sizeof(temp), remaining);

                ssize_t res = ::recv(fd, temp, to_read, MSG_DONTWAIT);

                if (res > 0)
                {
#if UVENT_DEBUG
                    spdlog::debug("recv fd={} size={}", fd, res);
#endif
                    buffer.append(temp, static_cast<size_t>(res));
                    total_read += res;
                    retries = 0;
                    continue;
                }

                if (res == 0)
                {
#if UVENT_DEBUG
                    spdlog::info("recv fd={} -> EOF", this->header_->fd);
#endif
                    this->remove();
#ifndef UVENT_ENABLE_REUSEADDR
                    if (total_read > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return 0;
                }

                if (errno == EINTR)
                {
                    if (++retries >= settings::max_read_retries)
                    {
#if UVENT_DEBUG
                        spdlog::error("recv fd={} -> EINTR too many times", fd);
#endif
                        this->remove();
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

#if UVENT_DEBUG
                    spdlog::debug("recv fd={} -> EAGAIN, awaiting read", fd);
#endif
                    co_await detail::AwaiterRead{this->header_};

                    if (!this->header_ || this->header_->fd < 0)
                    {
#if UVENT_DEBUG
                        spdlog::info("async_read(buffer): woken by kqueue, but socket is gone (header={}, fd={})",
                                     static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
                        co_return 0;
                    }

#if UVENT_DEBUG
                    spdlog::info("Triggered by kqueue: fd={}", this->header_->fd);
#endif
                    break;
                }

#if UVENT_DEBUG
                spdlog::error("recv fd={} -> errno={}", fd, errno);
#endif
                this->remove();
#ifndef UVENT_ENABLE_REUSEADDR
                if (total_read > 0)
                    this->header_->timeout_epoch_bump();
#endif
                co_return -1;
            }

            if (buffer.size() >= max_read_size)
            {
#ifndef UVENT_ENABLE_REUSEADDR
                if (total_read > 0)
                    this->header_->timeout_epoch_bump();
#endif
                co_return total_read;
            }
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_read(uint8_t* dst,
                                                                                                size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("Entered into read coroutine(raw): fd={}", this->header_ ? this->header_->fd : -1);
#endif

        if (!dst || max_read_size == 0)
            co_return 0;

        if (!this->header_ || this->header_->fd < 0)
        {
#if UVENT_DEBUG
            spdlog::error("async_read(raw): invalid header/fd at start (header={}, fd={})",
                          static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
            co_return -1;
        }

        int retries = 0;
        ssize_t total_read = 0;

        if constexpr (p == Proto::UDP)
        {
            for (;;)
            {
                if (!this->header_ || this->header_->fd < 0)
                {
#if UVENT_DEBUG
                    spdlog::error("async_read(raw-udp): fd invalid (header={}, fd={})",
                                  static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
                    co_return -1;
                }

                int fd = this->header_->fd;
                ssize_t res = ::recvfrom(fd, dst, max_read_size, MSG_DONTWAIT, nullptr, nullptr);

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
#if UVENT_DEBUG
                    spdlog::debug("UDP recv fd={} -> EAGAIN, awaiting read", fd);
#endif
                    co_await detail::AwaiterRead{this->header_};

                    if (!this->header_ || this->header_->fd < 0)
                    {
#if UVENT_DEBUG
                        spdlog::info("async_read(raw-udp): woken but socket gone (header={}, fd={})",
                                     static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
                        co_return 0;
                    }

#if UVENT_DEBUG
                    spdlog::info("Triggered by kqueue: fd={}", this->header_->fd);
#endif
                    continue;
                }

                this->remove();
                co_return -1;
            }
        }
        else
        {
            uint8_t* out = dst;

            for (;;)
            {
                while (total_read < static_cast<ssize_t>(max_read_size))
                {
                    if (!this->header_ || this->header_->fd < 0)
                    {
#if UVENT_DEBUG
                        spdlog::error("async_read(raw-tcp): fd invalid inside loop (header={}, fd={})",
                                      static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
                        co_return (total_read > 0) ? total_read : -1;
                    }

                    int fd = this->header_->fd;
                    size_t remaining = max_read_size - static_cast<size_t>(total_read);
                    if (remaining == 0)
                        break;

                    ssize_t res = ::recv(fd, out, remaining, MSG_DONTWAIT);

                    if (res > 0)
                    {
#if UVENT_DEBUG
                        spdlog::debug("recv fd={} size={}", fd, res);
#endif
                        out += static_cast<size_t>(res);
                        total_read += res;
                        retries = 0;
                        continue;
                    }

                    if (res == 0)
                    {
#if UVENT_DEBUG
                        spdlog::info("recv(raw) fd={} -> EOF", this->header_->fd);
#endif
                        this->remove();
#ifndef UVENT_ENABLE_REUSEADDR
                        if (total_read > 0)
                            this->header_->timeout_epoch_bump();
#endif
                        co_return 0;
                    }

                    if (errno == EINTR)
                    {
                        if (++retries >= settings::max_read_retries)
                        {
                            this->remove();
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

#if UVENT_DEBUG
                        spdlog::debug("recv fd={} -> EAGAIN, awaiting read", fd);
#endif
                        co_await detail::AwaiterRead{this->header_};

                        if (!this->header_ || this->header_->fd < 0)
                        {
#if UVENT_DEBUG
                            spdlog::info("async_read(raw-tcp): woken but socket gone (header={}, fd={})",
                                         static_cast<void*>(this->header_), this->header_ ? this->header_->fd : -1);
#endif
                            co_return 0;
                        }

#if UVENT_DEBUG
                        spdlog::info("Triggered by kqueue: fd={}", this->header_->fd);
#endif
                        break;
                    }

#if UVENT_DEBUG
                    spdlog::error("recv fd={} -> errno={}", fd, errno);
#endif
                    this->remove();
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
                    co_return 0;
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
                    if (total_written >= static_cast<ssize_t>(sz))
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
                    spdlog::info("async_write: EAGAIN, waiting for write-ready: fd={}", this->header_->fd);
#endif
                    co_await detail::AwaiterWrite{this->header_};
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
            size_t remaining = max_read_size - buffer.size();
            if (remaining == 0)
                break;

            size_t to_read = std::min(sizeof(temp), remaining);

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
            ssize_t res = ::send(this->header_->fd, buf_internal.get() + total_written,
                                 sz - static_cast<size_t>(total_written), MSG_DONTWAIT);
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
            this->set_timeout_ms(static_cast<timeout_t>(connect_timeout.count()));

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
            co_return usub::utils::errors::ConnectError::Timeout;
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
            this->set_timeout_ms(static_cast<timeout_t>(connect_timeout.count()));

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
            co_return usub::utils::errors::ConnectError::Timeout;
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
        if (sendRes != static_cast<size_t>(-1))
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

#if defined(__APPLE__)
        off_t off = offset ? *offset : 0;
        auto len = static_cast<off_t>(count);
        int rc = ::sendfile(in_fd, this->header_->fd, off, &len, nullptr, 0);
        if (rc == -1)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && len > 0)
            {
                if (offset)
                    *offset = off + len;
#ifndef UVENT_ENABLE_REUSEADDR
                this->header_->timeout_epoch_bump();
#endif
                co_return static_cast<ssize_t>(len);
            }
#ifdef UVENT_DEBUG
            spdlog::debug("sendfile(darwin) error: {}", strerror(errno));
#endif
            co_return -1;
        }
        if (offset)
            *offset = off + len;
#ifndef UVENT_ENABLE_REUSEADDR
        if (len > 0)
            this->header_->timeout_epoch_bump();
#endif
        co_return static_cast<ssize_t>(len);

#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__)
        off_t off = offset ? *offset : 0;
        off_t sbytes = 0;
        int rc = ::sendfile(in_fd, this->header_->fd, off,
#if defined(__FreeBSD__) || defined(__DragonFly__)
                            static_cast<size_t>(count),
#else
                            static_cast<off_t>(count),
#endif
                            nullptr, &sbytes, 0);
        if (rc == -1)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && sbytes > 0)
            {
                if (offset)
                    *offset = off + sbytes;
#ifndef UVENT_ENABLE_REUSEADDR
                this->header_->timeout_epoch_bump();
#endif
                co_return static_cast<ssize_t>(sbytes);
            }
#ifdef UVENT_DEBUG
            spdlog::debug("sendfile(bsd) error: {}", strerror(errno));
#endif
            co_return -1;
        }
        if (offset)
            *offset = off + sbytes;
#ifndef UVENT_ENABLE_REUSEADDR
        if (sbytes > 0)
            this->header_->timeout_epoch_bump();
#endif
        co_return static_cast<ssize_t>(sbytes);
#else
        (void)in_fd;
        (void)offset;
        (void)count;
        co_return -1;
#endif
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::sendfile(int in_fd, off_t* offset, size_t count)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if defined(__APPLE__)
        off_t off = offset ? *offset : 0;
        auto len = static_cast<off_t>(count);
        int rc = ::sendfile(in_fd, this->header_->fd, off, &len, nullptr, 0);
        if (rc == -1)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && len > 0)
            {
                if (offset)
                    *offset = off + len;
                return static_cast<ssize_t>(len);
            }
            this->remove();
#ifdef UVENT_DEBUG
            spdlog::debug("sendfile(darwin) err: {}", strerror(errno));
#endif
            return -1;
        }
        if (offset)
            *offset = off + len;
        return static_cast<ssize_t>(len);

#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__)
        off_t off = offset ? *offset : 0;
        off_t sbytes = 0;
        int rc = ::sendfile(in_fd, this->header_->fd, off,
#if defined(__FreeBSD__) || defined(__DragonFly__)
                            static_cast<size_t>(count),
#else
                            static_cast<off_t>(count),
#endif
                            nullptr, &sbytes, 0);
        if (rc == -1)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && sbytes > 0)
            {
                if (offset)
                    *offset = off + sbytes;
                return static_cast<ssize_t>(sbytes);
            }
            this->remove();
#ifdef UVENT_DEBUG
            spdlog::debug("sendfile(bsd) err: {}", strerror(errno));
#endif
            return -1;
        }
        if (offset)
            *offset = off + sbytes;
        return static_cast<ssize_t>(sbytes);
#else
        (void)in_fd;
        (void)offset;
        (void)count;
        errno = ENOTSUP;
        return -1;
#endif
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
        system::this_thread::detail::pl.removeEvent(this->header_, core::ALL);
#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.retire(static_cast<void*>(this->header_), &delete_header);
#else
        system::this_thread::detail::q_sh.enqueue(this->header_);
#endif
    }

    template <Proto p, Role r>
    void Socket<p, r>::remove()
    {
        system::this_thread::detail::pl.removeEvent(this->header_, core::ALL);
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
            return static_cast<size_t>(-1);

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
            return static_cast<size_t>(-1);
        }
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(SocketHeader* header) noexcept : header_(header)
    {
    }
} // namespace usub::uvent::net

#endif // SOCKETBSD_H
