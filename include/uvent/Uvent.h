//
// Created by kirill on 11/17/24.
//

#ifndef UVENT_UVENT_H
#define UVENT_UVENT_H

#include <cmath>
#include "uvent/net/Socket.h"
#include "uvent/pool/ThreadPool.h"
#include "uvent/sync/AsyncCancellation.h"
#include "uvent/sync/Select.h"
#include "uvent/system/Introspection.h"
#include "uvent/system/SystemContext.h"
#include "uvent/tasks/Task.h"
#ifdef UVENT_ENABLE_FIBERS
#include "uvent/fiber/Fiber.h"
#endif

namespace usub
{
    class Uvent : std::enable_shared_from_this<Uvent>
    {
    public:
        explicit Uvent(int threadCount);

        void stop();

        /// \brief UVENT_RUNTIME_DRAIN: tasks that were still alive when the workers exited (0 otherwise).
        static std::size_t drain_survivors() noexcept;

        void run();

        void for_each_thread(std::function<void(int, uvent::thread::ThreadLocalStorage*)> f) const;

    private:
        int thread_count_;
        uvent::ThreadPool pool;
    };
} // namespace usub

#endif // UVENT_UVENT_H
