//
// Created by root on 9/9/25.
//

#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include "uvent/system/Settings.h"
#include "uvent/utils/datastructures/DataStructuresMetadata.h"
#include "uvent/utils/intrinsics/optimizations.h"

// ======== helpers ========
namespace usub::queue::concurrent
{
    static inline size_t next_pow2(size_t x)
    {
        if (x <= 1)
            return 1;
        --x;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        x |= x >> 32;
        return x + 1;
    }

    template <typename T>
    struct storage_t
    {
        alignas(T) unsigned char data[sizeof(T)];
    };

    static constexpr size_t k_prefetch_ahead = 8;

    template <typename T>
    class alignas(data_structures::metadata::CACHELINE_SIZE) SPSCQueue
    {
    private:
        static_assert(std::is_move_constructible_v<T>, "T must be move constructible");

    public:
        explicit SPSCQueue(size_t capacity_pow2 = 1024) :
            cap_(next_pow2(capacity_pow2)), mask_(cap_ - 1),
            buf_(static_cast<storage_t<T>*>(
                ::operator new[](this->cap_ * sizeof(storage_t<T>), std::align_val_t(alignof(T)))))
        {
        }

        ~SPSCQueue()
        {
            T tmp;
            while (try_dequeue(tmp))
            {
            }
            ::operator delete[](this->buf_, std::align_val_t(alignof(T)));
        }

        bool try_enqueue(const T& v) { return emplace(v); }
        bool try_enqueue(T&& v) { return emplace(std::move(v)); }

        template <class... Args>
        bool emplace(Args&&... args)
        {
            size_t tail = this->tail_.load(std::memory_order_relaxed);
            size_t next = (tail + 1) & this->mask_;
            if (next == this->head_.load(std::memory_order_acquire))
                return false; // full

            prefetch_for_write(&this->buf_[(tail + 4) & this->mask_]);
            new (&this->buf_[tail]) T(std::forward<Args>(args)...);
            this->tail_.store(next, std::memory_order_release);
            return true;
        }

        bool try_dequeue(T& out)
        {
            size_t head = this->head_.load(std::memory_order_relaxed);
            if (head == this->tail_.load(std::memory_order_acquire))
                return false; // empty

            prefetch_for_read(&this->buf_[(head + 4) & this->mask_]);
            T* ptr = std::launder(reinterpret_cast<T*>(&this->buf_[head]));
            out = std::move(*ptr);
            if constexpr (!std::is_trivially_destructible_v<T>)
                ptr->~T();
            this->head_.store((head + 1) & this->mask_, std::memory_order_release);
            return true;
        }

        bool empty() const noexcept
        {
            return this->head_.load(std::memory_order_acquire) == this->tail_.load(std::memory_order_acquire);
        }

        size_t capacity() const noexcept { return this->cap_ - 1; }

        size_t size_relaxed() const noexcept
        {
            size_t h = this->head_.load(std::memory_order_relaxed);
            size_t t = this->tail_.load(std::memory_order_relaxed);
            return (t - h) & this->mask_;
        }

        size_t size() const noexcept
        {
            for (;;)
            {
                size_t h1 = this->head_.load(std::memory_order_acquire);
                size_t t = this->tail_.load(std::memory_order_acquire);
                size_t h2 = this->head_.load(std::memory_order_acquire);
                if (h1 == h2)
                    return (t - h2) & this->mask_;
                cpu_relax();
            }
        }

    private:
        const size_t cap_;
        const size_t mask_;
        storage_t<T>* buf_;

        alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<size_t> head_{0};
        alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<size_t> tail_{0};
        char _pad_[data_structures::metadata::CACHELINE_SIZE - sizeof(std::atomic<size_t>)]{};
    };

    template <typename T>
    class alignas(data_structures::metadata::CACHELINE_SIZE) MPMCQueue
    {
    private:
        static_assert(std::is_move_constructible_v<T>, "T must be move constructible");

        struct Cell
        {
            std::atomic<size_t> seq;
            storage_t<T> storage;
        };

