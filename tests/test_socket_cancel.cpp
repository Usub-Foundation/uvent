#include <cerrno>

#include "test_common.h"
#include "uvent/Uvent.h"

#if defined(UVENT_SOCKET_OWNER_FORWARDING)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    int connect_blocking(uint16_t port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (int i = 0; i < 100; ++i)
        {
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
                return fd;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::close(fd);
        std::abort();
    }

    constexpr uint16_t kReadPort = 24311; // below the ephemeral port range
    constexpr uint16_t kAcceptPort = 24312;

    std::atomic<bool> g_read_cancelled{false};

    task::Awaitable<void> silent_reader(usub::Uvent* rt, net::TCPClientSocket client)
    {
        uint8_t buf[256];
        const ssize_t r = co_await client.async_read(buf, sizeof(buf));
        if (r == -1 && errno == ECANCELED)
            g_read_cancelled.store(true, std::memory_order_release);
        client.shutdown();
        rt->stop();
    }

    task::Awaitable<void> read_server(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kReadPort};
        auto client = co_await acceptor.async_accept();
        CHECK(client.has_value());
        auto h = task::spawn(silent_reader(rt, std::move(*client)));
        co_await system::this_coroutine::sleep_for(100ms);
        h.cancel();
    }

    void read_cancellation()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(read_server(&rt), 0);
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kReadPort);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                ::close(fd);
            });
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        client.join();
        CHECK(g_read_cancelled.load());
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }

    std::atomic<bool> g_accept_cancelled{false};

    task::Awaitable<void> idle_acceptor(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kAcceptPort};
        auto client = co_await acceptor.async_accept();
        if (!client.has_value())
            g_accept_cancelled.store(true, std::memory_order_release);
        rt->stop();
    }

    task::Awaitable<void> accept_cancel_driver(usub::Uvent* rt)
    {
        auto h = task::spawn(idle_acceptor(rt), 1);
        co_await system::this_coroutine::sleep_for(100ms);
        h.cancel();
    }

    void accept_cancellation()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(accept_cancel_driver(&rt), 0);
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        CHECK(g_accept_cancelled.load());
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }
} // namespace

int main()
{
    return run_tests({
        {"read_cancellation", read_cancellation},
        {"accept_cancellation", accept_cancellation},
    });
}

#else

int main()
{
    std::printf("skipped: socket cancellation requires UVENT_SOCKET_OWNER_FORWARDING\n");
    return 0;
}

#endif
