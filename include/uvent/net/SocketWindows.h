//
// Created by kirill on 11/27/25.
//

#ifndef SOCKETWINDOWS_H
#define SOCKETWINDOWS_H

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <expected>
#include <memory>

#include <uvent/poll/IocpPoller.h>
#include "AwaiterOperations.h"
#include "SocketMetadata.h"
#include "uvent/base/Predefines.h"
#include "uvent/system/Defines.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/Awaitable.h"
#include "uvent/utils/buffer/DynamicBuffer.h"
#include "uvent/utils/errors/IOErrors.h"
#include "uvent/utils/net/net.h"
#include "uvent/utils/net/socket.h"

namespace usub::uvent::net
{
    enum class IocpOp : uint8_t
    {
        READ,
        WRITE,
        ACCEPT,
        CONNECT
    };

    struct IocpOverlapped
    {
        OVERLAPPED ov{};
        SocketHeader* header{};
        IocpOp op{};
        DWORD bytes_transferred{};
    };

    namespace detail
    {
        extern void processSocketTimeout(std::any arg);

        inline LPFN_CONNECTEX get_connect_ex(SOCKET s)
        {
            GUID guid = WSAID_CONNECTEX;
            LPFN_CONNECTEX fn = nullptr;
            DWORD bytes = 0;

            int rc = ::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn, sizeof(fn), &bytes,
                                nullptr, nullptr);

            if (rc == SOCKET_ERROR)
            {
#if UVENT_DEBUG
                spdlog::error("get_connect_ex(win): WSAIoctl failed, err={}", WSAGetLastError());
#endif
                return nullptr;
            }

#if UVENT_DEBUG
            spdlog::debug("get_connect_ex(win): got ConnectEx ptr={:p}", (void*)fn);
#endif
            return fn;
        }

