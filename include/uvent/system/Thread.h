//
// Created by Kirill Zhukov on 07.11.2024.
//

#ifndef UVENT_THREAD_H
#define UVENT_THREAD_H

#include <barrier>
#include <chrono>
#include <ctime>
#include <functional>
#include <stop_token>
#include <thread>
#include <uvent/pool/TLSRegistry.h>
#include "uvent/base/Predefines.h"
#include "uvent/system/Defines.h"
#include "uvent/system/SystemContext.h"

namespace usub::uvent::system
{
    enum ThreadLaunchMode
    {
        CURRENT,
        NEW
    };

    class Thread
    {
    public:
        friend class ThreadPool;

        Thread(std::barrier<>* barrier, int index, thread::ThreadLocalStorage* thread_local_storage,
               ThreadLaunchMode tlm);

        Thread(Thread&&) noexcept = default;

        Thread& operator=(Thread&&) noexcept = default;

        Thread(const Thread&) = delete;

        Thread& operator=(const Thread&) = delete;

        ~Thread();

        void run_current();

        bool stop();

    private:
        void threadFunction(std::stop_token token);

        void processInboxQueue();

        void processCancelKicks();
#ifdef UVENT_RUNTIME_DRAIN
        void drainStep();
        bool drain_started_{false};
#endif

#ifdef UVENT_SOCKET_OWNER_FORWARDING
        /// Apply socket maintenance ops forwarded to this worker as owner
        /// (see thread::SocketOp / net::detail::apply_socket_op).
        void processSocketOps();
#endif

    private:
        int index_;
        std::jthread thread_;
        std::barrier<>* barrier;
        std::stop_source stop_source_;
        ThreadLaunchMode tlm{NEW};
        std::vector<std::coroutine_handle<>> tmp_tasks_;
        std::vector<net::SocketHeader*> tmp_sockets_;
        std::vector<std::coroutine_handle<>> tmp_coroutines_;
        thread::ThreadLocalStorage* thread_local_storage_;
    };
} // namespace usub::uvent::system


#endif // UVENT_THREAD_H
