//
// Created by Kirill Zhukov on 20.10.2025.
//

// inspired by: https://www.stroustrup.com/lock-free-vector.pdf

#ifndef CONCURRENTVECTOR_H
#define CONCURRENTVECTOR_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include <uvent/utils/datastructures/DataStructuresMetadata.h>
#include <uvent/utils/intrinsics/optimizations.h>

#include <uvent/system/Defines.h>

// ~10.423 Mops/s

namespace usub::array::concurrent {
static inline constexpr std::size_t pow2(std::size_t e) {
  constexpr std::size_t W = 8u * sizeof(std::size_t);
  if (e >= W) return std::size_t(1) << (W - 1);
  return std::size_t(1) << e;
}
// S(L)=2^{L+1}-1, L=floor(log2(index+1))
static inline std::pair<std::size_t, std::size_t>
locate(std::size_t index) noexcept {
  std::size_t L =
      (index == 0) ? 0
                   : (8u * sizeof(std::size_t) - 1u - clzl_portable(index + 1));
  std::size_t base = (pow2(L) - 1);
  std::size_t off = index - base;
  return {L, off};
}

#ifdef _WIN32
  static inline void* win_alloc_pages(size_t bytes, size_t align) {
    size_t page = 64 * 1024;
    size_t need = (bytes + page - 1) & ~(page - 1);
    void* p = ::VirtualAlloc(nullptr, need, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p;
  }
  static void win_free_pages(void* p) {
    if (p) ::VirtualFree(p, 0, MEM_RELEASE);
  }
#endif

template <class T>
struct alignas(data_structures::metadata::CACHELINE_SIZE) Cell {
  enum : uint8_t { EMPTY = 0, WRITING = 1, READY = 2, DELETING = 3 };

  std::atomic<uint8_t> state{EMPTY};
  alignas(T) unsigned char storage[sizeof(T)];

  T *ptr() noexcept {
    return std::launder(reinterpret_cast<T *>(this->storage));
  }
  const T *ptr() const noexcept {
    return std::launder(reinterpret_cast<const T *>(this->storage));
  }

  void destroy_if_ready() noexcept {
    if (this->state.load(std::memory_order_acquire) == READY) {
      std::destroy_at(ptr());
      this->state.store(EMPTY, std::memory_order_release);
    }
  }
};

template <class T> struct Bucket {
  std::size_t cap{};
  Cell<T> *cells{};

  static Bucket *allocate(std::size_t cap) {
    if (!cap) cap = 1;

    void* mem = ::operator new(sizeof(Bucket));
    auto* b = new (mem) Bucket{cap, nullptr};

#ifdef _WIN32
    void* raw = win_alloc_pages(cap * sizeof(Cell<T>), alignof(Cell<T>));
    if (!raw) throw std::bad_alloc{};
    b->cells = static_cast<Cell<T>*>(raw);
#else
    void* raw = ::operator new[](cap * sizeof(Cell<T>), std::align_val_t{alignof(Cell<T>)});
    b->cells = static_cast<Cell<T>*>(raw);
#endif

    for (std::size_t i = 0; i < cap; ++i)
      std::construct_at(std::addressof(b->cells[i]));
    return b;
  }

  static void destroy(Bucket* b) {
    for (std::size_t i = 0; i < b->cap; ++i)
      std::destroy_at(std::addressof(b->cells[i]));
#ifdef _WIN32
    win_free_pages(b->cells);
#else
    ::operator delete[](b->cells, std::align_val_t{alignof(Cell<T>)});
#endif
    b->~Bucket();
    ::operator delete(b);
  }
};

template <class T, std::size_t MAX_LEVELS = (8 * sizeof(std::size_t))>
class LockFreeVector {
public:
  LockFreeVector() {
    for (auto &a : this->buckets_)
      a.store(nullptr, std::memory_order_relaxed);
    this->alloc_idx_.store(0, std::memory_order_relaxed);
    this->published_.store(0, std::memory_order_relaxed);
  }

  ~LockFreeVector() {
    const std::size_t n = this->published_.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
      auto [L, off] = locate(i);
      if (auto *b = this->buckets_[L].load(std::memory_order_acquire))
        b->cells[off].destroy_if_ready();
    }
    for (auto &a : this->buckets_)
      if (auto *b = a.load(std::memory_order_acquire))
        Bucket<T>::destroy(b);
  }

  LockFreeVector(const LockFreeVector &) = delete;
  LockFreeVector &operator=(const LockFreeVector &) = delete;

  template <class... Args>
  std::size_t emplace_back(Args&&... args) {
    const std::size_t idx = alloc_idx_.fetch_add(1, std::memory_order_acq_rel);

    auto [L, off] = locate(idx);
    if (!ensure_bucket(L)) [[unlikely]]
        throw std::bad_alloc();

    Bucket<T> *b = this->buckets_[L].load(std::memory_order_acquire);
    Cell<T> &c = b->cells[off];

    uint8_t st = c.state.load(std::memory_order_acquire);
    while (st != Cell<T>::EMPTY) {
      cpu_relax();
      st = c.state.load(std::memory_order_acquire);
    }
    if (c.state.compare_exchange_strong(st, Cell<T>::WRITING,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
      prefetch_for_write(c.ptr());
      std::construct_at(c.ptr(), std::forward<Args>(args)...);
      std::atomic_thread_fence(std::memory_order_release);
      c.state.store(Cell<T>::READY, std::memory_order_release);
    } else {
      while (c.state.load(std::memory_order_acquire) != Cell<T>::READY)
        cpu_relax();
    }

    const std::size_t pub = this->published_.load(std::memory_order_acquire);
    if (idx == pub)
      advance_published();

    return idx;
  }

  bool pop_back() {
    for (;;) {
      std::size_t pub = this->published_.load(std::memory_order_acquire);
      if (pub == 0)
        return false;
      if (this->published_.compare_exchange_weak(pub, pub - 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        const std::size_t idx = pub - 1;
        auto [L, off] = locate(idx);
        Bucket<T> *b = buckets_[L].load(std::memory_order_acquire);
        Cell<T> &c = b->cells[off];
        while (c.state.load(std::memory_order_acquire) != Cell<T>::READY)
          cpu_relax();
        uint8_t st = Cell<T>::READY;
        if (c.state.compare_exchange_strong(st, Cell<T>::DELETING,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
          std::destroy_at(c.ptr());
          c.state.store(Cell<T>::EMPTY, std::memory_order_release);
        }
        return true;
      }
      cpu_relax();
    }
  }

  bool erase(std::size_t i) {
    std::size_t pub = this->published_.load(std::memory_order_acquire);
    if (i >= pub)
      return false;

    auto [L, off] = locate(i);

    Bucket<T> *b = this->buckets_[L].load(std::memory_order_acquire);
    if (!b)
      return false;

    Cell<T> &c = b->cells[off];

    uint8_t st = c.state.load(std::memory_order_acquire);
    for (;;) {
      if (st == Cell<T>::READY)
        break;
      if (st == Cell<T>::EMPTY)
        return false;
      if (st == Cell<T>::DELETING) {
        while (c.state.load(std::memory_order_acquire) == Cell<T>::DELETING)
          cpu_relax();
        return false;
      }
      cpu_relax();
      st = c.state.load(std::memory_order_acquire);
    }

    st = Cell<T>::READY;
    if (!c.state.compare_exchange_strong(st, Cell<T>::DELETING,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
      return false;

    std::destroy_at(c.ptr());

    c.state.store(Cell<T>::EMPTY, std::memory_order_release);

    shrink_published_tail_();

    return true;
  }

  T &operator[](std::size_t i) noexcept {
    auto [L, off] = locate(i);
    Bucket<T> *b = nullptr;
    while ((b = this->buckets_[L].load(std::memory_order_acquire)) == nullptr)
      cpu_relax();
    Cell<T> &c = b->cells[off];

    for (;;) {
      uint8_t st = c.state.load(std::memory_order_acquire);
      if (st == Cell<T>::READY) {
        prefetch_for_read(c.ptr());
        return *c.ptr();
      }
      if (st == Cell<T>::EMPTY)
        TRAP();
      cpu_relax();
    }
  }

  const T &operator[](std::size_t i) const noexcept {
    auto [L, off] = locate(i);
    Bucket<T> *b = nullptr;
    while ((b = this->buckets_[L].load(std::memory_order_acquire)) == nullptr)
      cpu_relax();
    const Cell<T> &c = b->cells[off];

    for (;;) {
      uint8_t st = c.state.load(std::memory_order_acquire);
      if (st == Cell<T>::READY) {
        prefetch_for_read(c.ptr());
        return *c.ptr();
      }
      if (st == Cell<T>::EMPTY)
        TRAP();
      cpu_relax();
    }
  }

  T &at(std::size_t i) {
    std::size_t sz = size();
    if (i >= sz)
      TRAP();
    return (*this)[i];
  }

  const T &at(std::size_t i) const {
    std::size_t sz = size();
    if (i >= sz)
      TRAP();
    return (*this)[i];
  }

  void reserve(std::size_t n) {
    if (!n) return;

#ifdef _WIN32
    constexpr std::size_t SOFT_MAX = (1ull << 26);
#else
    constexpr std::size_t SOFT_MAX = (1ull << 27);
#endif
    if (n > SOFT_MAX) n = SOFT_MAX;

    auto [L, _] = locate(n - 1);
    for (std::size_t i = 0; i <= L; ++i) {
      if (this->buckets_[i].load(std::memory_order_acquire)) continue;
      if (!ensure_bucket(i)) break;
    }
  }

  std::size_t size() const noexcept {
    return this->published_.load(std::memory_order_acquire);
  }
  bool empty() const noexcept { return size() == 0; }

private:
  bool ensure_bucket(std::size_t L) {
    if (this->buckets_[L].load(std::memory_order_acquire)) return true;

#ifdef _WIN32
    constexpr std::size_t MAX_BUCKET = (1ull << 22);
#else
    constexpr std::size_t MAX_BUCKET = (1ull << 24);
#endif

    std::size_t cap = pow2(L);
    if (cap > MAX_BUCKET) return false;

    Bucket<T>* cand = nullptr;
    try {
      cand = Bucket<T>::allocate(cap);
    } catch (...) {
      return false;
    }

    Bucket<T>* exp = nullptr;
    if (!this->buckets_[L].compare_exchange_strong(
            exp, cand, std::memory_order_acq_rel, std::memory_order_acquire)) {
      Bucket<T>::destroy(cand);
            }
    return true;
  }

  void advance_published() noexcept {
    for (;;) {
      std::size_t cur = this->published_.load(std::memory_order_acquire);

      auto [L0, off0] = locate(cur);
      Bucket<T> *b0 = this->buckets_[L0].load(std::memory_order_acquire);
      if (!b0)
        break;
      if (b0->cells[off0].state.load(std::memory_order_acquire) !=
          Cell<T>::READY)
        break;

      std::size_t scan = cur;
      for (;;) {
        auto [L, off] = locate(scan);
        Bucket<T> *b = this->buckets_[L].load(std::memory_order_acquire);
        if (!b)
          break;
        if (b->cells[off].state.load(std::memory_order_acquire) !=
            Cell<T>::READY)
          break;
        ++scan;
      }

      if (this->published_.compare_exchange_weak(cur, scan,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        continue;
      }
      cpu_relax();
    }
  }

  void shrink_published_tail_() noexcept {
    for (;;) {
      std::size_t pub = this->published_.load(std::memory_order_acquire);
      if (pub == 0)
        return;

      std::size_t idx = pub - 1;

      auto [L, off] = locate(idx);
      Bucket<T> *b = this->buckets_[L].load(std::memory_order_acquire);
      if (!b)
        return;

      Cell<T> &c = b->cells[off];

      uint8_t st = c.state.load(std::memory_order_acquire);

      if (st == Cell<T>::READY || st == Cell<T>::WRITING ||
          st == Cell<T>::DELETING)
        return;

      if (this->published_.compare_exchange_weak(pub, pub - 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
        continue;

      cpu_relax();
    }
  }

private:
  alignas(usub::data_structures::metadata::CACHELINE_SIZE)
      std::atomic<std::size_t> alloc_idx_{0};

  alignas(usub::data_structures::metadata::CACHELINE_SIZE)
      std::atomic<std::size_t> published_{0};

  alignas(usub::data_structures::metadata::CACHELINE_SIZE)
      std::atomic<Bucket<T> *> buckets_[MAX_LEVELS]{};
};
} // namespace usub::array::concurrent

#endif // CONCURRENTVECTOR_H
