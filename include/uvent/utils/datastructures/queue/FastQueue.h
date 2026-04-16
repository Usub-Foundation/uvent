//
// Created by kirill on 5/7/25.
//

#ifndef FASTQUEUE_FASTQUEUE_H
#define FASTQUEUE_FASTQUEUE_H

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <utility>
#include "uvent/utils/intrinsics/optimizations.h"

namespace usub::queue::single_thread
{
    static constexpr size_t k_prefetch_ahead = 16;

    template <typename T, size_t Capacity>
    class RingQueue
    {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of two");

    public:
        bool enqueue(T&& item) noexcept
        {
            size_t next_tail = (this->tail + 1) & (Capacity - 1);
            if (next_tail == this->head)
                return false;

            prefetch_for_write(&this->buffer[next_tail]);
            this->buffer[this->tail] = std::forward<T>(item);
            this->tail = next_tail;
            return true;
        }

        bool dequeue(T& item) noexcept
        {
            if (this->head == this->tail)
                return false;

            prefetch_for_read(&this->buffer[(this->head + 1) & (Capacity - 1)]);
            item = this->buffer[this->head];
            this->head = (this->head + 1) & (Capacity - 1);
            return true;
        }

    private:
        T buffer[Capacity];
        size_t head = 0;
        size_t tail = 0;
    };

    template <typename T>
    class Queue
    {
    public:
        static_assert(std::is_move_constructible_v<T>);
        static_assert(std::is_nothrow_move_constructible_v<T> || std::is_trivially_copyable_v<T>);

        explicit Queue(size_t initial_capacity = 1024) { allocate(initial_capacity); }

        ~Queue()
        {
            destroy_all();
            free_items(this->buffer);
        }

        template <typename U>
        void enqueue(U&& item)
        {
            if (this->tail - this->head == this->capacity)
                grow();

            size_t idx = this->tail & this->mask;
            prefetch_for_write(&this->buffer[(idx + 4) & this->mask]);
            new (&this->buffer[idx]) T(std::forward<U>(item));
            ++this->tail;
        }

        template <typename... Args>
        void emplace(Args&&... args)
        {
            if (this->tail - this->head == this->capacity)
                grow();

            size_t idx = this->tail & this->mask;
            prefetch_for_write(&this->buffer[(idx + 4) & this->mask]);
            new (&this->buffer[idx]) T(std::forward<Args>(args)...);
            ++this->tail;
        }

        template <typename U>
        size_t enqueue_bulk(U* src, size_t count)
        {
            if (count == 0)
                return 0;

            const size_t sz = this->tail - this->head;
            const size_t need = sz + count;
            while (need > this->capacity)
                grow();

            const size_t start = this->tail;
            const size_t end = this->tail + count;

            for (size_t pos = start; pos < end; ++pos)
            {
                const size_t i = pos - start;
                const size_t look = i + k_prefetch_ahead;
                if (look < count)
                {
                    prefetch_for_write(&this->buffer[(pos + k_prefetch_ahead) & this->mask]);
                    prefetch_for_read(&src[look]);
                }
            }

            const size_t s = start & this->mask;
            const size_t e = end & this->mask;

            if constexpr (std::is_trivially_copyable_v<T> && std::is_same_v<std::remove_cv_t<U>, T>)
            {
                if (s < e)
                {
                    std::memcpy(&this->buffer[s], src, sizeof(T) * count);
                }
                else
                {
                    const size_t first_part = this->capacity - s;
                    std::memcpy(&this->buffer[s], src, sizeof(T) * first_part);
                    std::memcpy(&this->buffer[0], src + first_part, sizeof(T) * (count - first_part));
                }
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    const size_t idx = (start + i) & this->mask;
                    new (&this->buffer[idx]) T(std::forward<U>(src[i]));
                }
            }

            this->tail = end;
            return count;
        }

        template <typename... Arrays>
        size_t enqueue_bulk_emplace(size_t count, Arrays*... arrays)
        {
            if (count == 0)
                return 0;

            const size_t sz = this->tail - this->head;
            const size_t need = sz + count;
            while (need > this->capacity)
                grow();

            const size_t start = this->tail;
            const size_t end = this->tail + count;

            for (size_t pos = start; pos < end; ++pos)
            {
                const size_t i = pos - start;
                const size_t look = i + k_prefetch_ahead;
                if (look < count)
                {
                    prefetch_for_write(&this->buffer[(pos + k_prefetch_ahead) & this->mask]);
                }
            }

            for (size_t i = 0; i < count; ++i)
            {
                const size_t idx = (start + i) & this->mask;
                new (&this->buffer[idx]) T(std::forward<Arrays>(arrays[i])...);
            }

            this->tail = end;
            return count;
        }