    public:
        explicit MPMCQueue(size_t capacity_pow2 = 1024) :
            cap_(next_pow2(capacity_pow2)), mask_(cap_ - 1),
            cells_(static_cast<Cell*>(::operator new[](cap_ * sizeof(Cell), std::align_val_t(alignof(Cell)))))
        {
            for (size_t i = 0; i < cap_; ++i)
            {
                new (&this->cells_[i]) Cell{std::atomic<size_t>(i), {}};
            }
        }

        ~MPMCQueue()
        {
            T tmp;
            while (try_dequeue(tmp))
            {
            }

            for (size_t i = 0; i < this->cap_; ++i)
                this->cells_[i].~Cell();

            ::operator delete[](this->cells_, std::align_val_t(alignof(Cell)));
        }

        bool try_enqueue(const T& v)
        {
            return do_enqueue([&](void* p) { new (p) T(v); });
        }
        bool try_enqueue(T&& v)
        {
            return do_enqueue([&](void* p) { new (p) T(std::move(v)); });
        }

        size_t try_enqueue_bulk(const T* in, size_t n)
        {
            if (n == 0)
                return 0;
            size_t start = this->enq_pos_.load(std::memory_order_relaxed);
            for (;;)
            {
                Cell& c0 = this->cells_[start & this->mask_];
                size_t seq0 = c0.seq.load(std::memory_order_acquire);
                if ((intptr_t)seq0 - (intptr_t)start < 0)
                    return 0;

                size_t end = start + n;
                if (this->enq_pos_.compare_exchange_weak(start, end, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed))
                {
                    size_t taken = 0;
                    for (size_t pos = start; pos < end; ++pos)
                    {
                        Cell& c = this->cells_[pos & this->mask_];
                        size_t seq = c.seq.load(std::memory_order_acquire);
                        if ((intptr_t)seq - (intptr_t)pos != 0)
                            break;

                        prefetch_for_write(&this->cells_[(pos + 16) & this->mask_]);
                        void* p = static_cast<void*>(std::launder(reinterpret_cast<T*>(&c.storage)));
                        new (p) T(in[taken]);
                        c.seq.store(pos + 1, std::memory_order_release);
                        ++taken;
                    }
                    if (taken < n)
                    {
                        size_t expect = end;
                        this->enq_pos_.compare_exchange_strong(expect, start + taken, std::memory_order_acq_rel,
                                                               std::memory_order_relaxed);
                    }
                    return taken;
                }
                cpu_relax();
                start = this->enq_pos_.load(std::memory_order_relaxed);
            }
        }

        template <class... Args>
        bool emplace(Args&&... args)
        {
            return do_enqueue([&](void* p) { new (p) T(std::forward<Args>(args)...); });
        }

        bool try_dequeue(T& out)
        {
            size_t pos = this->deq_pos_.load(std::memory_order_relaxed);
            for (;;)
            {
                Cell& c = this->cells_[pos & this->mask_];
                size_t seq = c.seq.load(std::memory_order_acquire);
                intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
                if (dif == 0)
                {
                    if (this->deq_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed))
                    {
                        prefetch_for_read(&c);
                        T* ptr = std::launder(reinterpret_cast<T*>(&c.storage));
                        out = std::move(*ptr);
                        if constexpr (!std::is_trivially_destructible_v<T>)
                            ptr->~T();
                        c.seq.store(pos + this->cap_, std::memory_order_release);
                        return true;
                    }
                }
                else if (dif < 0)
                {
                    return false; // empty
                }
                else
                {
                    pos = this->deq_pos_.load(std::memory_order_relaxed);
                    cpu_relax();
                }
            }
        }