        inline LPFN_ACCEPTEX get_accept_ex(SOCKET s)
        {
            GUID guid = WSAID_ACCEPTEX;
            LPFN_ACCEPTEX fn = nullptr;
            DWORD bytes = 0;

            int rc = ::WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn, sizeof(fn), &bytes,
                                nullptr, nullptr);

#if UVENT_DEBUG
            if (rc == SOCKET_ERROR)
            {
                spdlog::error("get_accept_ex(win): WSAIoctl failed, err={}", WSAGetLastError());
            }
            else
            {
                spdlog::debug("get_accept_ex(win): got AcceptEx ptr={:p}", (void*)fn);
            }
#endif
            return (rc == SOCKET_ERROR) ? nullptr : fn;
        }
    } // namespace detail

    template <Proto p, Role r>
    class Socket : usub::utils::sync::refc::RefCounted<Socket<p, r>>
    {
    public:
        friend class usub::utils::sync::refc::RefCounted<Socket<p, r>>;
        friend class core::PollerBase;

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
        explicit Socket(socket_fd_t fd) noexcept;

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
         * \brief Asynchronously accepts one connection and invokes \p on_accept.
         *
         * \tparam F    Callable with signature \c void(TCPClientSocket, Args...).
         * \tparam Args Types of additional arguments forwarded to \p on_accept.
         *
         * \param on_accept Invoked with the accepted socket after each IOCP completion.
         * \param args      Zero or more additional arguments forwarded to \p on_accept.
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
        wsa_init_once();
        this->header_ =
            new SocketHeader{.fd = INVALID_FD,
                             .socket_info = (static_cast<uint8_t>(Proto::TCP) | static_cast<uint8_t>(Role::ACTIVE) |
                                             static_cast<uint8_t>(AdditionalState::CONNECTION_PENDING)),
#ifndef UVENT_ENABLE_REUSEADDR
                             .state = std::atomic<uint64_t>(1 & usub::utils::sync::refc::COUNT_MASK)
#else
                             .state = (1 & usub::utils::sync::refc::COUNT_MASK)
#endif
            };
#if UVENT_DEBUG
        spdlog::debug("Socket() default ctor(win): header={}, fd={}", static_cast<void*>(this->header_),
                      static_cast<std::uint64_t>(this->header_->fd));
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(socket_fd_t fd) noexcept
    {
        wsa_init_once();
        this->header_ =
            new SocketHeader{.fd = fd,
                             .socket_info = (static_cast<uint8_t>(Proto::TCP) | static_cast<uint8_t>(Role::ACTIVE) |
                                             static_cast<uint8_t>(AdditionalState::CONNECTION_PENDING)),
#ifndef UVENT_ENABLE_REUSEADDR
                             .state = std::atomic<uint64_t>(1 & usub::utils::sync::refc::COUNT_MASK)
#else
                             .state = (1 & usub::utils::sync::refc::COUNT_MASK)
#endif
            };
#if UVENT_DEBUG
        spdlog::debug("Socket(fd) ctor(win): header={}, fd={}", static_cast<void*>(this->header_),
                      static_cast<std::uint64_t>(this->header_->fd));
#endif
        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);
#if UVENT_DEBUG
        spdlog::debug("Socket(fd) ctor(win): addEvent(ALL) done fd={}", static_cast<std::uint64_t>(this->header_->fd));
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(std::string& ip_addr, int port, int backlog, utils::net::IPV ipv_,
                         utils::net::SocketAddressType socketAddressType) noexcept
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        wsa_init_once();
        this->header_ =
            new SocketHeader{.fd = utils::socket::createSocket(port, ip_addr, backlog, ipv_, socketAddressType),
                             .socket_info = (uint8_t(p) | uint8_t(r)),
#ifndef UVENT_ENABLE_REUSEADDR
                             .state = std::atomic<uint64_t>((1ull & usub::utils::sync::refc::COUNT_MASK))
#else
                             .state = (1ull & usub::utils::sync::refc::COUNT_MASK)
#endif
            };

        u_long mode = 1;
        ::ioctlsocket(this->header_->fd, FIONBIO, &mode);
#if UVENT_DEBUG
        spdlog::info("Socket(passive) ctor(win): listen fd={} ip={} port={}",
                     static_cast<std::uint64_t>(this->header_->fd), ip_addr, port);
#endif

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::READ);
#if UVENT_DEBUG
        spdlog::debug("Socket(passive) ctor(win): addEvent(READ) done fd={}",
                      static_cast<std::uint64_t>(this->header_->fd));
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(std::string&& ip_addr, int port, int backlog, utils::net::IPV ipv_,
                         utils::net::SocketAddressType socketAddressType) noexcept
        requires(p == Proto::TCP && r == Role::PASSIVE)
        : Socket(ip_addr, port, backlog, ipv_, socketAddressType)
    {
#if UVENT_DEBUG
        spdlog::debug("Socket(passive&&) ctor(win): fd={}", static_cast<std::uint64_t>(this->header_->fd));
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(SocketHeader* header) noexcept : header_(header)
    {
#if UVENT_DEBUG
        spdlog::debug("Socket(from_existing header) ctor(win): header={}, fd={}", static_cast<void*>(this->header_),
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::~Socket()
    {
        if (this->header_)
        {
#if UVENT_DEBUG
            spdlog::debug("~Socket(win) begin: header={}, fd={}", static_cast<void*>(this->header_),
                          static_cast<std::uint64_t>(this->header_->fd));
#endif
            this->release();
#if UVENT_DEBUG
            spdlog::warn("Socket counter(after release): {}, fd: {}",
                         (this->header_->state & usub::utils::sync::refc::COUNT_MASK),
                         (std::uint64_t)this->header_->fd);
#endif
        }
#if UVENT_DEBUG
        else
        {
            spdlog::debug("~Socket(win): header_ is nullptr");
        }
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(const Socket& o) noexcept : header_(o.header_)
    {
        if (this->header_)
            this->add_ref();
#if UVENT_DEBUG
        spdlog::debug("Socket copy-ctor(win): header={}, fd={}, refcnt={}", static_cast<void*>(this->header_),
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull,
                      this->header_ ? this->header_->get_counter() : 0);
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>::Socket(Socket&& o) noexcept : header_(o.header_)
    {
        o.header_ = nullptr;
#if UVENT_DEBUG
        spdlog::debug("Socket move-ctor(win): new header={}, fd={}", static_cast<void*>(this->header_),
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
    }

    template <Proto p, Role r>
    Socket<p, r>& Socket<p, r>::operator=(const Socket& o) noexcept
    {
        if (this == &o)
            return *this;
        Socket tmp(o);
        std::swap(this->header_, tmp.header_);
#if UVENT_DEBUG
        spdlog::debug("Socket copy-assign(win): header={}, fd={}", static_cast<void*>(this->header_),
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
        return *this;
    }

    template <Proto p, Role r>
    Socket<p, r>& Socket<p, r>::operator=(Socket&& o) noexcept
    {
        if (this == &o)
            return *this;
        Socket tmp(std::move(o));
        std::swap(this->header_, tmp.header_);
#if UVENT_DEBUG
        spdlog::debug("Socket move-assign(win): header={}, fd={}", static_cast<void*>(this->header_),
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
        return *this;
    }

    template <Proto p, Role r>
    Socket<p, r> Socket<p, r>::from_existing(SocketHeader* header)
    {
#if UVENT_DEBUG
        spdlog::debug("Socket::from_existing(win): header={}, fd={}", static_cast<void*>(header),
                      header ? static_cast<std::uint64_t>(header->fd) : 0ull);
#endif
        return Socket(header);
    }

    template <Proto p, Role r>
    SocketHeader* Socket<p, r>::get_raw_header()
    {
#if UVENT_DEBUG
        spdlog::trace("Socket::get_raw_header(win): header={}, fd={}", static_cast<void*>(this->header_),
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
        return this->header_;
    }

    template <Proto p, Role r>
    [[nodiscard]] task::Awaitable<std::optional<TCPClientSocket>,
                                  uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>
    Socket<p, r>::async_accept()
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        using Awaitable = task::Awaitable<std::optional<TCPClientSocket>,
                                          uvent::detail::AwaitableIOFrame<std::optional<TCPClientSocket>>>;

        SocketHeader* header = this->header_;
        if (!header || header->fd == INVALID_FD)
        {
#if UVENT_ERROR
            spdlog::error("async_accept(win): invalid listen header/fd");
#endif
            co_return std::nullopt;
        }

        SOCKET listen_s = static_cast<SOCKET>(header->fd);

#if UVENT_DEBUG
        spdlog::info("async_accept(win): enter, listen fd={}", static_cast<socket_fd_t>(listen_s));
#endif

        LPFN_ACCEPTEX accept_ex = detail::get_accept_ex(listen_s);
        if (!accept_ex)
        {
#if UVENT_ERROR
            spdlog::error("async_accept(win): get_accept_ex failed for listen fd={}",
                          static_cast<socket_fd_t>(listen_s));
#endif
            co_return std::nullopt;
        }

        int family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;

        SOCKET client_s = ::WSASocket(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

        if (client_s == INVALID_SOCKET)
        {
#if UVENT_ERROR
            spdlog::error("async_accept(win): WSASocket(accept) failed, err={}", WSAGetLastError());
#endif
            co_return std::nullopt;
        }

        DWORD addr_len = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
        DWORD buf_len = addr_len * 2;
        auto addr_buf = std::make_unique<char[]>(buf_len);

        auto ov = std::make_unique<IocpOverlapped>();
        std::memset(&ov->ov, 0, sizeof(ov->ov));
        ov->header = header;
        ov->op = IocpOp::ACCEPT;
        ov->bytes_transferred = 0;

        DWORD bytes = 0;

#if UVENT_DEBUG
        spdlog::trace("async_accept(win): posting AcceptEx listen_fd={} accept_fd={}",
                      static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s));
#endif

        BOOL ok = accept_ex(listen_s, client_s, addr_buf.get(), 0, addr_len, addr_len, &bytes, &ov->ov);

        if (!ok)
        {
            int err = ::WSAGetLastError();
            if (err != ERROR_IO_PENDING)
            {
#if UVENT_ERROR
                spdlog::error("async_accept(win): AcceptEx failed immediately, "
                              "listen_fd={}, accept_fd={}, err={}",
                              static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s), err);
#endif
                ::closesocket(client_s);
                co_return std::nullopt;
            }

#if UVENT_TRACE
            spdlog::trace("async_accept(win): AcceptEx pending, listen_fd={}, accept_fd={}",
                          static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s));
#endif

            co_await detail::AwaiterRead{header};

            DWORD flags = 0;
            if (!::WSAGetOverlappedResult(listen_s, &ov->ov, &bytes, FALSE, &flags))
            {
                int err2 = WSAGetLastError();
#if UVENT_ERROR
                spdlog::error("async_accept(win): WSAGetOverlappedResult failed, "
                              "listen_fd={}, accept_fd={}, err={}",
                              static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s), err2);
#endif
                ::closesocket(client_s);
                co_return std::nullopt;
            }
        }
        else
        {
#if UVENT_TRACE
            spdlog::trace("async_accept(win): AcceptEx completed synchronously, "
                          "listen_fd={}, accept_fd={}, bytes={}",
                          static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s), bytes);
#endif
        }

        if (::setsockopt(client_s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&listen_s),
                         sizeof(listen_s)) == SOCKET_ERROR)
        {
#if UVENT_ERROR
            spdlog::error("async_accept(win): SO_UPDATE_ACCEPT_CONTEXT failed, "
                          "accept_fd={}, err={}",
                          static_cast<socket_fd_t>(client_s), WSAGetLastError());
#endif
            ::closesocket(client_s);
            co_return std::nullopt;
        }

        {
            u_long mode = 1;
            if (::ioctlsocket(client_s, FIONBIO, &mode) == SOCKET_ERROR)
            {
#if UVENT_ERROR
                spdlog::error("async_accept(win): ioctlsocket(FIONBIO) failed, "
                              "accept_fd={}, err={}",
                              static_cast<socket_fd_t>(client_s), WSAGetLastError());
#endif
                ::closesocket(client_s);
                co_return std::nullopt;
            }
        }

        sockaddr* local_sa = nullptr;
        sockaddr* remote_sa = nullptr;
        int local_len = 0;
        int remote_len = 0;

        ::GetAcceptExSockaddrs(addr_buf.get(), 0, addr_len, addr_len, &local_sa, &local_len, &remote_sa, &remote_len);

        TCPClientSocket client{static_cast<socket_fd_t>(client_s)};

        if (remote_sa)
        {
            if (remote_sa->sa_family == AF_INET)
            {
                sockaddr_in sa{};
                std::memcpy(&sa, remote_sa, sizeof(sockaddr_in));
                client.address = sa;
                client.ipv = utils::net::IPV::IPV4;
            }
            else if (remote_sa->sa_family == AF_INET6)
            {
                sockaddr_in6 sa6{};
                std::memcpy(&sa6, remote_sa, sizeof(sockaddr_in6));
                client.address = sa6;
                client.ipv = utils::net::IPV::IPV6;
            }
        }

#if UVENT_DEBUG
        if (auto* ch = client.get_raw_header())
        {
            ch->socket_info &= ~static_cast<uint8_t>(AdditionalState::CONNECTION_PENDING);

            spdlog::info("async_accept(win): accepted client_fd={} on listen_fd={}", static_cast<socket_fd_t>(client_s),
                         static_cast<socket_fd_t>(listen_s));
            spdlog::debug("async_accept(win): client header={} refcnt={}", static_cast<void*>(ch), ch->get_counter());
        }
#endif

        co_return std::move(client);
    }

    template <Proto p, Role r>
    template <typename F, typename... Args>
        requires std::invocable<F, TCPClientSocket, Args...>
    task::Awaitable<void> Socket<p, r>::async_accept(F on_accept, Args&&... args)
        requires(p == Proto::TCP && r == Role::PASSIVE)
    {
        SocketHeader* header = this->header_;

        for (;;)
        {
            if (!header || header->fd == INVALID_FD)
            {
#if UVENT_ERROR
                spdlog::error("async_accept(win): invalid listen header/fd");
#endif
                co_return;
            }

            SOCKET listen_s = static_cast<SOCKET>(header->fd);

#if UVENT_DEBUG
            spdlog::info("async_accept(win): enter, listen fd={}", static_cast<socket_fd_t>(listen_s));
#endif

            LPFN_ACCEPTEX accept_ex = detail::get_accept_ex(listen_s);
            if (!accept_ex)
            {
#if UVENT_ERROR
                spdlog::error("async_accept(win): get_accept_ex failed for listen fd={}",
                              static_cast<socket_fd_t>(listen_s));
#endif
                co_return;
            }

            int family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;

            SOCKET client_s = ::WSASocket(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

            if (client_s == INVALID_SOCKET)
            {
#if UVENT_ERROR
                spdlog::error("async_accept(win): WSASocket(accept) failed, err={}", WSAGetLastError());
#endif
                co_return;
            }

            DWORD addr_len = static_cast<DWORD>(sizeof(sockaddr_storage) + 16);
            DWORD buf_len = addr_len * 2;
            auto addr_buf = std::make_unique<char[]>(buf_len);

            auto ov = std::make_unique<IocpOverlapped>();
            std::memset(&ov->ov, 0, sizeof(ov->ov));
            ov->header = header;
            ov->op = IocpOp::ACCEPT;
            ov->bytes_transferred = 0;

            DWORD bytes = 0;

#if UVENT_DEBUG
            spdlog::trace("async_accept(win): posting AcceptEx listen_fd={} accept_fd={}",
                          static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s));
#endif

            BOOL ok = accept_ex(listen_s, client_s, addr_buf.get(), 0, addr_len, addr_len, &bytes, &ov->ov);

            if (!ok)
            {
                int err = ::WSAGetLastError();
                if (err != ERROR_IO_PENDING)
                {
#if UVENT_ERROR
                    spdlog::error("async_accept(win): AcceptEx failed immediately, "
                                  "listen_fd={}, accept_fd={}, err={}",
                                  static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s), err);
#endif
                    ::closesocket(client_s);
                    co_return;
                }

#if UVENT_TRACE
                spdlog::trace("async_accept(win): AcceptEx pending, listen_fd={}, accept_fd={}",
                              static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s));
#endif

                co_await detail::AwaiterRead{header};

                DWORD flags = 0;
                if (!::WSAGetOverlappedResult(listen_s, &ov->ov, &bytes, FALSE, &flags))
                {
                    int err2 = WSAGetLastError();
#if UVENT_ERROR
                    spdlog::error("async_accept(win): WSAGetOverlappedResult failed, "
                                  "listen_fd={}, accept_fd={}, err={}",
                                  static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s), err2);
#endif
                    ::closesocket(client_s);
                    co_return;
                }
            }
            else
            {
#if UVENT_TRACE
                spdlog::trace("async_accept(win): AcceptEx completed synchronously, "
                              "listen_fd={}, accept_fd={}, bytes={}",
                              static_cast<socket_fd_t>(listen_s), static_cast<socket_fd_t>(client_s), bytes);
#endif
            }

            if (::setsockopt(client_s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<char*>(&listen_s),
                             sizeof(listen_s)) == SOCKET_ERROR)
            {
#if UVENT_ERROR
                spdlog::error("async_accept(win): SO_UPDATE_ACCEPT_CONTEXT failed, "
                              "accept_fd={}, err={}",
                              static_cast<socket_fd_t>(client_s), WSAGetLastError());
#endif
                ::closesocket(client_s);
                co_return;
            }

            {
                u_long mode = 1;
                if (::ioctlsocket(client_s, FIONBIO, &mode) == SOCKET_ERROR)
                {
#if UVENT_ERROR
                    spdlog::error("async_accept(win): ioctlsocket(FIONBIO) failed, "
                                  "accept_fd={}, err={}",
                                  static_cast<socket_fd_t>(client_s), WSAGetLastError());
#endif
                    ::closesocket(client_s);
                    co_return;
                }
            }

            sockaddr* local_sa = nullptr;
            sockaddr* remote_sa = nullptr;
            int local_len = 0;
            int remote_len = 0;

            ::GetAcceptExSockaddrs(addr_buf.get(), 0, addr_len, addr_len, &local_sa, &local_len, &remote_sa,
                                   &remote_len);

            TCPClientSocket client{static_cast<socket_fd_t>(client_s)};

            if (remote_sa)
            {
                if (remote_sa->sa_family == AF_INET)
                {
                    sockaddr_in sa{};
                    std::memcpy(&sa, remote_sa, sizeof(sockaddr_in));
                    client.address = sa;
                    client.ipv = utils::net::IPV::IPV4;
                }
                else if (remote_sa->sa_family == AF_INET6)
                {
                    sockaddr_in6 sa6{};
                    std::memcpy(&sa6, remote_sa, sizeof(sockaddr_in6));
                    client.address = sa6;
                    client.ipv = utils::net::IPV::IPV6;
                }
            }

#if UVENT_DEBUG
            if (auto* ch = client.get_raw_header())
            {
                ch->socket_info &= ~static_cast<uint8_t>(AdditionalState::CONNECTION_PENDING);

                spdlog::info("async_accept(win): accepted client_fd={} on listen_fd={}",
                             static_cast<socket_fd_t>(client_s), static_cast<socket_fd_t>(listen_s));
                spdlog::debug("async_accept(win): client header={} refcnt={}", static_cast<void*>(ch),
                              ch->get_counter());
            }
#endif

            if constexpr (std::is_void_v<std::invoke_result_t<F, TCPClientSocket, Args...>>)
                on_accept(std::move(client), std::forward<Args>(args)...);
            else
                system::co_spawn(on_accept(std::move(client), std::forward<Args>(args)...));
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>>
    Socket<p, r>::async_read(utils::DynamicBuffer& buffer, size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_read(buffer)(win): fd={}, cur_buf_size={}, max_read={}", (std::uint64_t)this->header_->fd,
                     buffer.size(), max_read_size);
#endif
        if constexpr (p == Proto::UDP)
        {
            int retries = 0;
            ssize_t total_read = 0;

            while (true)
            {
                uint8_t temp[16384];
                size_t remaining = max_read_size - buffer.size();
                if (remaining == 0)
                    break;

                size_t to_read = (std::min)(sizeof(temp), remaining);

                int res = ::recv(this->header_->fd, reinterpret_cast<char*>(temp), static_cast<int>(to_read), 0);

#if UVENT_DEBUG
                spdlog::trace("async_read(udp)(win): recv res={} fd={}", res, (std::uint64_t)this->header_->fd);
#endif

                if (res > 0)
                {
                    buffer.append(temp, res);
                    total_read += res;
                    retries = 0;
                }
                else if (res == 0)
                {
#if UVENT_DEBUG
                    spdlog::info("async_read(udp)(win): peer closed fd={}", (std::uint64_t)this->header_->fd);
#endif
                    this->remove();
                    co_return total_read > 0 ? total_read : 0;
                }
                else
                {
                    int err = WSAGetLastError();
#if UVENT_DEBUG
                    spdlog::debug("async_read(udp)(win): recv error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                    if (err == WSAEWOULDBLOCK)
                    {
                        break;
                    }
                    else if (err == WSAEINTR)
                    {
                        if (++retries >= settings::max_read_retries)
                        {
                            this->remove();
                            co_return -1;
                        }
                        continue;
                    }
                    else
                    {
                        this->remove();
                        co_return -1;
                    }
                }

                if (buffer.size() >= max_read_size)
                    break;
            }
#ifndef UVENT_ENABLE_REUSEADDR
            if (total_read > 0)
                this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
            spdlog::info("async_read(udp)(win): total_read={} fd={}", total_read, (std::uint64_t)this->header_->fd);
#endif
            co_return total_read;
        }
        else
        {
            size_t remaining = max_read_size - buffer.size();
            if (remaining == 0)
            {
#if UVENT_DEBUG
                spdlog::trace("async_read(tcp)(win): no remaining space, return 0");
#endif
                co_return 0;
            }

#if UVENT_DEBUG
            spdlog::trace("async_read(tcp)(win): posting WSARecv, remaining={}", remaining);
#endif

            auto ov = std::make_unique<IocpOverlapped>();
            ov->header = this->header_;
            ov->op = IocpOp::READ;
            std::memset(&ov->ov, 0, sizeof(ov->ov));

            auto tmp = std::make_unique<uint8_t[]>(remaining);
            WSABUF wbuf;
            wbuf.buf = reinterpret_cast<char*>(tmp.get());
            wbuf.len = static_cast<ULONG>(remaining);

            DWORD flags = 0;
            DWORD bytes = 0;

            int rc = ::WSARecv(this->header_->fd, &wbuf, 1, &bytes, &flags, &ov->ov, nullptr);
#if UVENT_DEBUG
            spdlog::trace("async_read(tcp)(win): WSARecv rc={}, bytes={}, fd={}", rc, bytes,
                          (std::uint64_t)this->header_->fd);
#endif
            if (rc == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("async_read(tcp)(win): WSARecv error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                if (err != WSA_IO_PENDING)
                {
                    this->remove();
                    co_return -1;
                }
            }

            if (rc == 0)
            {
                if (bytes == 0)
                {
#if UVENT_DEBUG
                    spdlog::info("async_read(tcp)(win): immediate EOF fd={}", (std::uint64_t)this->header_->fd);
#endif
                    this->remove();
                    co_return 0;
                }
                buffer.append(tmp.get(), bytes);
#ifndef UVENT_ENABLE_REUSEADDR
                if (bytes > 0)
                    this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
                spdlog::info("async_read(tcp)(win): immediate completion bytes={} fd={}", bytes,
                             (std::uint64_t)this->header_->fd);
#endif
                co_return static_cast<ssize_t>(bytes);
            }

#if UVENT_DEBUG
            spdlog::trace("async_read(tcp)(win): waiting on AwaiterRead fd={}", (std::uint64_t)this->header_->fd);
#endif
            co_await detail::AwaiterRead{this->header_};

            bytes = ov->bytes_transferred;

#if UVENT_DEBUG
            spdlog::trace("async_read(tcp)(win): overlapped completed bytes={} fd={}", bytes,
                          this->header_ ? (std::uint64_t)this->header_->fd : 0ull);
#endif

            if (!this->header_ || this->header_->fd == INVALID_FD)
            {
#if UVENT_DEBUG
                spdlog::info("async_read(tcp)(win): header/fd invalid after completion");
#endif
                co_return -1;
            }

            if (bytes == 0)
            {
#if UVENT_DEBUG
                spdlog::info("async_read(tcp)(win): overlapped EOF fd={}", (std::uint64_t)this->header_->fd);
#endif
                this->remove();
                co_return 0;
            }

            buffer.append(tmp.get(), bytes);
#ifndef UVENT_ENABLE_REUSEADDR
            if (bytes > 0)
                this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
            spdlog::info("async_read(tcp)(win): return bytes={} fd={}", bytes, (std::uint64_t)this->header_->fd);
#endif
            co_return static_cast<ssize_t>(bytes);
        }
    }


    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_read(uint8_t* dst,
                                                                                                size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_read(raw)(win): fd={}, max_read={}", (std::uint64_t)this->header_->fd, max_read_size);
#endif
        if (!dst || max_read_size == 0)
        {
#if UVENT_DEBUG
            spdlog::trace("async_read(raw)(win): dst=null or max_read=0, return 0");
#endif
            co_return 0;
        }

        if constexpr (p == Proto::UDP)
        {
            ssize_t total_read = 0;
            int retries = 0;

            for (;;)
            {
                int res = ::recvfrom(this->header_->fd, reinterpret_cast<char*>(dst), static_cast<int>(max_read_size),
                                     0, nullptr, nullptr);

#if UVENT_DEBUG
                spdlog::trace("async_read(raw-udp)(win): recvfrom res={} fd={}", res, (std::uint64_t)this->header_->fd);
#endif

                if (res > 0)
                {
#ifndef UVENT_ENABLE_REUSEADDR
                    this->header_->timeout_epoch_bump();
#endif
                    co_return res;
                }

                if (res == 0)
                {
#if UVENT_DEBUG
                    spdlog::info("async_read(raw-udp)(win): recvfrom EOF fd={}", (std::uint64_t)this->header_->fd);
#endif
                    co_return 0;
                }

                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("async_read(raw-udp)(win): error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                if (err == WSAEINTR)
                {
                    if (++retries >= settings::max_read_retries)
                    {
                        this->remove();
                        co_return -1;
                    }
                    continue;
                }

                if (err == WSAEWOULDBLOCK)
                    co_return 0;

                this->remove();
                co_return -1;
            }
        }
        else
        {
#if UVENT_DEBUG
            spdlog::trace("async_read(raw-tcp)(win): posting WSARecv fd={}", (std::uint64_t)this->header_->fd);
#endif
            auto ov = std::make_unique<IocpOverlapped>();
            ov->header = this->header_;
            ov->op = IocpOp::READ;
            std::memset(&ov->ov, 0, sizeof(ov->ov));

            WSABUF wbuf;
            wbuf.buf = reinterpret_cast<char*>(dst);
            wbuf.len = static_cast<ULONG>(max_read_size);

            DWORD flags = 0;
            DWORD bytes = 0;

            int rc = ::WSARecv(this->header_->fd, &wbuf, 1, &bytes, &flags, &ov->ov, nullptr);
#if UVENT_DEBUG
            spdlog::trace("async_read(raw-tcp)(win): WSARecv rc={}, bytes={} fd={}", rc, bytes,
                          (std::uint64_t)this->header_->fd);
#endif
            if (rc == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("async_read(raw-tcp)(win): WSARecv error={} fd={}", err,
                              (std::uint64_t)this->header_->fd);
#endif
                if (err != WSA_IO_PENDING)
                {
                    this->remove();
                    co_return -1;
                }
            }

            if (rc == 0)
            {
                if (bytes == 0)
                {
#if UVENT_DEBUG
                    spdlog::info("async_read(raw-tcp)(win): immediate EOF fd={}", (std::uint64_t)this->header_->fd);
#endif
                    this->remove();
                    co_return 0;
                }
#ifndef UVENT_ENABLE_REUSEADDR
                if (bytes > 0)
                    this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
                spdlog::info("async_read(raw-tcp)(win): immediate bytes={} fd={}", bytes,
                             (std::uint64_t)this->header_->fd);
#endif
                co_return static_cast<ssize_t>(bytes);
            }

#if UVENT_DEBUG
            spdlog::trace("async_read(raw-tcp)(win): waiting AwaiterRead fd={}", (std::uint64_t)this->header_->fd);
#endif
            co_await detail::AwaiterRead{this->header_};

            bytes = ov->bytes_transferred;

#if UVENT_DEBUG
            spdlog::trace("async_read(raw-tcp)(win): overlapped completed bytes={} fd={}", bytes,
                          this->header_ ? (std::uint64_t)this->header_->fd : 0ull);
#endif

            if (!this->header_ || this->header_->fd == INVALID_FD)
            {
#if UVENT_DEBUG
                spdlog::info("async_read(raw-tcp)(win): header/fd invalid after completion");
#endif
                co_return -1;
            }

            if (bytes == 0)
            {
#if UVENT_DEBUG
                spdlog::info("async_read(raw-tcp)(win): overlapped EOF fd={}", (std::uint64_t)this->header_->fd);
#endif
                this->remove();
                co_return 0;
            }

#ifndef UVENT_ENABLE_REUSEADDR
            if (bytes > 0)
                this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
            spdlog::info("async_read(raw-tcp)(win): return bytes={} fd={}", bytes, (std::uint64_t)this->header_->fd);
#endif
            co_return static_cast<ssize_t>(bytes);
        }
    }

    template <Proto p, Role r>
    task::Awaitable<ssize_t, uvent::detail::AwaitableIOFrame<ssize_t>> Socket<p, r>::async_write(uint8_t* buf,
                                                                                                 size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_write(win): fd={}, sz={}", (std::uint64_t)this->header_->fd, sz);
#endif
        if (!buf || sz == 0)
        {
#if UVENT_DEBUG
            spdlog::trace("async_write(win): buf=null or sz=0, return 0");
#endif
            co_return 0;
        }

        if constexpr (p == Proto::UDP)
        {
            int retries = 0;
            for (;;)
            {
                int res = ::send(this->header_->fd, reinterpret_cast<const char*>(buf), static_cast<int>(sz), 0);
#if UVENT_DEBUG
                spdlog::trace("async_write(udp)(win): send res={} fd={}", res, (std::uint64_t)this->header_->fd);
#endif
                if (res >= 0)
                {
#ifndef UVENT_ENABLE_REUSEADDR
                    if (res > 0)
                        this->header_->timeout_epoch_bump();
#endif
                    co_return res;
                }
                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("async_write(udp)(win): send error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                if (err == WSAEINTR)
                {
                    if (++retries >= settings::max_write_retries)
                        co_return -1;
                    continue;
                }
                if (err == WSAEWOULDBLOCK)
                {
                    co_return 0;
                }
                co_return -1;
            }
        }
        else
        {
            ssize_t total_written = 0;
            auto ov = std::make_unique<IocpOverlapped>();
            ov->header = this->header_;
            ov->op = IocpOp::WRITE;
            std::memset(&ov->ov, 0, sizeof(ov->ov));

            while (total_written < static_cast<ssize_t>(sz))
            {
                std::memset(&ov->ov, 0, sizeof(ov->ov));

                WSABUF wbuf;
                wbuf.buf = reinterpret_cast<char*>(buf + total_written);
                wbuf.len = static_cast<ULONG>(sz - static_cast<size_t>(total_written));

                DWORD bytes = 0;
                int rc = ::WSASend(this->header_->fd, &wbuf, 1, &bytes, 0, &ov->ov, nullptr);
#if UVENT_DEBUG
                spdlog::trace("async_write(tcp)(win): WSASend rc={}, bytes={} fd={}", rc, bytes,
                              (std::uint64_t)this->header_->fd);
#endif
                if (rc == SOCKET_ERROR)
                {
                    int err = WSAGetLastError();
#if UVENT_DEBUG
                    spdlog::debug("async_write(tcp)(win): WSASend error={} fd={}", err,
                                  (std::uint64_t)this->header_->fd);
#endif
                    if (err != WSA_IO_PENDING)
                    {
                        co_return -1;
                    }
                }

                if (rc == 0)
                {
                    if (bytes == 0)
                    {
#if UVENT_DEBUG
                        spdlog::error("async_write(tcp)(win): WSASend bytes=0 fd={}", (std::uint64_t)this->header_->fd);
#endif
                        co_return -1;
                    }
                    total_written += bytes;
#ifndef UVENT_ENABLE_REUSEADDR
                    if (bytes > 0)
                        this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
                    spdlog::trace("async_write(tcp)(win): immediate bytes={}, total_written={} fd={}", bytes,
                                  total_written, (std::uint64_t)this->header_->fd);
#endif
                    continue;
                }

#if UVENT_DEBUG
                spdlog::trace("async_write(tcp)(win): waiting AwaiterWrite fd={}", (std::uint64_t)this->header_->fd);
#endif
                co_await detail::AwaiterWrite{this->header_};

                bytes = ov->bytes_transferred;

                if (!this->header_ || this->header_->fd == INVALID_FD)
                {
#if UVENT_DEBUG
                    spdlog::info("async_write(tcp)(win): header/fd invalid after completion");
#endif
                    co_return -1;
                }

                if (bytes == 0)
                {
#if UVENT_DEBUG
                    spdlog::error("async_write(tcp)(win): overlapped bytes=0 fd={}", (std::uint64_t)this->header_->fd);
#endif
                    co_return -1;
                }

                total_written += bytes;
#ifndef UVENT_ENABLE_REUSEADDR
                if (bytes > 0)
                    this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
                spdlog::trace("async_write(tcp)(win): overlapped bytes={}, total_written={} fd={}", bytes,
                              total_written, (std::uint64_t)this->header_->fd);
#endif
            }

#if UVENT_DEBUG
            spdlog::info("async_write(tcp)(win): done total_written={} fd={}", total_written,
                         (std::uint64_t)this->header_->fd);
#endif
            co_return total_written;
        }
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::read(utils::DynamicBuffer& buffer, size_t max_read_size)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("read(sync)(win): fd={}, cur_buf_size={}, max_read={}", (std::uint64_t)this->header_->fd,
                     buffer.size(), max_read_size);
#endif
        ssize_t total_read = 0;
        int retries = 0;

        while (true)
        {
            uint8_t temp[16384];
            size_t remaining = max_read_size - buffer.size();
            if (remaining == 0)
                break;

            size_t to_read = (std::min)(sizeof(temp), remaining);

            int res = ::recv(this->header_->fd, reinterpret_cast<char*>(temp), static_cast<int>(to_read), 0);

#if UVENT_DEBUG
            spdlog::trace("read(sync)(win): recv res={} fd={}", res, (std::uint64_t)this->header_->fd);
#endif

            if (res > 0)
            {
                buffer.append(temp, res);
                total_read += res;
                retries = 0;
            }
            else if (res == 0)
            {
#if UVENT_DEBUG
                spdlog::info("read(sync)(win): EOF fd={}", (std::uint64_t)this->header_->fd);
#endif
                return total_read > 0 ? total_read : 0;
            }
            else
            {
                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("read(sync)(win): error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                if (err == WSAEWOULDBLOCK)
                {
                    break;
                }
                else if (err == WSAEINTR)
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
#if UVENT_DEBUG
        spdlog::info("read(sync)(win): total_read={}, fd={}", total_read, (std::uint64_t)this->header_->fd);
#endif
        return total_read;
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::write(uint8_t* buf, size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("write(sync)(win): fd={}, sz={}", (std::uint64_t)this->header_->fd, sz);
#endif
        auto buf_internal = std::unique_ptr<uint8_t[]>(new uint8_t[sz], std::default_delete<uint8_t[]>());
        std::copy_n(buf, sz, buf_internal.get());

        ssize_t total_written = 0;
        int retries = 0;

        while (total_written < static_cast<ssize_t>(sz))
        {
            int res = ::send(this->header_->fd, reinterpret_cast<const char*>(buf_internal.get() + total_written),
                             static_cast<int>(sz - total_written), 0);
#if UVENT_DEBUG
            spdlog::trace("write(sync)(win): send res={} fd={}", res, (std::uint64_t)this->header_->fd);
#endif
            if (res > 0)
            {
                total_written += res;
                retries = 0;
                continue;
            }
            else if (res == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("write(sync)(win): send error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                if (err == WSAEINTR)
                {
                    if (++retries >= settings::max_write_retries)
                    {
                        return -1;
                    }
                    continue;
                }
                else if (err == WSAEWOULDBLOCK)
                {
                    break;
                }
                else
                {
                    return -1;
                }
            }
        }
#if UVENT_DEBUG
        spdlog::info("write(sync)(win): total_written={}, fd={}", total_written, (std::uint64_t)this->header_->fd);
#endif
        return total_written;
    }

    template <Proto p, Role r>
    [[nodiscard]] task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                                  uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
    Socket<p, r>::async_connect(std::string& host, std::string& port, std::chrono::milliseconds connect_timeout)
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
#if UVENT_DEBUG
        spdlog::info("async_connect(win,lvalue): host='{}' port='{}'", host, port);
#endif

        addrinfo hints{}, *res = nullptr;
        hints.ai_family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,lvalue): getaddrinfo failed");
#endif
            this->header_->fd = INVALID_FD;
            co_return usub::utils::errors::ConnectError::GetAddrInfoFailed;
        }

        SOCKET s = ::WSASocket(res->ai_family, res->ai_socktype, res->ai_protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,lvalue): WSASocket failed, err={}", WSAGetLastError());
#endif
            ::freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::SocketCreationFailed;
        }

        this->header_->fd = s;

        u_long mode = 1;
        ::ioctlsocket(s, FIONBIO, &mode);

        if (res->ai_family == AF_INET)
        {
            this->address = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        }
        else
        {
            this->address = *reinterpret_cast<sockaddr_in6*>(res->ai_addr);
            this->ipv = utils::net::IPV::IPV6;
        }

        const bool has_timeout = (connect_timeout > std::chrono::milliseconds{0});
        if (has_timeout)
        {
            auto ms = connect_timeout.count();
            this->set_timeout_ms(static_cast<timeout_t>(ms));
        }

        int brc = 0;
        if (res->ai_family == AF_INET)
        {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = 0;

            brc = ::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        }
        else
        {
            sockaddr_in6 local6{};
            local6.sin6_family = AF_INET6;
            local6.sin6_addr = in6addr_any;
            local6.sin6_port = 0;

            brc = ::bind(s, reinterpret_cast<sockaddr*>(&local6), sizeof(local6));
        }

        if (brc == SOCKET_ERROR)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,lvalue): bind(any) failed, err={}", WSAGetLastError());
#endif
            ::closesocket(s);
            this->header_->fd = INVALID_FD;
            ::freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);
#if UVENT_DEBUG
        spdlog::debug("async_connect(win,lvalue): addEvent(ALL) fd={}", (socket_fd_t)this->header_->fd);
#endif

        LPFN_CONNECTEX connect_ex = detail::get_connect_ex(s);
        if (!connect_ex)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,lvalue): get_connect_ex failed");
#endif
            ::closesocket(s);
            this->header_->fd = INVALID_FD;
            ::freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        this->header_->socket_info |= static_cast<uint8_t>(net::AdditionalState::CONNECTION_PENDING);

        auto ov = std::make_unique<IocpOverlapped>();
        ov->header = this->header_;
        ov->op = IocpOp::CONNECT;
        std::memset(&ov->ov, 0, sizeof(ov->ov));

        DWORD bytes_sent = 0;

#if UVENT_DEBUG
        spdlog::trace("async_connect(win,lvalue): calling ConnectEx fd={}", (socket_fd_t)this->header_->fd);
#endif

        BOOL ok = connect_ex(s, res->ai_addr, static_cast<int>(res->ai_addrlen), nullptr, 0, &bytes_sent, &ov->ov);

        if (!ok)
        {
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING)
            {
#if UVENT_DEBUG
                spdlog::error("async_connect(win,lvalue): ConnectEx failed immediately, err={}", err);
#endif
                ::closesocket(s);
                this->header_->fd = INVALID_FD;
                ::freeaddrinfo(res);
                co_return usub::utils::errors::ConnectError::ConnectFailed;
            }
#if UVENT_DEBUG
            spdlog::trace("async_connect(win,lvalue): ConnectEx pending, err=ERROR_IO_PENDING");
#endif
        }
        else
        {
#if UVENT_DEBUG
            spdlog::trace("async_connect(win,lvalue): ConnectEx completed synchronously, bytes_sent={}", bytes_sent);
#endif
        }

#if UVENT_DEBUG
        spdlog::trace("async_connect(win,lvalue): await AwaiterWrite fd={}", (socket_fd_t)this->header_->fd);
#endif
        co_await detail::AwaiterWrite{this->header_};

        ::freeaddrinfo(res);

        if (this->header_->socket_info & static_cast<uint8_t>(net::AdditionalState::CONNECTION_FAILED))
        {
            auto err = has_timeout ? usub::utils::errors::ConnectError::Timeout
                                   : usub::utils::errors::ConnectError::ConnectFailed;

#if UVENT_DEBUG
            spdlog::error("async_connect(win,lvalue): CONNECTION_FAILED fd={} (err={})", (socket_fd_t)this->header_->fd,
                          static_cast<int>(err));
#endif
            ::closesocket(this->header_->fd);
            this->header_->fd = INVALID_FD;
            co_return err;
        }

        int opt_rc = ::setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
        if (opt_rc == SOCKET_ERROR)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,lvalue): SO_UPDATE_CONNECT_CONTEXT failed, err={}", WSAGetLastError());
#endif
            ::closesocket(this->header_->fd);
            this->header_->fd = INVALID_FD;
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }
        system::this_thread::detail::wh.removeTimer(this->header_->timer_id);

#ifndef UVENT_ENABLE_REUSEADDR
        if (has_timeout)
            this->header_->state.fetch_sub(1, std::memory_order_acq_rel);
        this->header_->timeout_epoch_bump();
#else
        if (has_timeout)
        {
            using namespace usub::utils::sync::refc;
            uint64_t& st = this->header_->state;
            const uint64_t cnt = st & COUNT_MASK;
            if (cnt > 0)
                st = (st & ~COUNT_MASK) | ((cnt - 1) & COUNT_MASK);
        }
#endif

#if UVENT_DEBUG
        spdlog::info("async_connect(win,lvalue): connected fd={}", (socket_fd_t)this->header_->fd);
#endif

        co_return std::nullopt;
    }

    template <Proto p, Role r>
    [[nodiscard]] task::Awaitable<std::optional<usub::utils::errors::ConnectError>,
                                  uvent::detail::AwaitableIOFrame<std::optional<usub::utils::errors::ConnectError>>>
    Socket<p, r>::async_connect(std::string&& host, std::string&& port, std::chrono::milliseconds connect_timeout)
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
#if UVENT_DEBUG
        spdlog::info("async_connect(win,rvalue): host='{}' port='{}'", host, port);
