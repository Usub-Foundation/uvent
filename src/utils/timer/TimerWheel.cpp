//
// Created by kirill on 8/27/24.
//

#include "uvent/utils/timer/TimerWheel.h"

#include <algorithm>

namespace usub::uvent::utils
{
    TimerWheel::TimerWheel()
        : currentTime_(getCurrentTime()), timerIdCounter_(0),
          nextExpiryTime_(0), activeTimerCount_(0)
    {
        /**
         @brief by default used 4 levels:
         LEVEL 0: 256 slots, interval 1 ms
         LEVEL 1: 256 slots, interval 256 ms
         LEVEL 2: 256 slots, interval 65,536 ms
         LEVEL 3: 256 slots, interval 16,777,216 ms
        */
        for (int i = 0; i < settings::tw_levels; i++)
            this->wheels_.emplace_back(256, (1ull << (8 * i)));
        this->ops_.resize(settings::max_pre_allocated_timer_wheel_operations_items);
    }

    uint64_t TimerWheel::addTimer(Timer* timer)
    {
        timer->expiryTime = getCurrentTime() + timer->duration_ms;
#ifndef UVENT_ENABLE_REUSEADDR
        timer->id = timerIdCounter_.fetch_add(1, std::memory_order_relaxed) + 1;
#else
        timer->id = ++this->timerIdCounter_;
#endif

        Op op{ .op = OpType::ADD, .timer = timer };
#ifndef UVENT_ENABLE_REUSEADDR
        while (!this->timer_operations_queue.try_enqueue(op)) cpu_relax();
#else
        this->timer_operations_queue.enqueue(op);
#endif
        return timer->id;
    }

    bool TimerWheel::updateTimer(uint64_t timerId, timer_duration_t new_duration)
    {
        Op op{ .op = OpType::UPDATE, .id = timerId, .new_dur = new_duration };
#ifndef UVENT_ENABLE_REUSEADDR
        while (!this->timer_operations_queue.try_enqueue(op)) cpu_relax();
#else
        this->timer_operations_queue.enqueue(op);
#endif
        return true;
    }

    bool TimerWheel::removeTimer(uint64_t timerId)
    {
        Op op{ .op = OpType::REMOVE, .id_only = timerId };
#ifndef UVENT_ENABLE_REUSEADDR
        while (!this->timer_operations_queue.try_enqueue(op)) cpu_relax();
#else
        this->timer_operations_queue.enqueue(op);
#endif
        return true;
    }

