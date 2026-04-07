//
// Created by kirill on 4/7/26.
//

#ifndef UVENT_SOCKETHEADERPOOL_H
#define UVENT_SOCKETHEADERPOOL_H

#include <cstddef>
#include <new>

#include "uvent/net/SocketMetadata.h"
#include "uvent/utils/intrinsincs/optimizations.h"

namespace usub::uvent::pool
{
    class SocketHeaderPool
    {
        struct FreeNode
        {
            FreeNode* next;
        };

        static_assert(sizeof(net::SocketHeader) >= sizeof(FreeNode));

        static constexpr size_t BLOCK = 512;
        static constexpr size_t PREFETCH_AHEAD = 8;

        FreeNode* head_ = nullptr;
        size_t size_ = 0; // freelist elements size
        size_t capacity_ = 0; // max freelist
        size_t total_ = 0; // ::new allocations total

        void push_free(net::SocketHeader* h) noexcept
        {
            auto* node = reinterpret_cast<FreeNode*>(h);
            node->next = this->head_;
            this->head_ = node;
            ++this->size_;
        }

        void grow()
        {
            net::SocketHeader* prev = nullptr;
            for (size_t i = 0; i < BLOCK; ++i)
            {
                auto* h = static_cast<net::SocketHeader*>(::operator new(sizeof(net::SocketHeader)));
                if (prev) [[likely]]
                    prefetch_for_write(prev);
                push_free(h);
                ++this->total_;
                prev = h;
            }
        }

    public:
        SocketHeaderPool() = default;

        ~SocketHeaderPool()
        {
            while (head_)
            {
                auto* node = this->head_;
                this->head_ = node->next;
                ::operator delete(reinterpret_cast<void*>(node));
            }
        }

        SocketHeaderPool(const SocketHeaderPool&) = delete;
        SocketHeaderPool& operator=(const SocketHeaderPool&) = delete;

        void init(size_t capacity)
        {
            this->capacity_ = capacity;
            while (this->size_ < this->capacity_)
                grow();
        }

        [[nodiscard]] net::SocketHeader* acquire()
        {
            if (this->head_) [[likely]]
            {
                auto* node = this->head_;
                if (node->next) [[likely]]
                    prefetch_for_read(node->next);
                this->head_ = node->next;
                --this->size_;
                auto* h = reinterpret_cast<net::SocketHeader*>(node);
                prefetch_for_write(h);
                new (h) net::SocketHeader{}; // конструируем только здесь
                return h;
            }
            ++this->total_;
            return new net::SocketHeader{};
        }

        void release(net::SocketHeader* h) noexcept
        {
            h->~SocketHeader();
            if (this->size_ < this->capacity_) [[likely]]
            {
                if (this->head_) [[likely]]
                    prefetch_for_read(this->head_);
                push_free(h);
            }
            else
            {
                ::operator delete(h);
                --this->total_;
            }
        }

        [[nodiscard]] size_t free_count() const noexcept { return this->size_; }
        [[nodiscard]] size_t total_count() const noexcept { return this->total_; }
    };

    inline thread_local SocketHeaderPool g_header_pool;

} // namespace usub::uvent::pool

#endif // UVENT_SOCKETHEADERPOOL_H
