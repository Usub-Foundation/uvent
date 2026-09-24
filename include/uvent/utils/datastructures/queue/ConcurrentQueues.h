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
#include <algorithm>
#include "uvent/system/Settings.h"
#include "uvent/utils/datastructures/DataStructuresMetadata.h"
#include "uvent/utils/intrinsics/optimizations.h"
#include "uvent/utils/sync/HazardPointers.h"
#include "uvent/utils/datastructures/queue/IntrusiveMPSC.h"

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
            size_t taken = 0;
            for (; taken < n; ++taken)
            {
                if (!try_enqueue(in[taken]))
                    break;
            }
            return taken;
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
            size_t n = 0;
            for (; n < max_items; ++n)
            {
                if (!try_dequeue(out[n]))
                    break;
            }
            return n;
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
    template <typename T, size_t SegmentSize = 1024>
    class alignas(data_structures::metadata::CACHELINE_SIZE) SegmentedMPMCQueue
    {
        static_assert(std::is_move_constructible_v<T>, "T must be move constructible");
        static_assert(SegmentSize >= 2, "SegmentSize must be >= 2");

        using Hazard = usub::utils::sync::HazardDomain;

        enum : uint8_t
        {
            kEmpty = 0,
            kFull = 1,
            kDead = 2
        };

        struct Cell
        {
            std::atomic<uint8_t> state;
            storage_t<T> storage;
        };

        struct alignas(data_structures::metadata::CACHELINE_SIZE) Segment
        {
            alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<size_t> enq_pos{0};
            alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<size_t> deq_pos{0};
            alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<Segment*> next{nullptr};
            uint64_t base{0};
            alignas(data_structures::metadata::CACHELINE_SIZE) Cell cells[SegmentSize];

            Segment() noexcept
            {
                for (auto& c : this->cells)
                    c.state.store(kEmpty, std::memory_order_relaxed);
            }

            void reset(uint64_t new_base) noexcept
            {
                this->enq_pos.store(0, std::memory_order_relaxed);
                this->deq_pos.store(0, std::memory_order_relaxed);
                this->next.store(nullptr, std::memory_order_relaxed);
                this->base = new_base;
                for (auto& c : this->cells)
                    c.state.store(kEmpty, std::memory_order_relaxed);
            }
        };

        struct SpareHolder
        {
            Segment* seg{nullptr};
            bool alive{true};

            ~SpareHolder()
            {
                delete this->seg;
                this->seg = nullptr;
                this->alive = false;
            }
        };

        static SpareHolder& spare() noexcept
        {
            static thread_local SpareHolder h;
            return h;
        }

        static Segment* take_segment(uint64_t base)
        {
            SpareHolder& h = spare();
            if (Segment* s = h.seg)
            {
                h.seg = nullptr;
                s->reset(base);
                return s;
            }
            Segment* s = new Segment();
            s->base = base;
            return s;
        }

        static void give_segment(Segment* s) noexcept
        {
            SpareHolder& h = spare();
            if (h.alive && !h.seg)
                h.seg = s;
            else
                delete s;
        }

        static void retire_deleter(void* p) noexcept { give_segment(static_cast<Segment*>(p)); }

        static T* value_ptr(Cell& c) noexcept { return std::launder(reinterpret_cast<T*>(&c.storage)); }

    public:
        explicit SegmentedMPMCQueue(size_t = 0)
        {
            Segment* s = new Segment();
            this->head_.store(s, std::memory_order_relaxed);
            this->tail_.store(s, std::memory_order_relaxed);
        }

        SegmentedMPMCQueue(const SegmentedMPMCQueue&) = delete;
        SegmentedMPMCQueue& operator=(const SegmentedMPMCQueue&) = delete;

        ~SegmentedMPMCQueue()
        {
            Segment* s = this->head_.load(std::memory_order_relaxed);
            while (s)
            {
                const size_t d = s->deq_pos.load(std::memory_order_relaxed);
                const size_t e = (std::min)(s->enq_pos.load(std::memory_order_relaxed), SegmentSize);
                for (size_t i = (std::min)(d, SegmentSize); i < e; ++i)
                {
                    Cell& c = s->cells[i];
                    if (c.state.load(std::memory_order_relaxed) == kFull)
                    {
                        if constexpr (!std::is_trivially_destructible_v<T>)
                            value_ptr(c)->~T();
                    }
                }
                Segment* nx = s->next.load(std::memory_order_relaxed);
                delete s;
                s = nx;
            }
        }

        void enqueue(const T& v)
        {
            T tmp(v);
            do_enqueue(tmp);
        }

        void enqueue(T&& v) { do_enqueue(v); }

        template <class... Args>
        void emplace(Args&&... args)
        {
            T tmp(std::forward<Args>(args)...);
            do_enqueue(tmp);
        }

        bool try_enqueue(const T& v)
        {
            enqueue(v);
            return true;
        }

        bool try_enqueue(T&& v)
        {
            enqueue(std::move(v));
            return true;
        }

        size_t try_enqueue_bulk(const T* in, size_t n) { return enqueue_bulk(in, n); }

        size_t enqueue_bulk(const T* in, size_t n)
        {
            if (n == 0)
                return 0;

            Hazard& hz = Hazard::instance();
            Hazard::Record* rec = hz.local_record();
            Segment* s = hz.protect(rec, this->tail_);

            size_t done = 0;
            while (done < n)
            {
                const size_t want = n - done;
                const size_t pos = s->enq_pos.fetch_add(want, std::memory_order_relaxed);
                if (pos >= SegmentSize)
                {
                    s = advance_tail(rec, s, nullptr);
                    continue;
                }

                const size_t end = (std::min)(pos + want, SegmentSize);
                for (size_t i = pos; i < end; ++i)
                {
                    Cell& c = s->cells[i];
                    if ((i - pos) + k_prefetch_ahead < end - pos)
                        prefetch_for_write(&s->cells[i + k_prefetch_ahead]);
                    T tmp(in[done]);
                    publish_cell(c, tmp);
                    ++done;
                }
                if (pos + want > SegmentSize)
                    s = advance_tail(rec, s, nullptr);
            }
            return n;
        }

        bool try_dequeue(T& out)
        {
            Hazard& hz = Hazard::instance();
            Hazard::Record* rec = hz.local_record();
            Segment* s = hz.protect(rec, this->head_);

            for (;;)
            {
                const size_t d = s->deq_pos.load(std::memory_order_relaxed);
                if (d >= SegmentSize)
                {
                    s = advance_head(rec, s);
                    if (!s)
                        return false;
                    continue;
                }

                Cell& c = s->cells[d];
                const uint8_t st = c.state.load(std::memory_order_acquire);
                if (st != kFull)
                {
                    if (d >= s->enq_pos.load(std::memory_order_acquire))
                        return false;
                    cpu_relax();
                    continue;
                }

                const bool scarce =
                    d + 1 >= SegmentSize || s->cells[d + 1].state.load(std::memory_order_acquire) != kFull;
                if (scarce)
                {
                    size_t expected = d;
                    if (!s->deq_pos.compare_exchange_weak(expected, d + 1, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed))
                        continue;
                    consume_cell(c, out);
                    return true;
                }

                const size_t pos = s->deq_pos.fetch_add(1, std::memory_order_relaxed);
                if (pos >= SegmentSize)
                    continue;
                if (take_cell(s, pos, s->cells[pos], out))
                    return true;
            }
        }

        size_t try_dequeue_bulk(T* out, size_t max_items)
        {
            if (max_items == 0)
                return 0;

            Hazard& hz = Hazard::instance();
            Hazard::Record* rec = hz.local_record();
            Segment* s = hz.protect(rec, this->head_);

            for (;;)
            {
                const size_t d = s->deq_pos.load(std::memory_order_relaxed);
                if (d >= SegmentSize)
                {
                    s = advance_head(rec, s);
                    if (!s)
                        return 0;
                    continue;
                }

                const size_t limit = (std::min)(SegmentSize - d, max_items);
                size_t ready = 0;
                while (ready < limit && s->cells[d + ready].state.load(std::memory_order_acquire) == kFull)
                    ++ready;

                if (ready == 0)
                {
                    if (d >= s->enq_pos.load(std::memory_order_acquire))
                        return 0;
                    cpu_relax();
                    continue;
                }

                if (ready == 1)
                {
                    size_t expected = d;
                    if (!s->deq_pos.compare_exchange_weak(expected, d + 1, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed))
                        continue;
                    consume_cell(s->cells[d], out[0]);
                    return 1;
                }

                const size_t pos = s->deq_pos.fetch_add(ready, std::memory_order_relaxed);
                if (pos >= SegmentSize)
                    continue;

                const size_t end = (std::min)(pos + ready, SegmentSize);
                size_t taken = 0;
                for (size_t i = pos; i < end; ++i)
                {
                    if ((i - pos) + k_prefetch_ahead < end - pos)
                        prefetch_for_read(&s->cells[i + k_prefetch_ahead]);
                    if (take_cell(s, i, s->cells[i], out[taken]))
                        ++taken;
                }
                if (taken)
                    return taken;
            }
        }

        bool empty_relaxed() const noexcept
        {
            Hazard& hz = Hazard::instance();
            Segment* s = hz.protect(hz.local_record(), this->head_);
            const size_t d = s->deq_pos.load(std::memory_order_relaxed);
            const size_t e = s->enq_pos.load(std::memory_order_relaxed);
            if (d < SegmentSize)
                return d >= e;
            return s->next.load(std::memory_order_relaxed) == nullptr;
        }

        bool empty() const noexcept { return empty_relaxed(); }

        size_t size_relaxed() const noexcept
        {
            Hazard& hz = Hazard::instance();
            Hazard::Record* rec = hz.local_record();
            Segment* h = hz.protect(rec, this->head_);
            Segment* t = hz.protect(rec, this->tail_);
            const uint64_t hd = h->base + (std::min)(h->deq_pos.load(std::memory_order_relaxed), SegmentSize);
            const uint64_t te = t->base + (std::min)(t->enq_pos.load(std::memory_order_relaxed), SegmentSize);
            return te > hd ? static_cast<size_t>(te - hd) : 0;
        }

        size_t size() const noexcept { return size_relaxed(); }

        static constexpr size_t segment_size() noexcept { return SegmentSize; }

    private:
        void do_enqueue(T& value)
        {
            Hazard& hz = Hazard::instance();
            Hazard::Record* rec = hz.local_record();
            Segment* s = hz.protect(rec, this->tail_);

            for (;;)
            {
                const size_t pos = s->enq_pos.fetch_add(1, std::memory_order_relaxed);
                if (pos < SegmentSize)
                {
                    publish_cell(s->cells[pos], value);
                    return;
                }

                s = advance_tail(rec, s, &value);
                if (!s)
                    return;
            }
        }

        static void publish_cell(Cell& c, T& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            new (&c.storage) T(std::move(value));
            c.state.store(kFull, std::memory_order_release);
        }

        static void consume_cell(Cell& c, T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
        {
            T* ptr = value_ptr(c);
            out = std::move(*ptr);
            if constexpr (!std::is_trivially_destructible_v<T>)
                ptr->~T();
        }

        Segment* advance_tail(Hazard::Record* rec, Segment* s, T* value)
        {
            Hazard& hz = Hazard::instance();
            Segment* nx = s->next.load(std::memory_order_acquire);
            if (!nx)
            {
                Segment* fresh = take_segment(s->base + SegmentSize);
                if (value)
                {
                    new (&fresh->cells[0].storage) T(std::move(*value));
                    fresh->cells[0].state.store(kFull, std::memory_order_relaxed);
                    fresh->enq_pos.store(1, std::memory_order_relaxed);
                }
                Segment* expected = nullptr;
                if (s->next.compare_exchange_strong(expected, fresh, std::memory_order_release,
                                                    std::memory_order_acquire))
                {
                    Segment* t = s;
                    this->tail_.compare_exchange_strong(t, fresh, std::memory_order_release,
                                                        std::memory_order_relaxed);
                    if (value)
                        return nullptr;
                    return hz.protect(rec, this->tail_);
                }
                if (value)
                {
                    T* ptr = value_ptr(fresh->cells[0]);
                    *value = std::move(*ptr);
                    if constexpr (!std::is_trivially_destructible_v<T>)
                        ptr->~T();
                    fresh->cells[0].state.store(kEmpty, std::memory_order_relaxed);
                    fresh->enq_pos.store(0, std::memory_order_relaxed);
                }
                give_segment(fresh);
                nx = expected;
            }
            Segment* t = s;
            this->tail_.compare_exchange_strong(t, nx, std::memory_order_release, std::memory_order_relaxed);
            return hz.protect(rec, this->tail_);
        }

        Segment* advance_head(Hazard::Record* rec, Segment* s)
        {
            Segment* nx = s->next.load(std::memory_order_acquire);
            if (!nx)
                return nullptr;
            Segment* expected = s;
            Hazard& hz = Hazard::instance();
            if (this->head_.compare_exchange_strong(expected, nx, std::memory_order_release,
                                                    std::memory_order_relaxed))
                hz.retire(rec, s, &retire_deleter);
            return hz.protect(rec, this->head_);
        }

        bool take_cell(Segment* s, size_t pos, Cell& c, T& out)
        {
            uint8_t st = c.state.load(std::memory_order_acquire);
            while (st == kEmpty)
            {
                size_t e = s->enq_pos.load(std::memory_order_acquire);
                if (e > pos)
                {
                    cpu_relax();
                    st = c.state.load(std::memory_order_acquire);
                    continue;
                }
                if (s->enq_pos.compare_exchange_weak(e, pos + 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
                {
                    for (size_t i = e; i <= pos; ++i)
                        s->cells[i].state.store(kDead, std::memory_order_release);
                    return false;
                }
            }
            if (st == kDead)
                return false;
            consume_cell(c, out);
            return true;
        }

        alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<Segment*> head_{nullptr};
        alignas(data_structures::metadata::CACHELINE_SIZE) std::atomic<Segment*> tail_{nullptr};
        char _pad_[data_structures::metadata::CACHELINE_SIZE - sizeof(std::atomic<Segment*>)]{};
    };
} // namespace usub::queue::concurrent

#endif // MPMCQUEUE_H