#endif

        addrinfo hints{}, *res = nullptr;
        hints.ai_family = (this->ipv == utils::net::IPV::IPV4) ? AF_INET : AF_INET6;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,rvalue): getaddrinfo failed");
#endif
            this->header_->fd = INVALID_FD;
            co_return usub::utils::errors::ConnectError::GetAddrInfoFailed;
        }

        SOCKET s = ::WSASocket(res->ai_family, res->ai_socktype, res->ai_protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,rvalue): WSASocket failed, err={}", WSAGetLastError());
#endif
            ::freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::SocketCreationFailed;
        }

        this->header_->fd = s;

        u_long mode = 1;
        ::ioctlsocket(s, FIONBIO, &mode);

        if (res->ai_family == AF_INET)
        {
            this->address = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        }
        else
        {
            this->address = *reinterpret_cast<sockaddr_in6*>(res->ai_addr);
            this->ipv = utils::net::IPV::IPV6;
        }

        const bool has_timeout = (connect_timeout > std::chrono::milliseconds{0});
        if (has_timeout)
        {
            auto ms = connect_timeout.count();
            this->set_timeout_ms(static_cast<timeout_t>(ms));
        }

        int brc = 0;
        if (res->ai_family == AF_INET)
        {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = 0;

            brc = ::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        }
        else
        {
            sockaddr_in6 local6{};
            local6.sin6_family = AF_INET6;
            local6.sin6_addr = in6addr_any;
            local6.sin6_port = 0;

            brc = ::bind(s, reinterpret_cast<sockaddr*>(&local6), sizeof(local6));
        }

        if (brc == SOCKET_ERROR)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,rvalue): bind(any) failed, err={}", WSAGetLastError());
