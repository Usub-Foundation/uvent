// Small utilities with no tests of their own: error-code names, the raw
// socket helpers, and the per-coroutine trace id / name setters.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // windows.h min/max macros would break std::min in uvent headers
#endif
#include <winsock2.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/utils/errors/IOErrors.h"
#include "uvent/utils/net/socket.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    constexpr uint16_t kHelperPort = 24405; // below the ephemeral range

    void error_names_are_non_empty_and_distinct()
    {
        using usub::utils::errors::ConnectError;
        using usub::utils::errors::SendError;
        using usub::utils::errors::toString;

        const ConnectError connect_errors[] = {
            ConnectError::GetAddrInfoFailed, ConnectError::SocketCreationFailed, ConnectError::ConnectFailed,
            ConnectError::InvalidAddressFamily, ConnectError::InvalidSockType, ConnectError::FcntlFailed,
            ConnectError::EpollAddFailed, ConnectError::AlreadyConnected, ConnectError::InvalidHostname,
            ConnectError::Timeout, ConnectError::Cancelled, ConnectError::Unknown,
        };
        std::vector<std::string> seen;
        for (auto e : connect_errors)
        {
            const char* s = toString(e);
            CHECK(s != nullptr && *s != '\0');
            for (auto& prev : seen)
                CHECK(prev != s);
            seen.emplace_back(s);
        }

        const SendError send_errors[] = {
            SendError::InvalidSocketFd, SendError::RecvFailed, SendError::RecvFromFailed,
            SendError::InvalidAddressVariant, SendError::Timeout, SendError::Closed,
            SendError::SendFailed, SendError::Cancelled,
        };
        seen.clear();
        for (auto e : send_errors)
        {
            const char* s = toString(e);
            CHECK(s != nullptr && *s != '\0');
            for (auto& prev : seen)
                CHECK(prev != s);
            seen.emplace_back(s);
        }

        const char* sock = toString(usub::utils::errors::SocketError::Timeout);
        CHECK(sock != nullptr && *sock != '\0');
    }

    void create_socket_and_make_non_blocking()
    {
        const socket_fd_t fd = utils::socket::createSocket(kHelperPort, "127.0.0.1", 16, utils::net::IPV4,
                                                           utils::net::TCP);
        CHECK(fd >= 0);
        CHECK(utils::socket::makeSocketNonBlocking(fd));
#ifdef _WIN32
        ::closesocket(fd);
#else
        const int fl = ::fcntl(fd, F_GETFL, 0);
        CHECK(fl != -1 && (fl & O_NONBLOCK));
        const int fdfl = ::fcntl(fd, F_GETFD, 0);
        CHECK(fdfl != -1 && (fdfl & FD_CLOEXEC));
        ::close(fd);
#endif

        // A closed descriptor cannot be switched: the helper reports it.
        CHECK(!utils::socket::makeSocketNonBlocking(fd));

        const socket_fd_t udp = utils::socket::createSocket(kHelperPort, "127.0.0.1", 0, utils::net::IPV4,
                                                            utils::net::UDP);
        CHECK(udp >= 0);
#ifdef _WIN32
        ::closesocket(udp);
#else
        ::close(udp);
#endif
    }

    std::atomic<uint64_t> g_child_seen_trace{0};

    task::Awaitable<int> traced_child()
    {
        // The id set by the parent is visible in the awaited child...
        g_child_seen_trace.store(system::this_coroutine::trace_id(), std::memory_order_release);
        // ...and a change made by the child is pushed up the await chain.
        system::this_coroutine::set_trace_id(77);
        system::this_coroutine::set_name("traced_child");
        co_return 1;
    }

    task::Awaitable<void> traced_parent(usub::Uvent* rt)
    {
        system::this_coroutine::set_trace_id(42);
        system::this_coroutine::set_name("traced_parent");
        CHECK_EQ(system::this_coroutine::trace_id(), 42u);
        const int v = co_await traced_child();
        CHECK_EQ(v, 1);
        CHECK_EQ(g_child_seen_trace.load(), 42u);
        CHECK_EQ(system::this_coroutine::trace_id(), 77u);
        rt->stop();
    }

    void trace_id_and_name_follow_the_await_chain()
    {
        // Outside any coroutine the setters are no-ops instead of crashes.
        system::this_coroutine::set_name("outside");
        system::this_coroutine::set_trace_id(1);

        usub::Uvent rt(1);
        system::co_spawn_static(traced_parent(&rt), 0);
        rt.run();
    }
} // namespace

int main()
{
    return run_tests({
        {"error_names_are_non_empty_and_distinct", error_names_are_non_empty_and_distinct},
        {"create_socket_and_make_non_blocking", create_socket_and_make_non_blocking},
        {"trace_id_and_name_follow_the_await_chain", trace_id_and_name_follow_the_await_chain},
    });
}
