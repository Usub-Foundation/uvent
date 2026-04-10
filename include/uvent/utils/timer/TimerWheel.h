//
// Created by kirill on 8/27/24.
//

#ifndef UVENT_TIMER_H
#define UVENT_TIMER_H

#include "uvent/system/Defines.h"
#include "uvent/tasks/AwaitableFrame.h"
#include "Timer.h"

#include <unordered_map>
#include <functional>
#include <coroutine>
#include <iostream>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif
#include <cstdint>
#include <chrono>
#include <vector>
#include <mutex>
#include <cmath>
#include <map>
#include "uvent/utils/datastructures/queue/ConcurrentQueues.h"
#include "uvent/system/Settings.h"

typedef uint64_t timer_duration_t;
typedef uint64_t timeout_t;

namespace usub::uvent::utils
{
    class TimerWheel
    {
    public:
        explicit TimerWheel();

        uint64_t addTimer(Timer* timer);

        bool updateTimer(uint64_t timerId, timer_duration_t new_duration);

        bool removeTimer(uint64_t timerId);

        void tick();

        int getNextTimeout() const;

        bool empty() const;

    public:
#ifndef UVENT_ENABLE_REUSEADDR
        std::mutex mtx;
#endif

    private:
        static timeout_t getCurrentTime();

        void addTimerToWheel(Timer* timer, timeout_t expiryTime);

        void removeTimerFromWheel(Timer* timer);

        void advance();

        void updateNextExpiryTime();

        inline static bool is_due(timeout_t now, timeout_t expiry, uint64_t interval) noexcept
        {
            if (expiry <= now) return true;
            return (expiry - now) < interval;
        }

    private:
        struct Wheel
        {
            Wheel(size_t slots, uint64_t interval)
                : slots_(slots), interval_(interval), currentSlot_(0)
            {
                buckets_.resize(slots_);
                for (auto& b : buckets_)
                    b.reserve(2);
            }

            size_t slots_;
            uint64_t interval_;
            size_t currentSlot_;
            std::vector<std::vector<Timer*>> buckets_;
        };

        std::vector<Wheel>                   wheels_;
        timeout_t                            currentTime_;
        std::unordered_map<uint64_t, Timer*> timerMap_;
#ifndef UVENT_ENABLE_REUSEADDR
        std::atomic<uint64_t>                timerIdCounter_{0};
#else
        uint64_t                             timerIdCounter_{0};
#endif
        timeout_t                            nextExpiryTime_;
        size_t                               activeTimerCount_;
#ifndef UVENT_ENABLE_REUSEADDR
        queue::concurrent::MPMCQueue<Op>     timer_operations_queue;
#else
        queue::single_thread::Queue<Op>      timer_operations_queue;
#endif
        std::vector<Op>                      ops_;
    };
}

#endif //UVENT_TIMER_H