#endif
            ::closesocket(s);
            this->header_->fd = INVALID_FD;
            ::freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        system::this_thread::detail::pl.addEvent(this->header_, core::OperationType::ALL);
#if UVENT_DEBUG
        spdlog::debug("async_connect(win,rvalue): addEvent(ALL) fd={}", (socket_fd_t)this->header_->fd);
#endif

        LPFN_CONNECTEX connect_ex = detail::get_connect_ex(s);
        if (!connect_ex)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,rvalue): get_connect_ex failed");
#endif
            ::closesocket(s);
            this->header_->fd = INVALID_FD;
            ::freeaddrinfo(res);
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

        this->header_->socket_info |= static_cast<uint8_t>(net::AdditionalState::CONNECTION_PENDING);

        auto ov = std::make_unique<IocpOverlapped>();
        ov->header = this->header_;
        ov->op = IocpOp::CONNECT;
        std::memset(&ov->ov, 0, sizeof(ov->ov));

        DWORD bytes_sent = 0;

#if UVENT_DEBUG
        spdlog::trace("async_connect(win,rvalue): calling ConnectEx fd={}", (socket_fd_t)this->header_->fd);
#endif

        BOOL ok = connect_ex(s, res->ai_addr, static_cast<int>(res->ai_addrlen), nullptr, 0, &bytes_sent, &ov->ov);

        if (!ok)
        {
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING)
            {
#if UVENT_DEBUG
                spdlog::error("async_connect(win,rvalue): ConnectEx failed immediately, err={}", err);
#endif
                ::closesocket(s);
                this->header_->fd = INVALID_FD;
                ::freeaddrinfo(res);
                co_return usub::utils::errors::ConnectError::ConnectFailed;
            }
