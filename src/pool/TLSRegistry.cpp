//
// Created by root on 10/21/25.
//

#include <uvent/pool/TLSRegistry.h>

namespace usub::uvent::thread
{
    TLSRegistry::TLSRegistry(int threadCount) : threadCount_(threadCount)
    {
        this->tls_storage_.reserve(threadCount);
        for (int i = 0; i < threadCount; ++i)
            this->tls_storage_.emplace_back(new ThreadLocalStorage{});
    }
    TLSRegistry::~TLSRegistry()
    {
        for (int i = 0; i < this->threadCount_; ++i)
            delete this->tls_storage_[i];
    }

    ThreadLocalStorage* TLSRegistry::getStorage(int index) const { return this->tls_storage_[index]; }
} // namespace usub::uvent::thread
