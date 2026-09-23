// Client-side socket paths: async_connect (success / refused / timeout /
// cancelled), async_send, write backpressure (the write waiter actually parks
// and is woken by EPOLLOUT), write timeout on a stalled peer, the inactivity
// timer being refreshed by update_timeout() on a busy connection, sendfile,
// and the Happy Eyeballs connector. All loopback; the server side is uvent too.
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/net/HappyEyeballs.h"
#include "uvent/utils/buffer/DynamicBuffer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The legacy shared-poller layout (UVENT_ENABLE_REUSEADDR=OFF) still hangs in
// socket timeout / connect scenarios (see docs/build-flags.md); these tests
// target the owner-forwarding layout.
#ifdef UVENT_ENABLE_REUSEADDR
using namespace usub::uvent;
using namespace std::chrono_literals;
using usub::utils::errors::ConnectError;

namespace
{
    // Listeners must stay below net.ipv4.ip_local_port_range (32768).
    constexpr int kEchoPort = 24411;
    constexpr int kBackpressurePort = 24412;
    constexpr int kWriteTimeoutPort = 24413;
    constexpr int kKeepAlivePort = 24414;
    constexpr int kSendfilePort = 24415;
    constexpr int kHappyPort = 24416;
    constexpr int kTarpitPort = 24417;
    constexpr int kRefusedPort = 24419; // nobody listens here

    // A listener whose accept queue is full: Linux then drops further SYNs, so a
    // connect() to it sits in SYN_SENT until the caller gives up. That is the
    // only deterministic "black hole" that needs no network at all (public
    // TEST-NET addresses may be proxied or unroutable depending on the host).
    struct Tarpit
    {
        int listener{-1};
        int held[2]{-1, -1};

        Tarpit(const char* ip, int port)
        {
            listener = ::socket(AF_INET, SOCK_STREAM, 0);
            CHECK(listener >= 0);
            int one = 1;
            ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(static_cast<uint16_t>(port));
            CHECK(::inet_pton(AF_INET, ip, &a.sin_addr) == 1);
            CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
            CHECK(::listen(listener, 1) == 0);
            for (int& fd : held) // fill the queue; never accept
            {
                fd = ::socket(AF_INET, SOCK_STREAM, 0);
                CHECK(fd >= 0);
                CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
            }
        }
        ~Tarpit()
        {
            for (int fd : held)
                ::close(fd);
            ::close(listener);
        }
    };

    std::string pattern(std::size_t n)
    {
        std::string s(n, '\0');
        for (std::size_t i = 0; i < n; ++i)
            s[i] = static_cast<char>('a' + (i % 26));
        return s;
    }

    task::Awaitable<void> wait_flag(std::atomic<bool>& flag)
    {
        while (!flag.load(std::memory_order_acquire))
            co_await system::this_coroutine::sleep_for(5ms);
    }