#if UVENT_DEBUG
            spdlog::trace("async_connect(win,rvalue): ConnectEx pending, err=ERROR_IO_PENDING");
#endif
        }
        else
        {
#if UVENT_DEBUG
            spdlog::trace("async_connect(win,rvalue): ConnectEx completed synchronously, bytes_sent={}", bytes_sent);
#endif
        }

#if UVENT_DEBUG
        spdlog::trace("async_connect(win,rvalue): await AwaiterWrite fd={}", (socket_fd_t)this->header_->fd);
#endif
        co_await detail::AwaiterWrite{this->header_};

        ::freeaddrinfo(res);

        if (this->header_->socket_info & static_cast<uint8_t>(net::AdditionalState::CONNECTION_FAILED))
        {
            auto err = has_timeout ? usub::utils::errors::ConnectError::Timeout
                                   : usub::utils::errors::ConnectError::ConnectFailed;

#if UVENT_DEBUG
            spdlog::error("async_connect(win,rvalue): CONNECTION_FAILED fd={} (err={})", (socket_fd_t)this->header_->fd,
                          static_cast<int>(err));
#endif
            ::closesocket(this->header_->fd);
            this->header_->fd = INVALID_FD;
            co_return err;
        }

        int opt_rc = ::setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
        if (opt_rc == SOCKET_ERROR)
        {
#if UVENT_DEBUG
            spdlog::error("async_connect(win,rvalue): SO_UPDATE_CONNECT_CONTEXT failed, err={}", WSAGetLastError());
#endif
            ::closesocket(this->header_->fd);
            this->header_->fd = INVALID_FD;
            co_return usub::utils::errors::ConnectError::ConnectFailed;
        }

