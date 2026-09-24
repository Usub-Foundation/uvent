//
// Created by kirill on 8/30/24.
//

#include "uvent/utils/net/socket.h"

namespace usub::uvent::utils::socket {

    socket_fd_t createSocket(int port,
                             const std::string &ip_addr,
                             int backlog,
                             net::IPV ipv,
                             net::SocketAddressType socType)
    {
#if defined(OS_LINUX) || defined(OS_BSD) || defined(OS_APPLE)

        int soc_fd = ::socket((ipv == net::IPV::IPV4) ? AF_INET : AF_INET6,
                              (socType == net::SocketAddressType::TCP) ? SOCK_STREAM : SOCK_DGRAM,
                              0);
        if (soc_fd < 0)
            throw std::system_error(errno, std::generic_category(), "socket()");

#ifdef SO_REUSEPORT
        int reuse = 1;
        if (::setsockopt(soc_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
            throw std::system_error(errno, std::generic_category(), "setsockopt(SO_REUSEADDR)");
        if (::setsockopt(soc_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0)
            throw std::system_error(errno, std::generic_category(), "setsockopt(SO_REUSEPORT)");
#endif

        if (ipv == net::IPV::IPV4) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = (ip_addr == "127.0.0.1")
                                   ? htonl(INADDR_LOOPBACK)
                                   : ::inet_addr(ip_addr.c_str());

            if (::bind(soc_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
                throw std::system_error(errno, std::generic_category(), "bind()");
        } else {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            if (::inet_pton(AF_INET6, ip_addr.c_str(), &addr.sin6_addr) <= 0)
                throw std::system_error(errno, std::generic_category(), "inet_pton(AF_INET6)");

            if (::bind(soc_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
                throw std::system_error(errno, std::generic_category(), "bind()");
        }

        if (socType == net::SocketAddressType::TCP) {
            if (::listen(soc_fd, backlog) < 0)
                throw std::system_error(errno, std::generic_category(), "listen()");
        }

        int flags;
        if ((flags = ::fcntl(soc_fd, F_GETFL, 0)) < 0 ||
            ::fcntl(soc_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "fcntl()");
        }

        return soc_fd;

#elif defined(OS_WINDOWS)

        wsa_init_once(); // the helper is usable before any runtime exists

        int family = (ipv == net::IPV::IPV4) ? AF_INET : AF_INET6;
        int type   = (socType == net::SocketAddressType::TCP) ? SOCK_STREAM : SOCK_DGRAM;
        int proto  = (socType == net::SocketAddressType::TCP) ? IPPROTO_TCP : IPPROTO_UDP;

        SOCKET s = ::socket(family, type, proto);
        if (s == INVALID_SOCKET)
            throw std::system_error(WSAGetLastError(), std::system_category(), "socket()");

        BOOL reuse = TRUE;
        if (::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&reuse),
                         sizeof(reuse)) == SOCKET_ERROR)
        {
            int ec = WSAGetLastError();
            ::closesocket(s);
            throw std::system_error(ec, std::system_category(), "setsockopt(SO_REUSEADDR)");
        }

        if (ipv == net::IPV::IPV4) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(static_cast<u_short>(port));

            if (ip_addr == "127.0.0.1") {
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            } else if (ip_addr == "0.0.0.0") {
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
            } else {
                if (::InetPtonA(AF_INET, ip_addr.c_str(), &addr.sin_addr) != 1) {
                    int ec = WSAGetLastError();
                    ::closesocket(s);
                    throw std::system_error(ec, std::system_category(), "InetPtonA(AF_INET)");
                }
            }

            if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
                int ec = WSAGetLastError();
                ::closesocket(s);
                throw std::system_error(ec, std::system_category(), "bind()");
            }
        } else {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port   = htons(static_cast<u_short>(port));

            if (::InetPtonA(AF_INET6, ip_addr.c_str(), &addr.sin6_addr) != 1) {
                int ec = WSAGetLastError();
                ::closesocket(s);
                throw std::system_error(ec, std::system_category(), "InetPtonA(AF_INET6)");
            }

            if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
                int ec = WSAGetLastError();
                ::closesocket(s);
                throw std::system_error(ec, std::system_category(), "bind()");
            }
        }

        if (socType == net::SocketAddressType::TCP) {
            if (::listen(s, backlog) == SOCKET_ERROR) {
                int ec = WSAGetLastError();
                ::closesocket(s);
                throw std::system_error(ec, std::system_category(), "listen()");
            }
        }

        if (!makeSocketNonBlocking(s)) {
            int ec = WSAGetLastError();
            ::closesocket(s);
            throw std::system_error(ec, std::system_category(), "makeSocketNonBlocking()");
        }

        return s;

#else
        static_assert(sizeof(port) == 0, "Unsupported platform");
#endif
    }

} // namespace usub::uvent::utils::socket