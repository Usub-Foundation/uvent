//
// Created by root on 11/27/25.
//

#ifndef SOCKETLINUX_H
#define SOCKETLINUX_H

#include <algorithm>
#include <utility>
#include <coroutine>
#include <expected>
#include <netinet/tcp.h>

#include <uvent/poll/EPoller.h>
#include "AwaiterOperations.h"
#include "Resolver.h"
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

#ifndef UVENT_ENABLE_REUSEADDR
        /// TimerWheel REMOVE done-callback: the wheel no longer references the embedded
        /// Timer, so the enclosing SocketHeader may now be retired (freed after QSBR grace).
        inline void retire_header_after_timer(void* header) noexcept
        {
            system::this_thread::detail::g_qsbr.retire(header, &delete_header);
        }
#endif

#ifdef UVENT_SOCKET_OWNER_FORWARDING
        /**
         * \brief Owner-forwarding of socket maintenance (UVENT_ENABLE_REUSEADDR).
         *
         * Poller, timer wheel and the header delete queue are thread_local, and
         * the socket timer is embedded in SocketHeader. If a coroutine that
         * migrated to another worker cancels the timer / destroys the socket
         * "locally", it hits a foreign wheel (no-op, id collision in
         * cancelledPending_), a foreign epoll and frees a header the owner's
         * wheel still points to (heap-use-after-free in TimerWheel::tick).
         * Every wheel/poller/delete operation is therefore executed on the
         * owner (SocketHeader::owner_tid, stamped by PollerImpl::addEvent);
         * foreign callers enqueue a SocketOp and wake the owner.
         *
         * Refcount rules are unchanged: the thread that arms the timer takes the
         * timer's reference, and the timer's reference is dropped exactly once —
         * by whoever claims it first via SocketHeader::tflags (timeout callback
         * on the owner, or shutdown() on any thread).
         */
        namespace tflags
        {
            /// the timer's reference has been consumed (fired or cancelled)
            inline constexpr uint8_t TIMER_CLAIMED = 1u << 0;
            /// Destroy has been forwarded to the owner; nobody may touch refs anymore
            inline constexpr uint8_t TEARDOWN_PENDING = 1u << 1;
            /// teardown done on the owner; delete once pending_ops == 0
            inline constexpr uint8_t DESTROYED = 1u << 2;
        } // namespace tflags

        /// \return true if `h` belongs to another worker and ops must be forwarded.
        UVENT_ALWAYS_INLINE_FN bool is_foreign_owner(const SocketHeader* h) noexcept
        {
            return h->owner_tid >= 0 && h->owner_tid != system::this_thread::detail::t_id &&
                system::global::detail::tls_registry != nullptr;
        }

        UVENT_ALWAYS_INLINE_FN void forward_socket_op(thread::SocketOp::Kind kind, SocketHeader* h,
                                                      uint64_t timeout_ms = 0)
        {
            // keeps the header alive until the owner has applied this op
            h->pending_ops.fetch_add(1, std::memory_order_acq_rel);
            system::global::detail::tls_registry->getStorage(h->owner_tid)
                ->push_socket_op(thread::SocketOp{.kind = kind, .header = h, .timeout_ms = timeout_ms});
        }

        /// Teardown on the owning thread: cancel timer, close fd, and free the header
        /// now (or, if forwarded ops are still queued for it, after the last of them).
        void teardown_socket_header_local(SocketHeader* h) noexcept;

        /// Arm (timer_id == 0) or refresh the embedded socket timer on the current
        /// thread's wheel. The caller must already hold the timer's reference when
        /// arming. Shared by the local path and the owner-side op handler.
        void arm_or_refresh_socket_timer(SocketHeader* h, uint64_t timeout_ms) noexcept;

        /// Applied by the owner in its event loop (Thread::processSocketOps).
        void apply_socket_op(const thread::SocketOp& op) noexcept;