#ifndef UVENT_ENABLE_REUSEADDR
        this->header_->timeout_epoch_bump();
#endif

        if (has_timeout)
        {
            this->update_timeout(settings::timeout_duration_ms);

#ifndef UVENT_ENABLE_REUSEADDR
            this->header_->state.fetch_sub(1, std::memory_order_acq_rel);
#else
            {
                using namespace usub::utils::sync::refc;
                uint64_t& st = this->header_->state;
                const uint64_t cnt = st & COUNT_MASK;
                if (cnt > 0)
                    st = (st & ~COUNT_MASK) | ((cnt - 1) & COUNT_MASK);
            }
#endif
        }

#if UVENT_DEBUG
        spdlog::info("async_connect(win,rvalue): connected fd={}", (socket_fd_t)this->header_->fd);
#endif

        co_return std::nullopt;
    }


    template <Proto p, Role r>
    task::Awaitable<std::expected<size_t, usub::utils::errors::SendError>,
                    uvent::detail::AwaitableIOFrame<std::expected<size_t, usub::utils::errors::SendError>>>
    Socket<p, r>::async_send(uint8_t* buf, size_t sz)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("async_send(win): fd={}, sz={}", (std::uint64_t)this->header_->fd, sz);
#endif
        auto buf_internal = std::unique_ptr<uint8_t[]>(new uint8_t[sz]);
        std::memcpy(buf_internal.get(), buf, sz);

        ssize_t total_written = 0;
        int retries = 0;

        while (total_written < static_cast<ssize_t>(sz))
        {
            for (;;)
            {
                if (this->is_disconnected_now())
                {
#if UVENT_DEBUG
                    spdlog::warn("async_send(win): is_disconnected_now(), fd={}", (std::uint64_t)this->header_->fd);
#endif
                    co_return std::unexpected(usub::utils::errors::SendError::Closed);
                }

                int res = 0;

                if constexpr (p == Proto::TCP)
                {
                    res = ::send(this->header_->fd, reinterpret_cast<const char*>(buf_internal.get() + total_written),
                                 static_cast<int>(sz - static_cast<size_t>(total_written)), 0);
                }
                else
                {
                    try
                    {
                        if (std::holds_alternative<sockaddr_in>(this->address))
                        {
                            auto& addr = std::get<sockaddr_in>(this->address);
                            int addr_len = sizeof(sockaddr_in);
                            res = ::sendto(this->header_->fd,
                                           reinterpret_cast<const char*>(buf_internal.get() + total_written),
                                           static_cast<int>(sz - static_cast<size_t>(total_written)), 0,
                                           reinterpret_cast<sockaddr*>(&addr), addr_len);
                        }
                        else if (std::holds_alternative<sockaddr_in6>(this->address))
                        {
                            auto& addr = std::get<sockaddr_in6>(this->address);
                            int addr_len = sizeof(sockaddr_in6);
                            res = ::sendto(this->header_->fd,
                                           reinterpret_cast<const char*>(buf_internal.get() + total_written),
                                           static_cast<int>(sz - static_cast<size_t>(total_written)), 0,
                                           reinterpret_cast<sockaddr*>(&addr), addr_len);
                        }
                        else
                        {
#if UVENT_DEBUG
                            spdlog::error("async_send(udp)(win): InvalidAddressVariant fd={}",
                                          (std::uint64_t)this->header_->fd);
#endif
                            co_return std::unexpected(usub::utils::errors::SendError::InvalidAddressVariant);
                        }
                    }
                    catch (const std::bad_variant_access&)
                    {
#if UVENT_DEBUG
                        spdlog::error("async_send(udp)(win): bad_variant_access fd={}",
                                      (std::uint64_t)this->header_->fd);
#endif
                        co_return std::unexpected(usub::utils::errors::SendError::InvalidAddressVariant);
                    }
                }

#if UVENT_DEBUG
                spdlog::trace("async_send(win): send/sendto res={} fd={}", res, (std::uint64_t)this->header_->fd);
#endif

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
#if UVENT_DEBUG
                    spdlog::warn("async_send(win): res=0 fd={}", (std::uint64_t)this->header_->fd);
#endif
                    this->remove();
                    co_return std::unexpected(usub::utils::errors::SendError::SendFailed);
                }

                int err = WSAGetLastError();
#if UVENT_DEBUG
                spdlog::debug("async_send(win): error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                if (err == WSAEINTR)
                {
                    if (++retries >= settings::max_write_retries)
                    {
                        this->remove();
                        co_return std::unexpected(usub::utils::errors::SendError::SendFailed);
                    }
                    continue;
                }

                if (err == WSAEWOULDBLOCK)
                {
#if UVENT_DEBUG
                    spdlog::trace("async_send(win): WSAEWOULDBLOCK, wait write fd={}",
                                  (std::uint64_t)this->header_->fd);
#endif
                    break;
                }

                this->remove();
                co_return std::unexpected(usub::utils::errors::SendError::SendFailed);
            }

#if UVENT_DEBUG
            spdlog::trace("async_send(win): awaiting AwaiterWrite fd={}", (std::uint64_t)this->header_->fd);
#endif
            co_await detail::AwaiterWrite{this->header_};
        }

    send_done:
#ifndef UVENT_ENABLE_REUSEADDR
        if (total_written > 0)
            this->header_->timeout_epoch_bump();
#endif
#if UVENT_DEBUG
        spdlog::info("async_send(win): done total_written={} fd={}", total_written, (std::uint64_t)this->header_->fd);
#endif
        co_return static_cast<size_t>(total_written);
    }

    template <Proto p, Role r>
    std::expected<std::string, usub::utils::errors::SendError> Socket<p, r>::send(uint8_t* buf, size_t sz,
                                                                                  size_t chunkSize, size_t maxSize)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("send(sync+recv)(win): fd={}, sz={}, chunkSize={}, maxSize={}", (std::uint64_t)this->header_->fd,
                     sz, chunkSize, maxSize);
#endif
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
#if UVENT_DEBUG
        spdlog::info("async_sendfile(win): fd={}, in_fd={}, count={}", (std::uint64_t)this->header_->fd, in_fd, count);
#endif
        auto ov = std::make_unique<IocpOverlapped>();
        ov->header = this->header_;
        ov->op = IocpOp::WRITE;

#if UVENT_DEBUG
        spdlog::trace("async_sendfile(win): wait AwaiterWrite(fd={}) before TransmitFile",
                      (std::uint64_t)this->header_->fd);
#endif
        co_await detail::AwaiterWrite{this->header_};
        if (this->is_disconnected_now())
        {
#if UVENT_DEBUG
            spdlog::warn("async_sendfile(win): disconnected before TransmitFile fd={}",
                         (std::uint64_t)this->header_->fd);
#endif
            co_return -3;
        }

        HANDLE hFile = (HANDLE)_get_osfhandle(in_fd);
        if (hFile == INVALID_HANDLE_VALUE)
        {
#if UVENT_DEBUG
            spdlog::error("async_sendfile(win): invalid hFile");
#endif
            co_return -1;
        }

        BOOL ok = ::TransmitFile(this->header_->fd, hFile, static_cast<DWORD>(count), 0, &ov->ov, nullptr, 0);
        if (!ok)
        {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING)
            {
#if UVENT_DEBUG
                spdlog::debug("TransmitFile(win) error: {}", err);
#endif
                co_return -1;
            }
        }

        DWORD bytes = 0;
        DWORD flags = 0;
        if (!::WSAGetOverlappedResult(this->header_->fd, &ov->ov, &bytes, TRUE, &flags))
        {
#if UVENT_DEBUG
            spdlog::debug("TransmitFile(win) overlapped result error: {}", WSAGetLastError());
#endif
            co_return -1;
        }

#ifndef UVENT_ENABLE_REUSEADDR
        if (bytes > 0)
            this->header_->timeout_epoch_bump();
#endif

        if (offset)
            *offset += static_cast<off_t>(bytes);
#if UVENT_DEBUG
        spdlog::info("async_sendfile(win): bytes={} fd={}", bytes, (std::uint64_t)this->header_->fd);
