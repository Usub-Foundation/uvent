#include "uvent/Uvent.h"
#include "uvent/sync/AsyncBarrier.h"
#include "uvent/sync/AsyncCancellation.h"
#include "uvent/sync/AsyncEvent.h"
#include "uvent/sync/AsyncMutex.h"
#include "uvent/sync/AsyncSemaphore.h"
#include "uvent/sync/AsyncWaitGroup.h"

using namespace usub::uvent;

task::Awaitable<void> clientCoro(net::TCPClientSocket socket)
{
    static constexpr size_t max_read_size = 64 * 1024;
    utils::DynamicBuffer buffer;
    buffer.reserve(max_read_size);

    static const std::string_view httpResponse = "HTTP/1.1 200 OK\r\n"
                                                 "Content-Type: application/json\r\n"
                                                 "Content-Length: 20\r\n"
                                                 "\r\n"
                                                 "{\"status\":\"success\"}";

    socket.set_timeout_ms(5000);
    while (true)
    {
        buffer.clear();
        ssize_t rdsz = co_await socket.async_read(buffer, max_read_size);
        socket.update_timeout(5000);
#ifdef UVENT_DEBUG
        spdlog::info("Read size: {}", rdsz);
        std::string s(buffer.data(), buffer.data() + buffer.size());
        spdlog::info("Received string (raw): {}", s);
#endif
        if (rdsz <= 0)
        {
#ifdef UVENT_DEBUG
            spdlog::info("Client disconnected");
#endif
            socket.shutdown();
            break;
        }
        size_t wrsz = co_await socket.async_write(
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(httpResponse.data())), httpResponse.size());
#ifdef UVENT_DEBUG
        spdlog::warn("Write size: {}", wrsz);
#endif
        if (wrsz <= 0)
            break;
        socket.update_timeout(5000);
    }
#ifdef UVENT_DEBUG
    spdlog::warn("client_coro finished");
#endif
    co_return;
}

task::Awaitable<void> test_coro()
{
    using namespace std::chrono_literals;
    std::cout << "test_coro()" << std::endl;
    co_await system::this_coroutine::sleep_for(2000ms);
    std::cout << "test_coro() 2" << std::endl;
    co_return;
}

task::Awaitable<void> listeningCoro()
{
    auto acceptor = new net::TCPServerSocket{"0.0.0.0", 45900};
    for (;;)
        co_await acceptor->async_accept(clientCoro);
}

task::Awaitable<void> sendingCoro()
{
#if UVENT_DEBUG
    spdlog::warn("sending coro");
#endif
    auto socket = net::TCPClientSocket{};
    auto res = co_await socket.async_connect("example.com", "80");
    if (res.has_value())
    {
        if (*res == usub::utils::errors::ConnectError::Timeout)
        {
#if UVENT_DEBUG
            spdlog::warn("sendingCoro: got expected ConnectError::Timeout");
#endif
        }
        co_return;
    }

#if UVENT_DEBUG
    spdlog::warn("connect success");
#endif

    uint8_t buffer[] = "GET / HTTP/1.1\r\n"
                       "Host: example.com\r\n"
                       "User-Agent: test-client\r\n"
                       "Accept: */*\r\n"
                       "Connection: close\r\n\r\n";

    auto result = co_await socket.async_send(buffer, sizeof(buffer) - 1);
    if (!result.has_value())
        co_return;

#if UVENT_DEBUG
    spdlog::warn("Success async_send: {} bytes", result.value());
#endif

    static constexpr size_t max_read_size = 64 * 1024;
    utils::DynamicBuffer read_buffer;
    read_buffer.reserve(max_read_size);

    while (true)
    {
        auto r = co_await socket.async_read(read_buffer, max_read_size);
        if (r <= 0 || read_buffer.size() >= max_read_size)
            break;
    }

#if UVENT_DEBUG
    spdlog::warn("RESPONSE BEGIN\n{}\nRESPONSE END",
                 std::string(reinterpret_cast<const char*>(read_buffer.data()), read_buffer.size()));
#endif
    co_await test_coro();
    co_return;
}

task::Awaitable<void> sendingCoroTimeout()
{
    using namespace std::chrono_literals;

#if UVENT_DEBUG
    spdlog::warn("sendingCoroTimeout: expect ConnectError::Timeout");
#endif

    net::TCPClientSocket socket;

    std::string host = "example.com";
    std::string port = "81";

    auto res = co_await socket.async_connect(host, port, 500ms);

    if (!res.has_value())
    {
#if UVENT_DEBUG
        spdlog::error("sendingCoroTimeout: connect unexpectedly succeeded (no timeout)");
#endif
        co_return;
    }

    if (*res == usub::utils::errors::ConnectError::Timeout)
    {
#if UVENT_DEBUG
        spdlog::warn("sendingCoroTimeout: got expected ConnectError::Timeout");
#endif
        co_return;
    }

#if UVENT_DEBUG
    spdlog::error("sendingCoroTimeout: connect failed with unexpected error={}", static_cast<int>(*res));
#endif
    co_return;
}