    task::Awaitable<std::string> read_exactly(net::TCPClientSocket& s, std::size_t n)
    {
        std::string out;
        uint8_t buf[64 * 1024];
        while (out.size() < n)
        {
            const ssize_t r = co_await s.async_read(buf, std::min(sizeof(buf), n - out.size()));
            if (r <= 0)
                break;
            out.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(r));
        }
        co_return out;
    }

    // ------------------------------------------------------------ echo server

    task::Awaitable<void> echo_session(net::TCPClientSocket s)
    {
        uint8_t buf[4096];
        for (;;)
        {
            const ssize_t r = co_await s.async_read(buf, sizeof(buf));
            if (r <= 0)
                break;
            if (co_await s.async_write(buf, static_cast<size_t>(r)) <= 0)
                break;
        }
        s.shutdown();
    }

    task::Awaitable<void> echo_server(int port)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", port};
        for (;;)
        {
            auto c = co_await acceptor.async_accept();
            if (c.has_value())
                task::spawn(echo_session(std::move(*c)));
        }
    }

    // ----------------------------------------------------- connect + echo

    task::Awaitable<void> connect_echo_client(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms); // let the listener bind
        net::TCPClientSocket c;
        const auto err = co_await c.async_connect("127.0.0.1", std::to_string(kEchoPort));
        CHECK(!err.has_value());

        const std::string msg = "hello over uvent";
        const ssize_t w = co_await c.async_write(reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data())), msg.size());
        CHECK_EQ(w, static_cast<ssize_t>(msg.size()));
        CHECK(co_await read_exactly(c, msg.size()) == msg);

        // async_send: the chunked variant with the expected<> result. The echo
        // must be drained concurrently: a payload larger than the receive buffer
        // would otherwise deadlock the echo pair (server blocked writing back,
        // client blocked sending), which is a property of echo, not of uvent.
        const std::string big = pattern(200000);
        auto drain = task::spawn(read_exactly(c, big.size()));
        auto sent = co_await c.async_send(reinterpret_cast<uint8_t*>(const_cast<char*>(big.data())), big.size());
        CHECK(sent.has_value());
        CHECK_EQ(*sent, big.size());
        CHECK(co_await drain == big);

        c.shutdown();
        rt->stop();
    }

    void connect_and_echo()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(echo_server(kEchoPort), 0);
        system::co_spawn_static(connect_echo_client(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------ connect refused

    task::Awaitable<void> refused_client(usub::Uvent* rt)
    {
        net::TCPClientSocket c;
        const auto t0 = std::chrono::steady_clock::now();
        const auto err = co_await c.async_connect("127.0.0.1", std::to_string(kRefusedPort), 2000ms);
        CHECK(err.has_value());
        CHECK(*err == ConnectError::ConnectFailed);
        CHECK(std::chrono::steady_clock::now() - t0 < 1500ms); // refused, not timed out
        rt->stop();
    }

    void connect_refused_fails_fast()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(refused_client(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------ connect timeout

    task::Awaitable<void> timeout_client(usub::Uvent* rt)
    {
        net::TCPClientSocket c;
        const auto t0 = std::chrono::steady_clock::now();
        const auto err = co_await c.async_connect("127.0.0.1", std::to_string(kTarpitPort), 300ms);
        const auto dt = std::chrono::steady_clock::now() - t0;
        CHECK(err.has_value());
        CHECK(*err == ConnectError::Timeout);
        CHECK(dt >= 250ms);
        CHECK(dt < 3s);
        rt->stop();
    }

    void connect_timeout_is_bounded()
    {
        Tarpit tarpit{"127.0.0.1", kTarpitPort};
        usub::Uvent rt(2);
        system::co_spawn_static(timeout_client(&rt), 0);
        rt.run();
    }

    // ---------------------------------------------------- connect cancelled

    std::atomic<int> g_cancel_result{-1};

    task::Awaitable<void> cancellable_connect()
    {
        net::TCPClientSocket c;
        const auto err = co_await c.async_connect("127.0.0.1", std::to_string(kTarpitPort), 5000ms);
        g_cancel_result.store(err.has_value() ? static_cast<int>(*err) : -2, std::memory_order_release);
    }

    task::Awaitable<void> cancel_driver(usub::Uvent* rt)
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto h = task::spawn(cancellable_connect());
        co_await system::this_coroutine::sleep_for(100ms);
        h.cancel();
        while (g_cancel_result.load(std::memory_order_acquire) == -1)
            co_await system::this_coroutine::sleep_for(5ms);
        const auto dt = std::chrono::steady_clock::now() - t0;
        const auto r = static_cast<ConnectError>(g_cancel_result.load());
        CHECK(r == ConnectError::Cancelled);
        CHECK(dt < 2s); // well under the 5 s connect timeout
        rt->stop();
    }

    void connect_is_cancellable()
    {
        Tarpit tarpit{"127.0.0.1", kTarpitPort};
        usub::Uvent rt(2);
        system::co_spawn_static(cancel_driver(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------- write backpressure

    constexpr std::size_t kBig = 8u << 20; // 8 MiB: far beyond the loopback send buffer
    std::atomic<bool> g_bp_done{false};

    task::Awaitable<void> slow_reader_server()
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kBackpressurePort};
        auto c = co_await acceptor.async_accept();
        CHECK(c.has_value());
        // Do not read for a while: the client's async_write must park on EPOLLOUT.
        co_await system::this_coroutine::sleep_for(300ms);
        const std::string got = co_await read_exactly(*c, kBig);
        CHECK_EQ(got.size(), kBig);
        CHECK(got == pattern(kBig));
        g_bp_done.store(true, std::memory_order_release);
    }

    task::Awaitable<void> big_writer_client(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms);
        net::TCPClientSocket c;
        std::string data = pattern(kBig);
        CHECK(!(co_await c.async_connect("127.0.0.1", std::to_string(kBackpressurePort))).has_value());
        const auto t0 = std::chrono::steady_clock::now();
        const ssize_t w = co_await c.async_write(reinterpret_cast<uint8_t*>(data.data()), data.size());
        CHECK_EQ(w, static_cast<ssize_t>(kBig));
        // The write could only finish after the reader woke up (>= 300 ms).
        CHECK(std::chrono::steady_clock::now() - t0 >= 250ms);
        co_await wait_flag(g_bp_done);
        c.shutdown();
        rt->stop();
    }

    void write_parks_until_peer_drains()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(slow_reader_server(), 0);
        system::co_spawn_static(big_writer_client(&rt), 0);
        rt.run();
        CHECK(g_bp_done.load());
    }

    // ------------------------------------------------ write timeout (stall)

    task::Awaitable<void> never_reading_server(std::atomic<bool>& release)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kWriteTimeoutPort};
        auto c = co_await acceptor.async_accept();
        CHECK(c.has_value());
        co_await wait_flag(release);
        c->shutdown();
    }

    std::atomic<bool> g_wt_release{false};

    task::Awaitable<void> stalled_writer_client(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms);
        net::TCPClientSocket c;
        CHECK(!(co_await c.async_connect("127.0.0.1", std::to_string(kWriteTimeoutPort))).has_value());
        std::string data = pattern(32u << 20); // build the payload BEFORE arming the timer
        c.set_timeout_ms(300);
        const auto t0 = std::chrono::steady_clock::now();
        const ssize_t w = co_await c.async_write(reinterpret_cast<uint8_t*>(data.data()), data.size());
        const auto dt = std::chrono::steady_clock::now() - t0;
        // The peer never reads: the parked writer is released by the timer, not by progress.
        CHECK(w < static_cast<ssize_t>(data.size()));
        CHECK(dt >= 250ms);
        CHECK(dt < 5s);
        g_wt_release.store(true, std::memory_order_release);
        co_await system::this_coroutine::sleep_for(50ms);
        rt->stop();
    }

    void write_times_out_on_stalled_peer()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(never_reading_server(g_wt_release), 0);
        system::co_spawn_static(stalled_writer_client(&rt), 0);
        rt.run();
    }

    // ------------------------------------- update_timeout keeps a busy link

    constexpr int kTicks = 24;
    std::atomic<bool> g_ka_server_done{false};

    task::Awaitable<void> keepalive_server()
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kKeepAlivePort};
        auto c = co_await acceptor.async_accept();
        CHECK(c.has_value());
        c->set_timeout_ms(250);
        std::size_t total = 0;
        uint8_t buf[64];
        const auto t0 = std::chrono::steady_clock::now();
        auto last_data = t0;
        for (;;)
        {
            const ssize_t r = co_await c->async_read(buf, sizeof(buf));
            if (r <= 0)
                break;
            total += static_cast<std::size_t>(r);
            last_data = std::chrono::steady_clock::now();
            c->update_timeout(250); // activity: push the deadline out again
        }
        const auto now = std::chrono::steady_clock::now();
        CHECK_EQ(total, static_cast<std::size_t>(kTicks));
        // Traffic ran for ~1.2 s with 50 ms gaps: a 250 ms timer that was not
        // refreshed would have fired long before the last byte.
        CHECK(now - t0 >= 1100ms);
        // ...and once the peer went quiet the refreshed timer still fired.
        CHECK(now - last_data >= 200ms);
        CHECK(now - last_data < 2s);
        g_ka_server_done.store(true, std::memory_order_release);
    }

    task::Awaitable<void> ticking_client(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms);
        net::TCPClientSocket c;
        CHECK(!(co_await c.async_connect("127.0.0.1", std::to_string(kKeepAlivePort))).has_value());
        uint8_t byte = 'x';
        for (int i = 0; i < kTicks; ++i)
        {
            CHECK_EQ(co_await c.async_write(&byte, 1), 1);
            co_await system::this_coroutine::sleep_for(50ms);
        }
        co_await wait_flag(g_ka_server_done); // stay silent but connected
        c.shutdown();
        rt->stop();
    }

    void update_timeout_keeps_active_connection()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(keepalive_server(), 0);
        system::co_spawn_static(ticking_client(&rt), 0);
        rt.run();
        CHECK(g_ka_server_done.load());
    }

    // ------------------------------------------------------------ sendfile

    constexpr std::size_t kFileBytes = 64 * 1024;
    std::atomic<bool> g_sf_done{false};

    task::Awaitable<void> sendfile_server()
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kSendfilePort};
        auto c = co_await acceptor.async_accept();
        CHECK(c.has_value());
        const std::string got = co_await read_exactly(*c, 2 * kFileBytes);
        CHECK_EQ(got.size(), 2 * kFileBytes);
        CHECK(got == pattern(kFileBytes) + pattern(kFileBytes));
        g_sf_done.store(true, std::memory_order_release);
    }

    task::Awaitable<void> sendfile_client(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms);
        char path[] = "/tmp/uvent_sendfile_XXXXXX";
        const int fd = ::mkstemp(path);
        CHECK(fd >= 0);
        ::unlink(path);
        const std::string content = pattern(kFileBytes);
        CHECK_EQ(::write(fd, content.data(), content.size()), static_cast<ssize_t>(kFileBytes));

        net::TCPClientSocket c;
        CHECK(!(co_await c.async_connect("127.0.0.1", std::to_string(kSendfilePort))).has_value());
        off_t off = 0;
        std::size_t sent = 0;
        while (sent < kFileBytes)
        {
            const ssize_t r = co_await c.async_sendfile(fd, &off, kFileBytes - sent);
            CHECK(r > 0);
            sent += static_cast<std::size_t>(r);
        }
        CHECK_EQ(off, static_cast<off_t>(kFileBytes));
        off = 0;
        sent = 0;
        while (sent < kFileBytes)
        {
            const ssize_t r = c.sendfile(fd, &off, kFileBytes - sent); // synchronous variant
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                co_await system::this_coroutine::sleep_for(5ms);
                continue;
            }
            CHECK(r > 0);
            sent += static_cast<std::size_t>(r);
        }
        ::close(fd);
        co_await wait_flag(g_sf_done);
        c.shutdown();
        rt->stop();
    }

    void sendfile_async_and_sync()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(sendfile_server(), 0);
        system::co_spawn_static(sendfile_client(&rt), 0);
        rt.run();
        CHECK(g_sf_done.load());
    }

    // ------------------------------------------------------ happy eyeballs

    task::Awaitable<void> happy_client(usub::Uvent* rt)
    {
        co_await system::this_coroutine::sleep_for(20ms);
        {
            auto s = co_await net::connect_happy("127.0.0.1", std::to_string(kHappyPort));
            CHECK(s.has_value());
            const std::string msg = "happy";
            CHECK_EQ(co_await s->async_write(reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data())), msg.size()),
                     static_cast<ssize_t>(msg.size()));
            CHECK(co_await read_exactly(*s, msg.size()) == msg);
            s->shutdown();
        }
        {
            // First address is a tarpit: the second attempt, started after
            // attempt_delay, must win.
            net::HappyEyeballsOptions opts;
            opts.attempt_delay = 100ms;
            opts.attempt_timeout = 3000ms;
            std::vector<net::ResolvedAddr> addrs{{"127.0.0.2", utils::net::IPV4}, {"127.0.0.1", utils::net::IPV4}};
            const auto t0 = std::chrono::steady_clock::now();
            auto s = co_await net::connect_happy_addrs(addrs, std::to_string(kHappyPort), opts);
            CHECK(s.has_value());
            CHECK(std::chrono::steady_clock::now() - t0 < 2500ms);
            const std::string msg = "eyeballs";
            CHECK_EQ(co_await s->async_write(reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data())), msg.size()),
                     static_cast<ssize_t>(msg.size()));
            CHECK(co_await read_exactly(*s, msg.size()) == msg);
            s->shutdown();
        }
        {
            // Every address is unreachable: a bounded failure, not a hang.
            net::HappyEyeballsOptions opts;
            opts.attempt_delay = 50ms;
            opts.attempt_timeout = 300ms;
            std::vector<net::ResolvedAddr> addrs{{"127.0.0.2", utils::net::IPV4}};
            const auto t0 = std::chrono::steady_clock::now();
            auto s = co_await net::connect_happy_addrs(addrs, std::to_string(kHappyPort), opts);
            CHECK(!s.has_value());
            CHECK(std::chrono::steady_clock::now() - t0 < 3s);
        }
        rt->stop();
    }

    void happy_eyeballs_connects_and_falls_back()
    {
        Tarpit tarpit{"127.0.0.2", kHappyPort};
        usub::Uvent rt(2);
        system::co_spawn_static(echo_server(kHappyPort), 0);
        system::co_spawn_static(happy_client(&rt), 0);
        rt.run();
    }
} // namespace

int main()
{
    return run_tests({
        {"connect_and_echo", connect_and_echo},
        {"connect_refused_fails_fast", connect_refused_fails_fast},
        {"connect_timeout_is_bounded", connect_timeout_is_bounded},
#ifdef UVENT_ENABLE_REUSEADDR
        {"connect_is_cancellable", connect_is_cancellable}, // needs the cancel kick
#endif
        {"write_parks_until_peer_drains", write_parks_until_peer_drains},
        {"write_times_out_on_stalled_peer", write_times_out_on_stalled_peer},
        {"update_timeout_keeps_active_connection", update_timeout_keeps_active_connection},
        {"sendfile_async_and_sync", sendfile_async_and_sync},
        {"happy_eyeballs_connects_and_falls_back", happy_eyeballs_connects_and_falls_back},
    });
}

#else

int main()
{
    std::printf("skipped: socket client tests need UVENT_ENABLE_REUSEADDR=ON\n");
    return 0;
}

#endif
