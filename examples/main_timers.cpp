//
// Created by root on 12/6/25.
//
#include <chrono>
#include <iomanip>
#include <iostream>
#include <source_location>
#include <sstream>
#include <string>
#include "uvent/Uvent.h"
#include "uvent/sync/AsyncCancellation.h"
#include "uvent/sync/AsyncEvent.h"
#include "uvent/sync/AsyncMutex.h"
#include "uvent/sync/AsyncSemaphore.h"
#include "uvent/sync/AsyncWaitGroup.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

inline std::string current_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

inline std::string make_location_string(const std::source_location& loc = std::source_location::current())
{
    return "[" + current_timestamp() + "] " + std::string(loc.file_name()) + "(" + std::to_string(loc.line()) + ":" +
        std::to_string(loc.column()) + ") `" + loc.function_name() + "`";
}

void function(std::any value)
{
    std::cout << make_location_string() << ": function res = " << std::any_cast<int>(value) << std::endl;
}

task::Awaitable<void> coroutine(int a)
{
    std::cout << make_location_string() << ": coroutine res = " << a << std::endl;
    co_return;
}

task::Awaitable<void> coroutine_non_arg()
{
    std::cout << make_location_string() << ": non_arg coroutine" << std::endl;
    co_return;
}

task::Awaitable<void> function_timer()
{
    auto timer = new utils::Timer(std::chrono::milliseconds(2000).count());
    timer->addFunction(function, 1);

    system::spawn_timer(timer);
    co_return;
}

task::Awaitable<void> coroutine_timer_arg()
{
    auto timer = new utils::Timer(std::chrono::milliseconds(2000).count());
    timer->addCoroutine(coroutine(2));

    system::spawn_timer(timer);
    co_return;
}

task::Awaitable<void> coroutine_timer()
{
    auto timer = new utils::Timer(std::chrono::milliseconds(2000).count());
    timer->addCoroutine(coroutine_non_arg());

    system::spawn_timer(timer);
    co_return;
}

task::Awaitable<void> coroutine_sleep()
{
    std::cout << make_location_string() << ": coroutine_sleep before" << std::endl;

    co_await system::this_coroutine::sleep_for(1000ms);

    std::cout << make_location_string() << ": coroutine_sleep after" << std::endl; // diff 1 second
    co_return;
}

int main()
{
    settings::timeout_duration_ms = 5000;
#ifdef UVENT_DEBUG
    std::cout << "UVENT_DEBUG enabled" << std::endl;
#endif

    usub::Uvent uvent(4);

    system::co_spawn_static(function_timer(), 0);
    system::co_spawn_static(coroutine_timer(), 1);
    system::co_spawn_static(coroutine_timer_arg(), 2);
    system::co_spawn_static(coroutine_sleep(), 3);

    uvent.run();
    return 0;
}