        bool dequeue(T& item)
        {
            if (this->head == this->tail)
                return false;

            size_t idx = this->head & this->mask;
            prefetch_for_read(&this->buffer[(idx + 4) & this->mask]);

            item = std::move(this->buffer[idx]);
            if constexpr (!std::is_trivially_destructible_v<T>)
                this->buffer[idx].~T();

            ++this->head;
            normalize_if_empty();
            return true;
        }

        size_t dequeue_bulk(T* out, size_t max_count)
        {
            size_t available = this->tail - this->head;
            if (available == 0 || max_count == 0)
                return 0;

            size_t count = (available < max_count) ? available : max_count;

            const size_t start = this->head;
            const size_t end = this->head + count;

            for (size_t pos = start; pos < end; ++pos)
            {
                size_t lookahead = (pos - start) + k_prefetch_ahead;
                if (lookahead < count)
                {
                    prefetch_for_read(&this->buffer[(pos + k_prefetch_ahead) & this->mask]);
                }
            }

            const size_t s = start & this->mask;
            const size_t e = end & this->mask;

            if constexpr (std::is_trivially_copyable_v<T>)
            {
                if (s < e)
                {
                    std::memcpy(out, &this->buffer[s], sizeof(T) * count);
                }
                else
                {
                    size_t first_part = this->capacity - s;
                    std::memcpy(out, &this->buffer[s], sizeof(T) * first_part);
                    std::memcpy(out + first_part, &this->buffer[0], sizeof(T) * (count - first_part));
                }
            }
            else
            {
                for (size_t i = 0; i < count; ++i)
                {
                    size_t idx = (this->head + i) & this->mask;
                    out[i] = std::move(this->buffer[idx]);
                }
                for (size_t i = 0; i < count; ++i)
                {
                    size_t idx = (this->head + i) & this->mask;
                    this->buffer[idx].~T();
                }
            }

            this->head = end;
            normalize_if_empty();
            return count;
        }

        [[nodiscard]] bool empty() const noexcept { return this->head == this->tail; }
        [[nodiscard]] size_t size() const noexcept { return this->tail - this->head; }

    private:
        static T* alloc_items(size_t n)
        {
            return static_cast<T*>(::operator new[](sizeof(T) * n, std::align_val_t(alignof(T))));
        }

        static void free_items(T* p) noexcept { ::operator delete[](p, std::align_val_t(alignof(T))); }

        static size_t next_pow2(size_t x)
        {
            if (x == 0)
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

        void allocate(size_t cap)
        {
            this->capacity = next_pow2(cap);
            this->mask = this->capacity - 1;
            this->buffer = alloc_items(this->capacity);
        }

        void destroy_all()
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                while (this->head != this->tail)
                {
                    this->buffer[this->head & this->mask].~T();
                    ++this->head;
                }
            }
            else
            {
                // для тривиальных деструкторов ничего не нужно
            }
        }

        void grow()
        {
            size_t new_cap = this->capacity * 2;
            T* new_buf = alloc_items(new_cap);
            assert(new_buf && "allocation failed");

            size_t count = size();
            for (size_t i = 0; i < count; ++i)
            {
                size_t src_idx = (this->head + i) & this->mask;
                new (&new_buf[i]) T(std::move(this->buffer[src_idx]));
                if constexpr (!std::is_trivially_destructible_v<T>)
                    this->buffer[src_idx].~T();
            }

            free_items(this->buffer);
            this->buffer = new_buf;
            this->head = 0;
            this->tail = count;
            this->capacity = new_cap;
            this->mask = new_cap - 1;
        }

        void normalize_if_empty() noexcept
        {
            if (this->head == this->tail)
            {
                this->head = this->tail = 0;
            }
        }

    private:
        T* buffer = nullptr;
        size_t capacity = 0;
        size_t head = 0; // монотонный
        size_t tail = 0; // монотонный
        size_t mask = 0;
    };
} // namespace usub::queue::single_thread

#endif // FASTQUEUE_FASTQUEUE_H
