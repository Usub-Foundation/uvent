// Cross-worker socket benchmark.
//
// One acceptor on worker 0 hands every accepted connection to a handler
// coroutine on another worker (UVENT_XW_MODE=foreign, default) or keeps it on
// worker 0 (UVENT_XW_MODE=local, the reference). In the foreign mode every
// poller event and socket timeout is raised on worker 0 (the fd's owner) for a
// coroutine that belongs to worker 1..N-1, so each wake takes the cross-worker
// path. Drive it with: wrk -t8 -c500 -d10s http://127.0.0.1:24900/
//
//   UVENT_XW_WORKERS  runtime size (default 4)
//   UVENT_XW_PORT     listen port  (default 24900, keep it below 32768)
//   UVENT_XW_MODE     foreign | local
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "uvent/Uvent.h"

using namespace usub::uvent;

namespace
{
    task::Awaitable<void> client_coro(net::TCPClientSocket socket)
    {
        static constexpr size_t max_read_size = 64 * 1024;
        static constexpr std::string_view response = "HTTP/1.1 200 OK\r\n"
                                                     "Content-Type: application/json\r\n"
                                                     "Content-Length: 20\r\n"
                                                     "\r\n"
                                                     "{\"status\":\"success\"}";
        utils::DynamicBuffer buffer;
        buffer.reserve(max_read_size);
        socket.set_timeout_ms(5000);
        for (;;)
        {
            buffer.clear();
            const ssize_t rd = co_await socket.async_read(buffer, max_read_size);
            if (rd <= 0)
            {
                socket.shutdown();
                break;
            }
            socket.update_timeout(5000);
            const ssize_t wr = co_await socket.async_write(
                const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(response.data())), response.size());
            if (wr <= 0)
                break;
        }
    }

    task::Awaitable<void> acceptor_coro(int port, int workers, bool foreign)
    {
        net::TCPServerSocket acceptor{"0.0.0.0", port};
        std::size_t n = 0;
        for (;;)
        {
            auto client = co_await acceptor.async_accept();
            if (!client.has_value())
                continue;
            const int tid = (foreign && workers > 1) ? 1 + static_cast<int>(n++ % static_cast<std::size_t>(workers - 1))
                                                     : 0;
            task::spawn(client_coro(std::move(*client)), tid);
        }
    }

    long env_long(const char* name, long def)
    {
        const char* v = std::getenv(name);
        return v ? std::atol(v) : def;
    }
} // namespace

int main()
{
    const int workers = static_cast<int>(env_long("UVENT_XW_WORKERS", 4));
    const int port = static_cast<int>(env_long("UVENT_XW_PORT", 24900));
    const char* mode = std::getenv("UVENT_XW_MODE");
    const bool foreign = !(mode && std::strcmp(mode, "local") == 0);
    settings::timeout_duration_ms = 5000;

    usub::Uvent rt(workers);
    system::co_spawn_static(acceptor_coro(port, workers, foreign), 0);
    std::printf("xworker bench: workers=%d port=%d mode=%s\n", workers, port, foreign ? "foreign" : "local");
    rt.run();
    return 0;
}