#endif
        co_return static_cast<ssize_t>(bytes);
    }

    template <Proto p, Role r>
    ssize_t Socket<p, r>::sendfile(int in_fd, off_t* offset, size_t count)
        requires((p == Proto::TCP && r == Role::ACTIVE) || (p == Proto::UDP))
    {
#if UVENT_DEBUG
        spdlog::info("sendfile(sync)(win): fd={}, in_fd={}, count={}", (std::uint64_t)this->header_->fd, in_fd, count);
#endif
        HANDLE hFile = (HANDLE)_get_osfhandle(in_fd);
        if (hFile == INVALID_HANDLE_VALUE)
        {
#if UVENT_DEBUG
            spdlog::debug("sendfile(sync)(win): invalid hFile");
#endif
            return -1;
        }

        BOOL ok = ::TransmitFile(this->header_->fd, hFile, static_cast<DWORD>(count), 0, nullptr, nullptr, 0);
        if (!ok)
        {
#if UVENT_DEBUG
            spdlog::debug("TransmitFile(win) error: {}", WSAGetLastError());
#endif
            return -1;
        }

        if (offset)
            *offset += static_cast<off_t>(count);
        return static_cast<ssize_t>(count);
    }

    template <Proto p, Role r>
    void Socket<p, r>::update_timeout(timer_duration_t new_duration) const
    {
#if UVENT_DEBUG
        spdlog::debug("update_timeout(win): fd={}",
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
        system::this_thread::detail::wh.updateTimer(this->header_->timer_id, new_duration);
    }


    template <Proto p, Role r>
    void Socket<p, r>::shutdown()
    {
#if UVENT_DEBUG
        spdlog::info("shutdown(win): fd={}", (std::uint64_t)this->header_->fd);
#endif
        ::shutdown(this->header_->fd, SD_BOTH);
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
        spdlog::debug("set_timeout_ms(win): fd={}, timeout_ms={}, counter={}", (std::uint64_t)this->header_->fd,
                      timeout, this->header_->get_counter());
#endif
        auto* timer = new utils::Timer(timeout);
        timer->addFunction(detail::processSocketTimeout, this->header_);
        this->header_->timer_id = system::this_thread::detail::wh.addTimer(timer);
    }

    template <Proto p, Role r>
    void Socket<p, r>::destroy() noexcept
    {
#if UVENT_DEBUG
        spdlog::info("Socket::destroy(win): header={}, fd={}", static_cast<void*>(this->header_),
                     this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
        this->header_->close_for_new_refs();
        system::this_thread::detail::pl.removeEvent(this->header_, core::OperationType::ALL);
#ifndef UVENT_ENABLE_REUSEADDR
        system::this_thread::detail::g_qsbr.retire(static_cast<void*>(this->header_), &delete_header);
#else
        system::this_thread::detail::q_sh.enqueue(this->header_);
#endif
    }

    template <Proto p, Role r>
    void Socket<p, r>::remove()
    {
#if UVENT_DEBUG
        spdlog::info("Socket::remove(win): header={}, fd={}", static_cast<void*>(this->header_),
                     this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull);
#endif
        system::this_thread::detail::pl.removeEvent(this->header_, core::OperationType::ALL);
        this->header_->close_for_new_refs();
    }

    template <Proto p, Role r>
    std::expected<std::string, usub::utils::errors::SendError> Socket<p, r>::receive(size_t chunk_size, size_t maxSize)
    {
#if UVENT_DEBUG
        spdlog::info("receive(sync)(win): fd={}, chunk_size={}, maxSize={}", (std::uint64_t)this->header_->fd,
                     chunk_size, maxSize);
#endif
        std::string result;
        result.reserve(chunk_size * 2);

        size_t totalReceive{0};
        auto recv_loop = [&](auto&& recv_fn) -> std::expected<std::string, usub::utils::errors::SendError>
        {
            std::vector<char> buf(chunk_size);
            while (true)
            {
                int received = recv_fn(buf.data(), buf.size());
#if UVENT_DEBUG
                spdlog::trace("receive(sync)(win): recv received={} fd={}", received, (std::uint64_t)this->header_->fd);
#endif
                if (received < 0)
                {
                    int err = WSAGetLastError();
#if UVENT_DEBUG
                    spdlog::debug("receive(sync)(win): recv error={} fd={}", err, (std::uint64_t)this->header_->fd);
#endif
                    if (err == WSAEWOULDBLOCK)
                        break;
                    return std::unexpected(usub::utils::errors::SendError::RecvFailed);
                }
                if (received == 0)
                    break;

                totalReceive += received;
                if (totalReceive >= maxSize)
                    break;

                result.append(buf.data(), received);
                if (received < static_cast<int>(chunk_size))
                    break;
            }
            return result;
        };

        if constexpr (p == Proto::TCP)
        {
            return recv_loop([&](char* b, size_t sz) { return ::recv(this->header_->fd, b, static_cast<int>(sz), 0); });
        }

        try
        {
            return std::visit(
                [&](auto&& addr) -> std::expected<std::string, usub::utils::errors::SendError>
                {
                    using T = std::decay_t<decltype(addr)>;
                    int addr_len = sizeof(T);
                    return recv_loop(
                        [&](char* b, size_t sz)
                        {
                            return ::recvfrom(this->header_->fd, b, static_cast<int>(sz), 0,
                                              reinterpret_cast<sockaddr*>(&addr), &addr_len);
                        });
                },
                this->address);
        }
        catch (const std::bad_variant_access&)
        {
#if UVENT_DEBUG
            spdlog::error("receive(sync)(win): bad_variant_access fd={}", (std::uint64_t)this->header_->fd);
#endif
            return std::unexpected(usub::utils::errors::SendError::InvalidAddressVariant);
        }
    }

    template <Proto p, Role r>
    client_addr_t Socket<p, r>::get_client_addr() const
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
#if UVENT_DEBUG
        spdlog::trace("get_client_addr(const)(win): fd={}", (std::uint64_t)this->header_->fd);
#endif
        return this->address;
    }

    template <Proto p, Role r>
    client_addr_t Socket<p, r>::get_client_addr()
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
#if UVENT_DEBUG
        spdlog::trace("get_client_addr(win): fd={}", (std::uint64_t)this->header_->fd);
#endif
        return this->address;
    }

    template <Proto p, Role r>
    utils::net::IPV Socket<p, r>::get_client_ipv() const
        requires(p == Proto::TCP && r == Role::ACTIVE)
    {
#if UVENT_DEBUG
        spdlog::trace("get_client_ipv(win): fd={}, ipv={}", (std::uint64_t)this->header_->fd,
                      static_cast<int>(this->ipv));
#endif
        return this->ipv;
    }

    template <Proto p, Role r>
    size_t Socket<p, r>::send_aux(uint8_t* buf, size_t size)
    {
#if UVENT_DEBUG
        spdlog::trace("send_aux(win): fd={}, size={}",
                      this->header_ ? static_cast<std::uint64_t>(this->header_->fd) : 0ull, size);
#endif
        if (this->header_->fd == INVALID_FD)
            return static_cast<size_t>(-1);

        if constexpr (p == Proto::TCP)
        {
            int r = ::send(this->header_->fd, reinterpret_cast<const char*>(buf), static_cast<int>(size), 0);
#if UVENT_DEBUG
            spdlog::trace("send_aux(tcp)(win): send r={} fd={}", r, (std::uint64_t)this->header_->fd);
#endif
            return (r < 0) ? static_cast<size_t>(-1) : static_cast<size_t>(r);
        }

        try
        {
            return std::visit(
                [&](auto&& addr) -> size_t
                {
                    using T = std::decay_t<decltype(addr)>;
                    int addr_len = static_cast<int>(sizeof(T));
                    int r = ::sendto(this->header_->fd, reinterpret_cast<const char*>(buf), static_cast<int>(size), 0,
                                     reinterpret_cast<sockaddr*>(&addr), addr_len);
#if UVENT_DEBUG
                    spdlog::trace("send_aux(udp)(win): sendto r={} fd={}", r, (std::uint64_t)this->header_->fd);
#endif
                    return (r < 0) ? static_cast<size_t>(-1) : static_cast<size_t>(r);
                },
                this->address);
        }
        catch (const std::bad_variant_access&)
        {
#if UVENT_DEBUG
            spdlog::error("send_aux(udp)(win): bad_variant_access fd={}", (std::uint64_t)this->header_->fd);
#endif
            return static_cast<size_t>(-1);
        }
    }
} // namespace usub::uvent::net

#endif // SOCKETWINDOWS_H