        size_t try_dequeue_bulk(T* out, size_t max_items)
        {
            if (max_items == 0)
                return 0;

            size_t start = this->deq_pos_.load(std::memory_order_relaxed);
            Cell* c0 = &this->cells_[start & this->mask_];
            size_t seq0 = c0->seq.load(std::memory_order_acquire);
            if ((intptr_t)seq0 - (intptr_t)(start + 1) < 0)
                return 0;

            size_t end = start + max_items;
            if (!this->deq_pos_.compare_exchange_strong(start, end, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed))
            {
                size_t n = 0;
                for (; n < max_items; ++n)
                {
                    if (!try_dequeue(out[n]))
                        break;
                }
                return n;
            }

            size_t n_taken = 0;
            for (size_t pos = start; pos < end; ++pos)
            {
                Cell& c = this->cells_[pos & this->mask_];
                if ((pos - start) + k_prefetch_ahead < max_items)
                    prefetch_for_read(&this->cells_[(pos + k_prefetch_ahead) & this->mask_]);
                size_t seq = c.seq.load(std::memory_order_acquire);
                if ((intptr_t)seq - (intptr_t)(pos + 1) < 0)
                    break;

                T* ptr = std::launder(reinterpret_cast<T*>(&c.storage));
                out[n_taken++] = std::move(*ptr);
                if constexpr (!std::is_trivially_destructible_v<T>)
                    ptr->~T();
                c.seq.store(pos + cap_, std::memory_order_release);
            }

            if (n_taken < max_items)
            {
                size_t expect_end = end;
                size_t give_back = max_items - n_taken;
                this->deq_pos_.compare_exchange_strong(expect_end, end - give_back, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed);
            }
            return n_taken;
        }

        bool empty() const noexcept
        {
            size_t pos = this->deq_pos_.load(std::memory_order_acquire);
            size_t seq = this->cells_[pos & this->mask_].seq.load(std::memory_order_acquire);
            return static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0;
        }

        bool empty_relaxed() const noexcept
        {
            return this->enq_pos_.load(std::memory_order_relaxed) == this->deq_pos_.load(std::memory_order_relaxed);
        }

        size_t capacity() const noexcept { return this->cap_; }

        size_t size_relaxed() const noexcept
        {
            size_t e = this->enq_pos_.load(std::memory_order_relaxed);
            size_t d = this->deq_pos_.load(std::memory_order_relaxed);
            size_t diff = e - d;
            return diff > this->cap_ ? this->cap_ : diff;
        }

        size_t size() const noexcept
        {
            for (;;)
            {
                size_t d1 = this->deq_pos_.load(std::memory_order_acquire);
                size_t e = this->enq_pos_.load(std::memory_order_acquire);
                size_t d2 = this->deq_pos_.load(std::memory_order_acquire);
                if (d1 == d2)
                {
                    size_t diff = e - d2;
                    return diff > this->cap_ ? this->cap_ : diff;
                }
                cpu_relax();
            }
        }

    private:
        template <class Ctor>
        bool do_enqueue(Ctor&& ctor)
        {
            size_t pos = this->enq_pos_.load(std::memory_order_relaxed);
            for (;;)
            {
                Cell& c = this->cells_[pos & this->mask_];
                size_t seq = c.seq.load(std::memory_order_acquire);
                intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
                if (dif == 0)
                {
                    if (this->enq_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed))
                    {
                        prefetch_for_write(&c);
                        void* p = static_cast<void*>(std::launder(reinterpret_cast<T*>(&c.storage)));
                        ctor(p);
                        c.seq.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (dif < 0)
                {
                    return false;
                }
                else
                {
                    pos = this->enq_pos_.load(std::memory_order_relaxed);
                    cpu_relax();
                }
            }
        }

        const size_t cap_;
        const size_t mask_;
        Cell* cells_;

        alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<size_t> enq_pos_{0};
        alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<size_t> deq_pos_{0};
        char _pad0_[data_structures::metadata::CACHELINE_SIZE - sizeof(std::atomic<size_t>)]{};
        char _pad1_[data_structures::metadata::CACHELINE_SIZE - sizeof(std::atomic<size_t>)]{};
    };
} // namespace usub::queue::concurrent

#endif // MPMCQUEUE_H
