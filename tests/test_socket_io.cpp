// Socket I/O paths that the runtime tests never touched: synchronous
// receive()/read()/write(), the inactivity timeout, and the owner-forwarding
// ops (timeout / shutdown / destroy issued from a worker that does not own
// the socket). Everything runs on loopback with a plain blocking client thread.
#include <cerrno>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/utils/buffer/DynamicBuffer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The legacy shared-poller layout (UVENT_ENABLE_REUSEADDR=OFF) still hangs in
// socket timeout / connect scenarios (see docs/build-flags.md); these tests
// target the owner-forwarding layout.
#ifdef UVENT_ENABLE_REUSEADDR
using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    // Listeners must stay below net.ipv4.ip_local_port_range (32768).
    constexpr uint16_t kReceivePort = 24401;
    constexpr uint16_t kSyncRwPort = 24402;
    constexpr uint16_t kTimeoutPort = 24403;
    constexpr uint16_t kForeignPort = 24404;
    constexpr uint16_t kZeroWritePort = 24406;

    constexpr std::size_t kPayload = 10000;

    void send_all(int fd, const std::string& s)
    {
        std::size_t off = 0;
        while (off < s.size())
        {
            const ssize_t w = ::send(fd, s.data() + off, s.size() - off, 0);
            CHECK(w > 0);
            off += static_cast<std::size_t>(w);
        }
    }

    std::string recv_exactly(int fd, std::size_t n)
    {
        std::string out(n, '\0');
        std::size_t off = 0;
        while (off < n)
        {
            const ssize_t r = ::recv(fd, out.data() + off, n - off, 0);
            CHECK(r > 0);
            off += static_cast<std::size_t>(r);
        }
        return out;
    }

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

    // ---------------------------------------------------------------- receive()

    std::atomic<bool> g_recv_sent{false};
    std::atomic<bool> g_recv_done{false};
    std::atomic<bool> g_recv_closed{false};

    task::Awaitable<void> receive_server(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kReceivePort};
        auto client = co_await acceptor.async_accept();
        CHECK(client.has_value());
        co_await wait_flag(g_recv_sent);
        co_await system::this_coroutine::sleep_for(50ms); // let loopback deliver everything

        // maxSize caps the result exactly, and the bytes past the cap are NOT consumed.
        auto first = client->receive(1024, 4000);
        CHECK(first.has_value());
        CHECK_EQ(first->size(), 4000u);
        CHECK(*first == pattern(kPayload).substr(0, 4000));

        // The remainder is still in the socket: nothing was dropped at the cap.
        auto rest = client->receive(1024, 1 << 20);
        CHECK(rest.has_value());
        CHECK_EQ(rest->size(), kPayload - 4000);
        CHECK(*rest == pattern(kPayload).substr(4000));

        // No data pending: EAGAIN ends the loop with an empty success.
        auto empty = client->receive(1024, 1 << 20);
        CHECK(empty.has_value());
        CHECK(empty->empty());

        g_recv_done.store(true, std::memory_order_release);
        co_await wait_flag(g_recv_closed);
        co_await system::this_coroutine::sleep_for(50ms);

        // Peer closed: recv() == 0 ends the loop, still a success.
        auto eof = client->receive(1024, 1 << 20);
        CHECK(eof.has_value());
        CHECK(eof->empty());
        rt->stop();
    }

    void receive_caps_without_losing_bytes()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(receive_server(&rt), 0);
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kReceivePort);
                send_all(fd, pattern(kPayload));
                g_recv_sent.store(true, std::memory_order_release);
                while (!g_recv_done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(5ms);
                ::close(fd);
                g_recv_closed.store(true, std::memory_order_release);
            });
        rt.run();
        client.join();
    }

    // ------------------------------------------------------ sync read()/write()

    std::atomic<bool> g_rw_sent{false};

    task::Awaitable<void> sync_rw_server(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kSyncRwPort};
        auto client = co_await acceptor.async_accept();
        CHECK(client.has_value());
        co_await wait_flag(g_rw_sent);
        co_await system::this_coroutine::sleep_for(50ms);

        utils::DynamicBuffer buf;
        const ssize_t n = client->read(buf, 1 << 16);
        CHECK_EQ(n, static_cast<ssize_t>(kPayload));
        CHECK_EQ(buf.size(), kPayload);
        CHECK(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()) == pattern(kPayload));

        const ssize_t w = client->write(buf.data(), buf.size());
        CHECK_EQ(w, static_cast<ssize_t>(kPayload));

        // Empty writes are a no-op success on both paths.
        CHECK_EQ(client->write(buf.data(), 0), 0);
        const ssize_t aw = co_await client->async_write(buf.data(), 0);
        CHECK_EQ(aw, 0);
        rt->stop();
    }

    void sync_read_write_roundtrip()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(sync_rw_server(&rt), 0);
        std::string back;
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kSyncRwPort);
                send_all(fd, pattern(kPayload));
                g_rw_sent.store(true, std::memory_order_release);
                back = recv_exactly(fd, kPayload);
                ::close(fd);
            });
        rt.run();
        client.join();
        CHECK(back == pattern(kPayload));
    }

    // ------------------------------------------------------------ read timeout

    std::atomic<bool> g_timeout_hit{false};

    task::Awaitable<void> timeout_server(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kTimeoutPort};
        auto client = co_await acceptor.async_accept();
        CHECK(client.has_value());
        client->set_timeout_ms(150);
        uint8_t buf[64];
        const auto t0 = std::chrono::steady_clock::now();
        const ssize_t r = co_await client->async_read(buf, sizeof(buf));
        const auto dt = std::chrono::steady_clock::now() - t0;
        // The peer never writes: only the inactivity timer can wake us.
        CHECK(r <= 0);
        CHECK(dt >= 100ms);
        CHECK(dt < 5s);
        g_timeout_hit.store(true, std::memory_order_release);
        rt->stop();
    }

    void read_timeout_wakes_silent_reader()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(timeout_server(&rt), 0);
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kTimeoutPort);
                while (!g_timeout_hit.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(5ms);
                ::close(fd);
            });
        rt.run();
        client.join();
        CHECK(g_timeout_hit.load());
    }

    // ------------------------------------------- ops forwarded to the owner

    std::atomic<bool> g_foreign_done{false};

    // Runs on worker 1 with a socket registered on worker 0's poller: the
    // timeout, the shutdown and the final destroy all take the forwarding path.
    task::Awaitable<void> foreign_consumer(net::TCPClientSocket client)
    {
        client.set_timeout_ms(150);
        uint8_t buf[64];
        const auto t0 = std::chrono::steady_clock::now();
        const ssize_t r = co_await client.async_read(buf, sizeof(buf));
        CHECK(r <= 0);
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
        client.set_timeout_ms(1000); // re-arm through the forwarding path as well
        client.shutdown();
        g_foreign_done.store(true, std::memory_order_release);
        // `client` is destroyed here, on worker 1: Destroy is forwarded to worker 0.
    }

    task::Awaitable<void> foreign_server(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kForeignPort};
        auto client = co_await acceptor.async_accept();
        CHECK(client.has_value());
        task::spawn(foreign_consumer(std::move(*client)), 1);
        co_await wait_flag(g_foreign_done);
        co_await system::this_coroutine::sleep_for(100ms); // let the forwarded ops land
        rt->stop();
    }

    void foreign_worker_timeout_shutdown_destroy()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(foreign_server(&rt), 0);
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kForeignPort);
                while (!g_foreign_done.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(5ms);
                ::close(fd);
            });
        rt.run();
        client.join();
        CHECK(g_foreign_done.load());
    }

    // ----------------------------------------------------- zero-length I/O

    task::Awaitable<void> zero_io_server(usub::Uvent* rt)
    {
        net::TCPServerSocket acceptor{"127.0.0.1", kZeroWritePort};
        auto client = co_await acceptor.async_accept();
        CHECK(client.has_value());
        uint8_t byte = 0;
        CHECK_EQ(co_await client->async_read(&byte, 0), 0);
        CHECK_EQ(co_await client->async_read(nullptr, 8), 0);
        CHECK_EQ(co_await client->async_write(&byte, 0), 0);
        CHECK_EQ(co_await client->async_write(nullptr, 8), 0);
        rt->stop();
    }

    void zero_length_io_is_a_noop()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(zero_io_server(&rt), 0);
        std::thread client(
            [&]
            {
                int fd = connect_blocking(kZeroWritePort);
                std::this_thread::sleep_for(200ms);
                ::close(fd);
            });
        rt.run();
        client.join();
    }
} // namespace

int main()
{
    return run_tests({
        {"receive_caps_without_losing_bytes", receive_caps_without_losing_bytes},
        {"sync_read_write_roundtrip", sync_read_write_roundtrip},
        {"read_timeout_wakes_silent_reader", read_timeout_wakes_silent_reader},
        {"foreign_worker_timeout_shutdown_destroy", foreign_worker_timeout_shutdown_destroy},
        {"zero_length_io_is_a_noop", zero_length_io_is_a_noop},
    });
}

#else

int main()
{
    std::printf("skipped: socket I/O tests need UVENT_ENABLE_REUSEADDR=ON\n");
    return 0;
}

#endif