    int TimerWheel::getNextTimeout() const
    {
        const timeout_t now  = getCurrentTime();
        const timeout_t next = this->nextExpiryTime_;

        if (next == 0) return -1;
        if (next <= now) return 0;

        uint64_t diff = next - now;
        if (diff > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return std::numeric_limits<int>::max();

        return static_cast<int>(diff);
    }

    timeout_t TimerWheel::getCurrentTime()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
                   steady_clock::now().time_since_epoch())
            .count();
    }

    void TimerWheel::addTimerToWheel(Timer* timer, timeout_t expiryTime)
    {
        uint64_t diff  = expiryTime - this->currentTime_;
        size_t   level = 0;

        while (level < this->wheels_.size())
        {
            Wheel& wheel = this->wheels_[level];
            if (diff < wheel.interval_ * wheel.slots_)
            {
                size_t ticks = diff / wheel.interval_;
                size_t slot  = (wheel.currentSlot_ + ticks) % wheel.slots_;
                wheel.buckets_[slot].push_back(timer);

                timer->level     = level;
                timer->slotIndex = slot;

                if (this->nextExpiryTime_ == 0 || expiryTime < this->nextExpiryTime_)
                    this->nextExpiryTime_ = expiryTime;

                return;
            }
            ++level;
        }

        Wheel& lastWheel = this->wheels_.back();
        lastWheel.buckets_.back().push_back(timer);
        timer->level     = this->wheels_.size() - 1;
        timer->slotIndex = lastWheel.buckets_.size() - 1;

        if (this->nextExpiryTime_ == 0 || expiryTime < this->nextExpiryTime_)
            this->nextExpiryTime_ = expiryTime;
    }

    void TimerWheel::removeTimerFromWheel(Timer* timer)
    {
        if (timer->level >= this->wheels_.size())
            return;

        Wheel& wheel  = this->wheels_[timer->level];
        auto&  bucket = wheel.buckets_[timer->slotIndex];

        auto it = std::find(bucket.begin(), bucket.end(), timer);
        if (it != bucket.end())
        {
            *it = bucket.back();
            bucket.pop_back();
        }

        if (timer->expiryTime == this->nextExpiryTime_)
            this->nextExpiryTime_ = 0;
    }

    void TimerWheel::updateNextExpiryTime()
    {
        this->nextExpiryTime_ = 0;
        for (const auto& wheel : this->wheels_)
        {
            for (const auto& bucket : wheel.buckets_)
            {
                for (const auto* t : bucket)
                {
                    if (!t->active) continue;
                    if (this->nextExpiryTime_ == 0 || t->expiryTime < this->nextExpiryTime_)
                        this->nextExpiryTime_ = t->expiryTime;
                }
            }
        }
    }

    void TimerWheel::tick()
    {
        for (;;)
        {
            const size_t cap = this->ops_.size();
#ifndef UVENT_ENABLE_REUSEADDR
            size_t n = this->timer_operations_queue.try_dequeue_bulk(
                this->ops_.data(), cap);
#else
            const size_t n = this->timer_operations_queue.dequeue_bulk(
                this->ops_.data(), cap);
#endif
            if (n == 0) break;

            for (size_t i = 0; i < n; ++i)
            {
                auto& op = this->ops_[i];
                switch (op.op)
                {
                case OpType::ADD:
                {
                    Timer* t = op.timer;
                    addTimerToWheel(t, t->expiryTime);
                    this->timerMap_[t->id] = t;
                    ++this->activeTimerCount_;
                    break;
                }

                case OpType::UPDATE:
                {
                    auto it = timerMap_.find(op.id);
                    if (it != timerMap_.end())
                    {
                        Timer* t = it->second;
                        if (t->active)
                        {
                            removeTimerFromWheel(t);
                            t->duration_ms = op.new_dur;
                            t->expiryTime  = getCurrentTime() + t->duration_ms;
                            addTimerToWheel(t, t->expiryTime);
                        }
                    }
                    else
                    {
                        auto* t       = new Timer(op.new_dur);
                        t->active     = true;
                        t->id         = op.id;
                        t->expiryTime = getCurrentTime() + t->duration_ms;
                        addTimerToWheel(t, t->expiryTime);
                        this->timerMap_[t->id] = t;
                        ++this->activeTimerCount_;
                    }
                    break;
                }

                case OpType::REMOVE:
                {
                    auto it = timerMap_.find(op.id_only);
                    if (it != this->timerMap_.end())
                    {
                        Timer* t = it->second;
                        if (t->active)
                        {
                            t->active = false;
                            removeTimerFromWheel(t);
                            this->timerMap_.erase(it);
                            --this->activeTimerCount_;
                            if (t->coro) t->coro.destroy();
                            delete t;
                        }
                    }
                    break;
                }
                }
            }
        }

        const timeout_t newTime = getCurrentTime();
        const uint64_t  elapsed = newTime - this->currentTime_;

        if (elapsed == 0)
        {
            if (this->nextExpiryTime_ == 0 && this->activeTimerCount_ > 0)
                updateNextExpiryTime();
            return;
        }

        this->currentTime_ = newTime;

        uint64_t ticks = elapsed / this->wheels_[0].interval_;
        if (ticks == 0) ticks = 1;

        for (uint64_t i = 0; i < ticks; ++i)
            advance();

        if (this->nextExpiryTime_ == 0 && this->activeTimerCount_ > 0)
            updateNextExpiryTime();
    }

    void TimerWheel::advance()
    {
        for (auto& wheel : this->wheels_)
        {
            auto& bucket = wheel.buckets_[wheel.currentSlot_];

            size_t i = 0;
            while (i < bucket.size())
            {
                Timer* timer = bucket[i];

                if (!timer->active)
                {
                    bucket[i] = bucket.back();
                    bucket.pop_back();
                    continue;
                }

                if (is_due(this->currentTime_, timer->expiryTime, wheel.interval_))
                {
                    if (timer->coro)
                        system::this_thread::detail::q->enqueue(timer->coro);

                    timer->active = false;
                    this->timerMap_.erase(timer->id);
                    --this->activeTimerCount_;

                    if (timer->expiryTime == this->nextExpiryTime_)
                        this->nextExpiryTime_ = 0;

                    delete timer;

                    bucket[i] = bucket.back();
                    bucket.pop_back();
                }
                else
                {
                    addTimerToWheel(timer, timer->expiryTime);

                    bucket[i] = bucket.back();
                    bucket.pop_back();
                }
            }

            wheel.currentSlot_ = (wheel.currentSlot_ + 1) % wheel.slots_;
            if (wheel.currentSlot_ != 0)
                break;
        }
    }

    bool TimerWheel::empty() const
    {
        return this->activeTimerCount_ == 0;
    }

    task::Awaitable<void> timeout_coroutine(std::function<void(std::any&)> f, std::any arg)
    {
        f(arg);
        co_return;
    }
}