task::Awaitable<int, detail::AwaitableFrame<int>> generator()
{
    std::cout << "Thread count: " << system::global::detail::thread_count << std::endl;
    ;
    for (int i = 1; i <= 3; ++i)
    {
        std::cout << "yield " << i << "\n";
        co_yield i;
    }
    co_return 0;
}

task::Awaitable<void, detail::AwaitableFrame<void>> consumer()
{
    auto g = generator();
    while (true)
    {
        int v = co_await g;
        std::cout << "got " << v << "\n";
        if (g.get_promise()->get_coroutine_handle().done())
            break;
    }
    co_return;
}

static usub::uvent::sync::AsyncMutex g_mutex;

task::Awaitable<void> critical_task(int id)
{
    {
        auto guard = co_await g_mutex.lock();
        std::cout << "task " << id << " entered critical section\n";
        co_await system::this_coroutine::sleep_for(std::chrono::milliseconds(500));
        std::cout << "task " << id << " leaving critical section\n";
    }
    co_return;
}

usub::uvent::sync::AsyncSemaphore g_sem{2};
usub::uvent::sync::AsyncEvent g_evt{usub::uvent::sync::Reset::Manual, false};
usub::uvent::sync::WaitGroup g_wg;
usub::uvent::sync::CancellationSource g_cancel_src;
static usub::uvent::sync::AsyncBarrier g_bar{4};

task::Awaitable<void> barrier_worker()
{
    int id = std::coroutine_handle<detail::AwaitableFrameBase>::from_address(system::this_thread::detail::cec.address())
                 .promise()
                 .get_thread_id();
    std::cout << "w" << id << " before barrier\n";
    co_await g_bar.arrive_and_wait();
    std::cout << "w" << id << " after barrier\n";
    co_return;
}

task::Awaitable<void> semaphore_task(int id)
{
    co_await g_sem.acquire();
    std::cout << "[sem] task " << id << " acquired\n";
    co_await system::this_coroutine::sleep_for(std::chrono::milliseconds(300));
    std::cout << "[sem] task " << id << " released\n";
    g_sem.release();
    g_wg.done();
    co_return;
}

task::Awaitable<void> event_waiter(int id)
{
    std::cout << "[evt] waiter " << id << " waiting\n";
    co_await g_evt.wait();
    std::cout << "[evt] waiter " << id << " woke up\n";
    co_return;
}

task::Awaitable<void> set_event_after_1s()
{
    using namespace std::chrono_literals;
    co_await system::this_coroutine::sleep_for(1s);
    std::cout << "[evt] set manual event\n";
    g_evt.set();
    co_return;
}

task::Awaitable<void> cancellation_task(sync::CancellationToken tok)
{
    int ticks = 0;
    while (!tok.stop_requested())
    {
        ++ticks;
        co_await system::this_coroutine::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "[cancel] canceled after " << ticks << " ticks\n";
    for (int i = 0; i < 4; i++)
    {
        auto barrier_worker_c = barrier_worker();
        barrier_worker_c.get_promise()->set_thread_id(i);
        system::co_spawn_static(barrier_worker_c, i);
    }
    co_return;
}

task::Awaitable<void> cancel_after_1500ms()
{
    using namespace std::chrono_literals;
    co_await system::this_coroutine::sleep_for(1500ms);
    std::cout << "[cancel] request_cancel()\n";
    g_cancel_src.request_cancel();
    co_return;
}

task::Awaitable<void> wg_waiter()
{
    co_await g_wg.wait();
    std::cout << "[wg] all semaphore tasks done\n";
    co_return;
}

int main()
{
    settings::timeout_duration_ms = 5000;
    settings::per_thread_socket_header_pool_size = 60000;
#ifdef UVENT_DEBUG
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [thread %t] [%l] %v%$");
    spdlog::set_level(spdlog::level::trace);
#endif

    usub::Uvent uvent(4);
    uvent.for_each_thread([&](int threadIndex, thread::ThreadLocalStorage* tls)
                          { system::co_spawn_static(listeningCoro(), threadIndex); });

    system::co_spawn(sendingCoro());
    system::co_spawn(sendingCoroTimeout());
    system::co_spawn(consumer());
    system::co_spawn(critical_task(1));
    system::co_spawn(critical_task(2));
    system::co_spawn(critical_task(3));

    g_wg.add(4);
    system::co_spawn(semaphore_task(0));
    system::co_spawn(semaphore_task(1));
    system::co_spawn(semaphore_task(2));
    system::co_spawn(semaphore_task(3));

    system::co_spawn(event_waiter(1));
    system::co_spawn(event_waiter(2));
    system::co_spawn(set_event_after_1s());

    auto tok = g_cancel_src.token();
    system::co_spawn(cancellation_task(tok));
    system::co_spawn(cancel_after_1500ms());

    system::co_spawn(wg_waiter());

    uvent.run();
    return 0;
}
