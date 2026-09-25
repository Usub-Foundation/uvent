#include "uvent/blocking/Blocking.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "uvent/system/Settings.h"

// The legacy shared-poller layout (UVENT_ENABLE_REUSEADDR=OFF) hands a job back through `pl.wake()`, which needs
// the complete poller type; the per-worker layout goes through co_spawn_static only.
#ifndef UVENT_ENABLE_REUSEADDR
#ifdef OS_LINUX
#ifndef UVENT_ENABLE_IO_URING
#include "uvent/poll/EPoller.h"
#else
#include "uvent/poll/IOUringPoller.h"
#endif
#elif defined(OS_BSD) || defined(OS_APPLE)
#include "uvent/poll/KPoller.h"
#else
#include "uvent/poll/IocpPoller.h"
#endif
#endif

namespace usub::uvent::blocking::detail
{
    namespace
    {
        class Pool
        {
        public:
            static Pool& instance()
            {
                static Pool p;
                return p;
            }

            void submit(Job* job)
            {
                std::unique_lock lk(this->mtx_);
                this->queue_.push_back(job);
                ++this->pending_;
                if (this->idle_ == 0 && this->threads_ < this->max_threads())
                {
                    ++this->threads_;
                    std::thread([this] { this->worker(); }).detach();
                }
                lk.unlock();
                this->cv_.notify_one();
            }

            void wait_idle()
            {
                std::unique_lock lk(this->mtx_);
                this->idle_cv_.wait(lk, [this] { return this->pending_ == 0; });
            }

            std::size_t thread_count() noexcept
            {
                std::lock_guard lk(this->mtx_);
                return this->threads_;
            }

            ~Pool()
            {
                std::unique_lock lk(this->mtx_);
                this->stop_ = true;
                this->cv_.notify_all();
                this->exit_cv_.wait(lk, [this] { return this->threads_ == 0; }); // running jobs finish first
            }

        private:
            std::size_t max_threads() const noexcept
            {
                const std::size_t v = settings::blocking_threads_max;
                if (v)
                    return v;
                const unsigned hw = std::thread::hardware_concurrency();
                return std::min<std::size_t>(512, 4 * (hw ? hw : 4));
            }

            static void hand_back(Job* job) noexcept
            {
                // Same path as the resolver threads: through the owner's inbox in the per-worker layout, through
                // the shared queue plus a poller wake in the legacy layout.
#ifdef UVENT_ENABLE_REUSEADDR
                if (job->origin_tid >= 0)
                    system::co_spawn_static(job->waiter, job->origin_tid);
                else
                    system::co_spawn(job->waiter);
#else
                system::this_thread::detail::st->enqueue(job->waiter);
                system::this_thread::detail::pl.wake();
#endif
            }

            void worker()
            {
                const auto idle = std::chrono::milliseconds(settings::blocking_idle_timeout_ms);
                std::unique_lock lk(this->mtx_);
                for (;;)
                {
                    if (this->queue_.empty())
                    {
                        ++this->idle_;
                        const bool got = this->cv_.wait_for(lk, idle, [this] { return this->stop_ || !this->queue_.empty(); });
                        --this->idle_;
                        if (!got || (this->stop_ && this->queue_.empty()))
                        {
                            if (this->queue_.empty()) // idle timeout (or stop): retire this thread
                            {
                                --this->threads_;
                                if (this->threads_ == 0)
                                    this->exit_cv_.notify_all();
                                return;
                            }
                        }
                        if (this->queue_.empty())
                            continue;
                    }
                    Job* job = this->queue_.front();
                    this->queue_.pop_front();
                    lk.unlock();
                    job->run();
                    hand_back(job);
                    lk.lock();
                    if (--this->pending_ == 0)
                        this->idle_cv_.notify_all();
                }
            }

            std::mutex mtx_;
            std::condition_variable cv_, idle_cv_, exit_cv_;
            std::deque<Job*> queue_;
            std::size_t threads_{0}, idle_{0}, pending_{0};
            bool stop_{false};
        };
    } // namespace

    void submit(Job* job) { Pool::instance().submit(job); }

    void wait_idle() { Pool::instance().wait_idle(); }

    std::size_t thread_count() noexcept { return Pool::instance().thread_count(); }
} // namespace usub::uvent::blocking::detail