#endif
    } // namespace detail

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
        explicit Socket(std::string& ip_addr, int port = 8080, int backlog = SOMAXCONN,
                        utils::net::IPV ipv = utils::net::IPV4,
                        utils::net::SocketAddressType socketAddressType = utils::net::TCP) noexcept
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Constructs a passive TCP socket bound to given address/port (rvalue ip).
         * Used for listening sockets (bind + listen).
         */
        explicit Socket(std::string&& ip_addr, int port = 8080, int backlog = SOMAXCONN,
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

        task::Awaitable<std::optional<TCPClientSocket>, uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
        async_accept()
            requires(p == Proto::TCP && r == Role::PASSIVE);

        /**
         * \brief Asynchronously accepts all pending connections in one ET wakeup.
         *
         * Drains the kernel accept queue completely before suspending. In ET mode,
         * epoll fires once per edge — stopping after the first connection would silently
         * lose all others buffered since the last wakeup. This overload fixes that by
         * looping until EAGAIN and invoking \p on_accept for every accepted socket,
         * forwarding \p args on each call.
         *
         * Usage:
         * \code
         * for (;;)
         *     co_await acceptor->async_accept(
         *         [](net::TCPClientSocket socket, auto&&... extras) {
         *             system::co_spawn(clientCoro(std::move(socket), extras...));
         *         }, arg1, arg2);
         * \endcode
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

        /**
         * \brief Enables/disables TCP_NODELAY (Nagle's algorithm) on the underlying socket.
         * Applies to any connected TCP socket regardless of which thread owns it.
         * \return true if the option was applied.
         * \warning Method doesn't check if socket was initialized. Please use it only after socket
         * initialisation.
         */
        bool set_nodelay(bool enable = true) const noexcept
            requires(p == Proto::TCP);

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
                             .state = std::atomic<uint64_t>((1ull & usub::utils::sync::refc::COUNT_MASK))};
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
                             .state = std::atomic<uint64_t>((1ull & usub::utils::sync::refc::COUNT_MASK))};
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
            spdlog::warn("Socket counter: {}, fd: {}",
                         (this->header_->state.load(std::memory_order_acquire) & usub::utils::sync::refc::COUNT_MASK),
                         this->header_->fd);
