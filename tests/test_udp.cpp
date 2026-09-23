// UDP sockets: a bound pair on loopback exchanging datagrams through the
// async and the synchronous read/write paths. UDP sockets are wrapped raw
// descriptors (Socket(int fd)); the peer address lives in Socket::address.
#include <cstring>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/utils/buffer/DynamicBuffer.h"
#include "uvent/utils/net/socket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    constexpr int kServerPort = 24421; // below the ephemeral range
    constexpr int kClientPort = 24422;

    sockaddr_in loopback(int port)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return a;
    }

    net::UDPBoundSocket make_udp(int port, int peer_port)
    {
        const socket_fd_t fd = utils::socket::createSocket(port, "127.0.0.1", 0, utils::net::IPV4, utils::net::UDP);
        CHECK(fd >= 0);
        CHECK(utils::socket::makeSocketNonBlocking(fd));
        net::UDPBoundSocket s(fd); // registers with this worker's poller
        s.address = loopback(peer_port);
        return s;
    }

    task::Awaitable<void> udp_body(usub::Uvent* rt)
    {
        auto server = make_udp(kServerPort, kClientPort);
        auto client = make_udp(kClientPort, kServerPort);

        // async: client -> server, then server -> client
        std::string ping = "ping";
        CHECK_EQ(co_await client.async_write(reinterpret_cast<uint8_t*>(ping.data()), ping.size()), 4);
        utils::DynamicBuffer buf;
        const ssize_t r = co_await server.async_read(buf, 1500);
        CHECK_EQ(r, 4);
        CHECK(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()) == "ping");

        std::string pong = "pong!";
        CHECK_EQ(co_await server.async_write(reinterpret_cast<uint8_t*>(pong.data()), pong.size()), 5);
        uint8_t raw[64];
        const ssize_t r2 = co_await client.async_read(raw, sizeof(raw));
        CHECK_EQ(r2, 5);
        CHECK(std::memcmp(raw, "pong!", 5) == 0);

        // datagram boundaries survive: two sends, two reads, each its own size
        std::string a = "aa", b = "bbbb";
        CHECK_EQ(co_await client.async_write(reinterpret_cast<uint8_t*>(a.data()), a.size()), 2);
        CHECK_EQ(co_await client.async_write(reinterpret_cast<uint8_t*>(b.data()), b.size()), 4);
        buf.clear();
        CHECK_EQ(co_await server.async_read(buf, 1500), 2);
        buf.clear();
        CHECK_EQ(co_await server.async_read(buf, 1500), 4);

        // sync variants (data is already in the socket buffer by the time we read)
        std::string s1 = "sync";
        CHECK_EQ(client.write(reinterpret_cast<uint8_t*>(s1.data()), s1.size()), 4);
        co_await system::this_coroutine::sleep_for(20ms);
        buf.clear();
        CHECK_EQ(server.read(buf, 1500), 4);
        CHECK(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()) == "sync");

        // async_send over UDP (sendto with the stored address)
        std::string s2 = "sendto";
        auto sent = co_await client.async_send(reinterpret_cast<uint8_t*>(s2.data()), s2.size());
        CHECK(sent.has_value());
        CHECK_EQ(*sent, 6u);
        buf.clear();
        CHECK_EQ(co_await server.async_read(buf, 1500), 6);

        // zero-length requests are no-ops
        CHECK_EQ(co_await server.async_read(buf, 0), 0);
        CHECK_EQ(co_await client.async_write(raw, 0), 0);

        rt->stop();
    }

    void udp_roundtrip()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(udp_body(&rt), 0);
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
    }
} // namespace

int main()
{
    return run_tests({
        {"udp_roundtrip", udp_roundtrip},
    });
}
