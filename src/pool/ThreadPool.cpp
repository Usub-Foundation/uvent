//
// Created by Kirill Zhukov on 07.11.2024.
//

#include "uvent/pool/ThreadPool.h"

namespace usub::uvent
{
    ThreadPool::ThreadPool(int size) : size_(size)
    {
        this->barrier = new std::barrier<>(size);
        system::global::detail::tls_registry = std::make_unique<thread::TLSRegistry>(this->size_);
#ifdef UVENT_RUNTIME_DRAIN
        static ThreadPool* self = nullptr;
        self = this;
        system::global::detail::request_stop_all = +[]
        {
            if (self)
                self->stop();
        };
#endif
        for (int i = 0; i < size - 1; i++)
            this->threads.push_back(
                new system::Thread(this->barrier, i, system::global::detail::tls_registry->getStorage(i), system::NEW));
    }

    void ThreadPool::stop()
    {
        for (auto& thread : this->threads)
            thread->stop();
    }

    void ThreadPool::addThread(system::ThreadLaunchMode tlm)
    {
        const int index = static_cast<int>(threads.size());
        auto* t = new system::Thread(barrier, index,
                                     system::global::detail::tls_registry->getStorage(this->threads.size()), tlm);
        threads.push_back(t);

        if (tlm == system::CURRENT)
            t->run_current();
    }

    const thread::TLSRegistry* ThreadPool::getTLSRegistry() { return system::global::detail::tls_registry.get(); }

    ThreadPool::~ThreadPool()
    {
        this->stop();
        for (auto& thread : this->threads)
            delete thread;
        delete this->barrier;
    }
} // namespace usub::uvent