#endif
        }
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(const Socket& o) noexcept : address(o.address), ipv(o.ipv), header_(o.header_)
    {
        if (this->header_)
            this->add_ref();
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(Socket&& o) noexcept : address(o.address), ipv(o.ipv), header_(o.header_)
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
        this->address = tmp.address;
        this->ipv = tmp.ipv;
        return *this;
    }

    template <Proto p, Role r>
    Socket<p, r>& Socket<p, r>::operator=(Socket&& o) noexcept
    {
        if (this == &o)
            return *this;
        Socket tmp(std::move(o));
        std::swap(this->header_, tmp.header_);
        this->address = tmp.address;
        this->ipv = tmp.ipv;
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
            if (this->header_->is_read_armed()) this->header_->disarm_read();
            cfd = ::accept4(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);

            if (cfd >= 0)
            {
                {
                    int one = 1;
                    int busy_us = 50;
                    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_BUSY_POLL
                    ::setsockopt(cfd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif
#ifdef SO_PREFER_BUSY_POLL
                    ::setsockopt(cfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one));
#endif
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
                co_return sc;
            }

            switch (errno)
            {
            case EINTR:
            case ECONNABORTED:
            case EPROTO:
                continue;
            case EAGAIN: // same for EWOULDBLOCK (EWOULDBLOCK = EAGAIN = 11)
                co_await detail::AwaiterAccept{this->header_};
                if (system::this_coroutine::cancel_requested()) [[unlikely]]
                    co_return std::nullopt;
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
                if (system::this_coroutine::cancel_requested()) [[unlikely]]
                    co_return std::nullopt;
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

                if (this->header_->is_read_armed()) this->header_->disarm_read();
                int cfd =
                    ::accept4(this->header_->fd, reinterpret_cast<sockaddr*>(&ss), &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd >= 0)
                {
                    {
                        int one = 1;
                        int busy_us = 50;
                        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_BUSY_POLL
                        ::setsockopt(cfd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif
#ifdef SO_PREFER_BUSY_POLL
                        ::setsockopt(cfd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one));
#endif
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
                    {
#ifndef UVENT_ENABLE_REUSEADDR
                        system::co_spawn(on_accept(std::move(sc), std::forward<Args>(args)...));
#else
                        system::co_spawn_static(on_accept(std::move(sc), std::forward<Args>(args)...),
                                                system::this_thread::detail::t_id);
#endif
                    }
                    continue;
                }

                switch (errno)
                {
                case EINTR:
                case ECONNABORTED:
                case EPROTO:
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
            if (system::this_coroutine::cancel_requested()) [[unlikely]]
                co_return;
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
    Socket<p, r>::async_read(utils::DynamicBuffer& buffer, size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        if (max_read_size == 0)
            co_return 0;

        if (!system::coop::consume()) [[unlikely]]
            co_await system::this_coroutine::yield();

        int retries = 0;

        if constexpr (p == Proto::UDP)
        {
            for (;;)
            {
                uint8_t* dst = buffer.reserve_tail(max_read_size);

                if (this->header_->is_read_armed()) this->header_->disarm_read();
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
                    if (system::this_coroutine::cancel_requested()) [[unlikely]]
                    {
                        errno = ECANCELED;
                        co_return -1;
                    }
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

                    if (this->header_->is_read_armed()) this->header_->disarm_read();
                    ssize_t res = ::recv(this->header_->fd, dst, to_read, 0);

                    if (res > 0)
                    {
                        buffer.commit(static_cast<size_t>(res));
                        total_read += res;
                        retries = 0;
                        if (static_cast<size_t>(res) < to_read)
                        {
#ifndef UVENT_ENABLE_REUSEADDR
                            this->header_->timeout_epoch_bump();
#endif
                            co_return total_read;
                        }
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
                        if (system::this_coroutine::cancel_requested()) [[unlikely]]
                        {
                            errno = ECANCELED;
                            co_return -1;
                        }
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

        if (!system::coop::consume()) [[unlikely]]
            co_await system::this_coroutine::yield();

        int retries = 0;

        if constexpr (p == Proto::UDP)
        {
            for (;;)
            {
                if (this->header_->is_read_armed()) this->header_->disarm_read();
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
                    if (system::this_coroutine::cancel_requested()) [[unlikely]]
                    {
                        errno = ECANCELED;
                        co_return -1;
                    }
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

                    if (this->header_->is_read_armed()) this->header_->disarm_read();
                    ssize_t res = ::recv(this->header_->fd, out, remaining, 0);

                    if (res > 0)
                    {
                        out += static_cast<size_t>(res);
                        total_read += res;
                        retries = 0;
                        if (static_cast<size_t>(res) < remaining)
                        {
#ifndef UVENT_ENABLE_REUSEADDR
                            this->header_->timeout_epoch_bump();
#endif
                            co_return total_read;
                        }
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
                        if (system::this_coroutine::cancel_requested()) [[unlikely]]
                        {
                            errno = ECANCELED;
                            co_return -1;
                        }
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

        if (!system::coop::consume()) [[unlikely]]
            co_await system::this_coroutine::yield();

        if constexpr (p == Proto::UDP)
        {
            int retries = 0;
            for (;;)
            {
                if (this->header_->is_write_armed()) this->header_->disarm_write();
                ssize_t res = detail::udp_send(this->header_->fd, this->address, buf, sz, MSG_DONTWAIT);
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
                    if (system::this_coroutine::cancel_requested()) [[unlikely]]
                    {
                        errno = ECANCELED;
                        co_return -1;
                    }
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
                if (this->header_->is_write_armed()) this->header_->disarm_write();
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
                    if (system::this_coroutine::cancel_requested()) [[unlikely]]
                    {
                        errno = ECANCELED;
                        co_return -1;
                    }
                    continue;
                }
                co_return -1;
            }
            co_return total_written;
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
        ssize_t total_written = 0;
        int retries = 0;

        while (total_written < static_cast<ssize_t>(sz))
        {
            ssize_t res;
            if constexpr (p == Proto::UDP)
                res = detail::udp_send(this->header_->fd, this->address, buf + total_written, sz - total_written,
                                       MSG_DONTWAIT | MSG_NOSIGNAL);
            else
                res = ::send(this->header_->fd, buf + total_written, sz - total_written, MSG_DONTWAIT | MSG_NOSIGNAL);
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
        addrinfo hints{};
        hints.ai_family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        auto resolved = co_await async_resolve(host, port, hints);
        if (!resolved || !*resolved)
        {
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::GetAddrInfoFailed;
        }
        AddrInfoPtr res_holder = std::move(*resolved);
        addrinfo* res = res_holder.get();

        this->header_->fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (this->header_->fd < 0)
        {
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
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);

        co_await detail::AwaiterWrite{this->header_};

        if (system::this_coroutine::cancel_requested()) [[unlikely]]
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::Cancelled;
        }

        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::TIMEOUT))
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::Timeout;
        }
        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::CONNECTION_FAILED))
        {
            const int cerr = std::exchange(this->header_->connect_error, 0);
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return cerr == ETIMEDOUT ? usub::utils::errors::ConnectError::Timeout
                                        : usub::utils::errors::ConnectError::ConnectFailed;
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

#ifdef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::wh.cancelTimer(this->header_->timer_id);
#else
        // synchronous: the Timer is embedded in the header and may be re-armed right
        // after connect (set_timeout_ms) — a queued REMOVE would race with that ADD
        system::this_thread::detail::wh.cancelTimerSync(this->header_->timer_id);
#endif
        this->header_->timer_id = 0;

        if (connect_timeout.count() > 0)
            this->header_->state.fetch_sub(1, std::memory_order_acq_rel);
        this->header_->timeout_epoch_bump();

        co_return std::nullopt;
    }

    template <Proto p, Role r>
    task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                    uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
    Socket<p, r>::async_connect(std::string&& host, std::string&& port, std::chrono::milliseconds connect_timeout)
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        addrinfo hints{};
        hints.ai_family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        auto resolved = co_await async_resolve(host, port, hints);
        if (!resolved || !*resolved)
        {
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::GetAddrInfoFailed;
        }
        AddrInfoPtr res_holder = std::move(*resolved);
        addrinfo* res = res_holder.get();

        this->header_->fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (this->header_->fd < 0)
        {
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
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);

        co_await detail::AwaiterWrite{this->header_};

        if (system::this_coroutine::cancel_requested()) [[unlikely]]
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::Cancelled;
        }

        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::TIMEOUT))
        {
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return usub::utils::errors::ConnectError::Timeout;
        }
        if (this->header_->socket_info & static_cast<uint8_t>(AdditionalState::CONNECTION_FAILED))
        {
            const int cerr = std::exchange(this->header_->connect_error, 0);
            ::close(this->header_->fd);
            this->header_->fd = -1;
            co_return cerr == ETIMEDOUT ? usub::utils::errors::ConnectError::Timeout
                                        : usub::utils::errors::ConnectError::ConnectFailed;
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

#ifdef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::wh.cancelTimer(this->header_->timer_id);
#else
        // synchronous: the Timer is embedded in the header and may be re-armed right
        // after connect (set_timeout_ms) — a queued REMOVE would race with that ADD
        system::this_thread::detail::wh.cancelTimerSync(this->header_->timer_id);
#endif
        this->header_->timer_id = 0;

        if (connect_timeout.count() > 0)
            this->header_->state.fetch_sub(1, std::memory_order_acq_rel);
        this->header_->timeout_epoch_bump();

        co_return std::nullopt;
    }

    template <Proto p, Role r>
    task::Awaitable<std::expected<size_t, usub::utils::errors::SendError>,
                    uvent::detail::AwaitableIOFrame<std::expected<size_t, usub::utils::errors::SendError>>>
    Socket<p, r>::async_send(uint8_t* buf, size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
        ssize_t total_written = 0;
        int retries = 0;

        while (total_written < static_cast<ssize_t>(sz))
        {
            for (;;)
            {
                if (this->header_->is_write_armed()) this->header_->disarm_write();
                if (this->is_disconnected_now())
                    co_return std::unexpected(usub::utils::errors::SendError::Closed);

                ssize_t res = 0;

                if constexpr (p == Proto::TCP)
                {
                    res = ::send(this->header_->fd, buf + total_written, sz - static_cast<size_t>(total_written),
                                 MSG_DONTWAIT | MSG_NOSIGNAL);
                }
                else
                {
                    try
                    {
                        if (std::holds_alternative<sockaddr_in>(this->address))
                        {
                            auto& addr = std::get<sockaddr_in>(this->address);
                            socklen_t addr_len = sizeof(sockaddr_in);
                            res = ::sendto(this->header_->fd, buf + total_written,
                                           sz - static_cast<size_t>(total_written), MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr*>(&addr), addr_len);
                        }
                        else if (std::holds_alternative<sockaddr_in6>(this->address))
                        {
                            auto& addr = std::get<sockaddr_in6>(this->address);
                            socklen_t addr_len = sizeof(sockaddr_in6);
                            res = ::sendto(this->header_->fd, buf + total_written,
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
            if (system::this_coroutine::cancel_requested()) [[unlikely]]
            {
                this->remove();
                co_return std::unexpected(usub::utils::errors::SendError::Cancelled);
            }
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
        if (system::this_coroutine::cancel_requested()) [[unlikely]]
        {
            errno = ECANCELED;
            co_return -1;
        }
        if (this->is_disconnected_now())
            co_return -3;

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
#ifdef UVENT_ENABLE_REUSEADDR
        if (detail::is_foreign_owner(this->header_))
        {
            // The timer lives in the owner's wheel; a local updateTimer would be a
            // silent no-op there and the connection would time out on the stale deadline.
            if (this->header_->timer_id != 0)
                detail::forward_socket_op(thread::SocketOp::Kind::Timeout, this->header_,
                                          static_cast<uint64_t>(new_duration));
            return;
        }
#endif
        system::this_thread::detail::wh.updateTimer(this->header_->timer_id, new_duration);
    }

    template <Proto p, Role r>
    void Socket<p, r>::shutdown()
    {
#ifdef UVENT_ENABLE_REUSEADDR
        if constexpr (p == Proto::TCP && r == Role::ACTIVE)
        {
            if (detail::is_foreign_owner(this->header_))
            {
                // Timer cancel and shutdown(2) both run on the owner (its wheel, its
                // fd — a stale fd number here could belong to another socket by now).
                // The timer's reference is dropped here iff nobody (timeout callback
                // on the owner) claimed it first.
                const bool had_timer = this->header_->timer_id != 0;
                detail::forward_socket_op(thread::SocketOp::Kind::Shutdown, this->header_);
                if (had_timer &&
                    (this->header_->tflags.fetch_or(detail::tflags::TIMER_CLAIMED, std::memory_order_acq_rel) &
                     detail::tflags::TIMER_CLAIMED) == 0)
                {
                    this->release(); // may destroy() -> forwarded to the owner as well
                }
                return;
            }
            if (this->header_->timer_id != 0)
            {
                if (system::this_thread::detail::wh.cancelTimer(this->header_->timer_id))
                {
                    this->header_->timer_id = 0;
                    if ((this->header_->tflags.fetch_or(detail::tflags::TIMER_CLAIMED, std::memory_order_acq_rel) &
                         detail::tflags::TIMER_CLAIMED) == 0)
                    {
                        ::shutdown(this->header_->fd, SHUT_RDWR);
                        this->release();
                        return;
                    }
                }
            }
        }
#endif
        ::shutdown(this->header_->fd, SHUT_RDWR);
    }

    template <Proto p, Role r>
    bool Socket<p, r>::set_nodelay(bool enable) const noexcept
        requires(p == Proto::TCP)
    {
        if (!this->header_ || this->header_->fd == INVALID_FD)
            return false;
        const int one = enable ? 1 : 0;
        return ::setsockopt(this->header_->fd, IPPROTO_TCP, TCP_NODELAY, &one, static_cast<socklen_t>(sizeof(one))) ==
            0;
    }

    template <Proto p, Role r>
    void Socket<p, r>::set_timeout_ms(timeout_t timeout) const
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
        if (this->header_->timer_id != 0)
        {
#ifdef UVENT_ENABLE_REUSEADDR
            if (detail::is_foreign_owner(this->header_))
            {
                detail::forward_socket_op(thread::SocketOp::Kind::Timeout, this->header_,
                                          static_cast<uint64_t>(timeout));
                return;
            }
#endif
            system::this_thread::detail::wh.updateTimer(this->header_->timer_id, timeout);
            return;
        }
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
#if UVENT_DEBUG
        spdlog::debug("set_timeout_ms: {}", this->header_->get_counter());
#endif
#ifdef UVENT_ENABLE_REUSEADDR
        // fresh timer: its reference is unclaimed
        this->header_->tflags.fetch_and(static_cast<uint8_t>(~detail::tflags::TIMER_CLAIMED),
                                        std::memory_order_acq_rel);
        if (detail::is_foreign_owner(this->header_))
        {
            detail::forward_socket_op(thread::SocketOp::Kind::Timeout, this->header_, static_cast<uint64_t>(timeout));
            return;
        }
        detail::arm_or_refresh_socket_timer(this->header_, static_cast<uint64_t>(timeout));
#else
        auto* timer = &this->header_->timer;
        timer->arm_embedded(
            timeout,
            [](void* hp)
            {
                std::any a{static_cast<SocketHeader*>(hp)};
                detail::processSocketTimeout(a);
            },
            this->header_);
        this->header_->timer_id = system::this_thread::detail::wh.addTimer(timer);
#endif
    }

    template <Proto p, Role r>
    void Socket<p, r>::destroy() noexcept
    {
#ifdef UVENT_ENABLE_REUSEADDR
        if (detail::is_foreign_owner(this->header_))
        {
            // Last reference dropped on a foreign worker: the owner's wheel may still
            // hold &header_->timer and the fd is registered in the owner's epoll, so
            // cancel/removeEvent/delete have to run there. Nobody else references the
            // header any more (refcount == 0), only the owner's timeout callback could
            // race — TEARDOWN_PENDING makes it a no-op.
            this->header_->tflags.fetch_or(detail::tflags::TEARDOWN_PENDING, std::memory_order_acq_rel);
            detail::forward_socket_op(thread::SocketOp::Kind::Destroy, this->header_);
            return;
        }
        detail::teardown_socket_header_local(this->header_);
#else
        this->header_->close_for_new_refs();
        system::this_thread::detail::pl.removeEvent(this->header_);
        if (this->header_->timer_id != 0)
        {
            // The Timer is embedded in the header and REMOVE is asynchronous (drained by
            // whichever thread ticks the shared wheel next): retire the header only once
            // the wheel has dropped the node, otherwise tick() reads freed memory.
            const uint64_t id = this->header_->timer_id;
            this->header_->timer_id = 0;
            system::this_thread::detail::wh.removeTimer(id, &detail::retire_header_after_timer, this->header_);
        }
        else
            system::this_thread::detail::g_qsbr.retire(static_cast<void*>(this->header_), &delete_header);
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
            std::string buffer(chunk_size, '\0');
            while (totalReceive < maxSize)
            {
                const size_t want = std::min(chunk_size, maxSize - totalReceive);
                ssize_t received = recv_fn(buffer.data(), want);
                if (received < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    return std::unexpected(usub::utils::errors::SendError::RecvFailed);
                }
                if (received == 0)
                    break;
                totalReceive += static_cast<size_t>(received);
                result.append(buffer.data(), static_cast<size_t>(received));
                if (static_cast<size_t>(received) < want)
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
