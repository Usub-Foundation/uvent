#ifndef UVENT_FS_MAPPING_H
#define UVENT_FS_MAPPING_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include "uvent/fs/Fs.h"

/**
 * \file Mapping.h
 * \brief `fs::Mapping` – a memory-mapped file range as a `std::span` (memmap2 shape), for hot read-mostly data.
 *
 * A mapping is the fastest way to read data that is already in memory: a resident page costs ~0.5 µs per 4 KiB
 * access with no syscall and no copy, and a sequential scan runs at memory speed (measured 10 GB/s vs 7 GB/s for
 * `pread`, see docs/fs.md). It is deliberately **not** the engine behind `File::async_read_at`, because a page
 * that is not resident blocks the worker synchronously inside the load instruction, a write to a shared mapping
 * can stall in dirty-page throttling, and an I/O error or a file shortened by another process arrives as SIGBUS
 * rather than an `error_code`. The contract is therefore:
 *
 *   - `co_await m.async_prefetch(off, len)` before touching a range that may be cold (runs on the blocking pool:
 *     `MADV_POPULATE_READ` on Linux ≥ 5.14, `MADV_WILLNEED` + page touch elsewhere, `PrefetchVirtualMemory` + touch
 *     on Windows). Prefetched pages can still be evicted under memory pressure – the runtime cannot prevent a
 *     later fault, only make it unlikely.
 *   - `m.resident_pages(off, len)` / `m.is_resident(off, len)` is a hint (`mincore` / `QueryWorkingSetEx`, one
 *     syscall, may be stale by the time you read).
 *   - `co_await m.async_flush(off, len)` writes dirty pages of a `ReadWrite` mapping back and waits for the device
 *     (`msync(MS_SYNC)`; `FlushViewOfFile` + `FlushFileBuffers` on Windows). A `Private` mapping never writes back.
 *   - The mapped range must lie inside the file; the file must not be shortened while mapped. Growing a file
 *     needs `File::async_set_len` and a new mapping (a mapping never resizes).
 *
 * Mapping and unmapping are cheap syscalls that do no I/O (page tables only), so `map(const File&)` is synchronous;
 * `async_map(path)` opens, maps and closes on the pool in one job. Move-only; the destructor unmaps without
 * flushing (call `async_flush` first for durability).
 */
namespace usub::uvent::fs
{
    enum class MapAccess : uint8_t
    {
        /// PROT_READ, MAP_SHARED: writes through the span are undefined (SIGSEGV).
        Read,
        /// PROT_READ | PROT_WRITE, MAP_SHARED: writes reach the file (after write-back or `async_flush`).
        ReadWrite,
        /// PROT_READ | PROT_WRITE, MAP_PRIVATE (copy-on-write): writes stay in this process, the file is untouched.
        Private
    };

    enum class MapAdvice : uint8_t
    {
        Normal,     ///< MADV_NORMAL
        Sequential, ///< MADV_SEQUENTIAL: aggressive read-ahead, drop pages behind
        Random,     ///< MADV_RANDOM: no read-ahead
        DontNeed    ///< MADV_DONTNEED: drop the pages from this mapping (they are re-read on next touch)
    };

    class Mapping
    {
    public:
        static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

        Mapping() = default;
        Mapping(Mapping&& o) noexcept;
        Mapping& operator=(Mapping&& o) noexcept;
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;
        ~Mapping();

        /**
         * \brief Map `[offset, offset + len)` of an open file; `len == 0` means "to the end of the file".
         *
         * `offset` may be unaligned (the mapping is widened to page / allocation granularity internally and
         * `data()` starts at the requested byte). Fails with `invalid_argument` when the range is empty or extends
         * past the end of the file, and with the OS error otherwise. `ReadWrite` needs a file opened for writing.
         */
        static Result<Mapping> map(const File& file, MapAccess access = MapAccess::Read, uint64_t offset = 0,
                                   std::size_t len = 0) noexcept;

        /// Open `path` (read-only for `Read` / `Private`, read-write for `ReadWrite`), map, close — one pool job.
        static task::Awaitable<Result<Mapping>> async_map(std::string path, MapAccess access = MapAccess::Read,
                                                          uint64_t offset = 0, std::size_t len = 0);

        [[nodiscard]] std::span<std::byte> data() noexcept { return {this->data_, this->len_}; }
        [[nodiscard]] std::span<const std::byte> data() const noexcept { return {this->data_, this->len_}; }
        [[nodiscard]] std::size_t size() const noexcept { return this->len_; }
        [[nodiscard]] bool is_mapped() const noexcept { return this->data_ != nullptr; }
        /// File offset of `data()[0]`.
        [[nodiscard]] uint64_t offset() const noexcept { return this->file_offset_; }
        [[nodiscard]] MapAccess access() const noexcept { return this->access_; }

        /// Bring `[off, off + len)` of the mapping into memory (blocking pool). Whole mapping by default.
        task::Awaitable<Result<void>> async_prefetch(std::size_t off = 0, std::size_t len = npos);

        /// Write back dirty pages of `[off, off + len)` and wait for the device (blocking pool). No-op for `Read`
        /// and `Private` mappings.
        task::Awaitable<Result<void>> async_flush(std::size_t off = 0, std::size_t len = npos);

        /// Number of pages of `[off, off + len)` currently resident (a hint). `page_count(off, len)` is the total.
        [[nodiscard]] Result<std::size_t> resident_pages(std::size_t off = 0, std::size_t len = npos) const noexcept;
        [[nodiscard]] std::size_t page_count(std::size_t off = 0, std::size_t len = npos) const noexcept;
        [[nodiscard]] bool is_resident(std::size_t off = 0, std::size_t len = npos) const noexcept;

        /// Access-pattern hint for `[off, off + len)` (madvise; ignored on Windows except `DontNeed`).
        Result<void> advise(MapAdvice advice, std::size_t off = 0, std::size_t len = npos) noexcept;

        /// Unmap now (without flushing). Idempotent.
        void unmap() noexcept;

        static std::size_t page_size() noexcept;

    private:
        // Clamp a user range to [0, len_) on page boundaries of the underlying map; returns false if empty.
        bool page_range(std::size_t off, std::size_t len, std::byte*& addr, std::size_t& n) const noexcept;

        std::byte* base_{nullptr};   // page-aligned start of the OS mapping
        std::size_t base_len_{0};    // length of the OS mapping
        std::byte* data_{nullptr};   // base_ + (offset % granularity)
        std::size_t len_{0};         // user-visible length
        uint64_t file_offset_{0};
        MapAccess access_{MapAccess::Read};
#ifdef _WIN32
        void* file_{reinterpret_cast<void*>(-1)}; // duplicated HANDLE for FlushFileBuffers
        void* section_{nullptr};                  // file-mapping object
#endif
    };
} // namespace usub::uvent::fs

#endif // UVENT_FS_MAPPING_H
