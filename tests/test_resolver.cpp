// async_resolve() through the resolver thread pool. AI_NUMERICHOST keeps both
// cases local: no DNS traffic, the negative case fails inside getaddrinfo().
#include <cstring>

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/net/Resolver.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    task::Awaitable<void> resolve_body(usub::Uvent* rt)
    {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

        auto ok = co_await net::async_resolve("127.0.0.1", "8080", hints);
        CHECK(ok.has_value());
        CHECK((*ok)->ai_family == AF_INET);
        auto* sin = reinterpret_cast<sockaddr_in*>((*ok)->ai_addr);
        CHECK_EQ(ntohl(sin->sin_addr.s_addr), INADDR_LOOPBACK);
        CHECK_EQ(ntohs(sin->sin_port), 8080);

        auto bad = co_await net::async_resolve("no.such.host.invalid", "8080", hints);
        CHECK(!bad.has_value());
        CHECK(bad.error() != 0);

        // Several requests in flight at once share the worker pool.
        int done = 0;
        for (int i = 0; i < 8; ++i)
        {
            auto r = co_await net::async_resolve("127.0.0.1", std::to_string(1000 + i), hints);
            CHECK(r.has_value());
            ++done;
        }
        CHECK_EQ(done, 8);
        rt->stop();
    }

    void numeric_host_resolves_and_bad_host_fails()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(resolve_body(&rt), 0);
        auto t0 = std::chrono::steady_clock::now();
        rt.run();
        CHECK(std::chrono::steady_clock::now() - t0 < 10s);
    }
} // namespace

int main()
{
    return run_tests({
        {"numeric_host_resolves_and_bad_host_fails", numeric_host_resolves_and_bad_host_fails},
    });
}
