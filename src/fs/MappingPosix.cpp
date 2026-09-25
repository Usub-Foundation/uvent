// fs::Mapping on POSIX: mmap / munmap inline (no I/O), prefetch and flush on the blocking pool, mincore inline.
#ifndef _WIN32

#include "uvent/fs/Mapping.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "uvent/blocking/Blocking.h"
#include "uvent/system/SystemContext.h"

#if defined(__linux__) && !defined(MADV_POPULATE_READ)
#define MADV_POPULATE_READ 22 // Linux 5.14; older kernels answer EINVAL and we fall back to touching pages
#endif

namespace usub::uvent::fs
{
    namespace
    {
        bool cancel_now() noexcept { return system::this_coroutine::cancel_requested(); }

        std::size_t page_size_cached() noexcept
        {
            static const std::size_t ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            return ps;
        }

        // mmap the page-aligned range covering [offset, offset + len) (len 0 = to EOF); fills base / base_len / len.
        Result<void> map_fd(int fd, MapAccess access, uint64_t offset, std::size_t& len, std::byte*& base,
                            std::size_t& base_len) noexcept
        {
            struct stat st{};
            if (::fstat(fd, &st) < 0)
                return std::unexpected(os_error(errno));
            const uint64_t size = static_cast<uint64_t>(st.st_size);
            if (offset > size)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            if (len == 0)
                len = static_cast<std::size_t>(size - offset);
            if (len == 0 || offset + len > size)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));

            const std::size_t ps = page_size_cached();
            const uint64_t aligned_off = offset & ~static_cast<uint64_t>(ps - 1);
            const std::size_t delta = static_cast<std::size_t>(offset - aligned_off);
            base_len = delta + len;

            int prot = PROT_READ, flags = MAP_SHARED;
            if (access == MapAccess::ReadWrite)
                prot |= PROT_WRITE;
            else if (access == MapAccess::Private)
            {
                prot |= PROT_WRITE;
                flags = MAP_PRIVATE;
            }
            void* p = ::mmap(nullptr, base_len, prot, flags, fd, static_cast<off_t>(aligned_off));
            if (p == MAP_FAILED)
                return std::unexpected(os_error(errno));
            base = static_cast<std::byte*>(p);
            return Result<void>{};
        }

        // Touch one byte per page so the kernel reads it in (portable prefetch after the advisory hint).
        void touch_pages(const std::byte* addr, std::size_t n, std::size_t ps) noexcept
        {
            const volatile std::byte* p = addr;
            for (std::size_t i = 0; i < n; i += ps)
                (void)p[i];
            if (n)
                (void)p[n - 1];
        }
    } // namespace

    std::size_t Mapping::page_size() noexcept { return page_size_cached(); }

    Mapping::Mapping(Mapping&& o) noexcept
        : base_(o.base_), base_len_(o.base_len_), data_(o.data_), len_(o.len_), file_offset_(o.file_offset_),
          access_(o.access_)
    {
        o.base_ = o.data_ = nullptr;
        o.base_len_ = o.len_ = 0;
    }

    Mapping& Mapping::operator=(Mapping&& o) noexcept
    {
        if (this != &o)
        {
            this->unmap();
            this->base_ = std::exchange(o.base_, nullptr);
            this->base_len_ = std::exchange(o.base_len_, 0);
            this->data_ = std::exchange(o.data_, nullptr);
            this->len_ = std::exchange(o.len_, 0);
            this->file_offset_ = o.file_offset_;
            this->access_ = o.access_;
        }
        return *this;
    }

    Mapping::~Mapping() { this->unmap(); }

    void Mapping::unmap() noexcept
    {
        if (this->base_)
            ::munmap(this->base_, this->base_len_);
        this->base_ = this->data_ = nullptr;
        this->base_len_ = this->len_ = 0;
    }

    Result<Mapping> Mapping::map(const File& file, MapAccess access, uint64_t offset, std::size_t len) noexcept
    {
        if (!file.is_open())
            return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
        Mapping m;
        std::byte* base = nullptr;
        std::size_t base_len = 0;
        auto r = map_fd(file.native_handle(), access, offset, len, base, base_len);
        if (!r)
            return std::unexpected(r.error());
        const std::size_t delta = static_cast<std::size_t>(offset % page_size_cached());
        m.base_ = base;
        m.base_len_ = base_len;
        m.data_ = base + delta;
        m.len_ = len;
        m.file_offset_ = offset;
        m.access_ = access;
        return m;
    }

    task::Awaitable<Result<Mapping>> Mapping::async_map(std::string path, MapAccess access, uint64_t offset,
                                                        std::size_t len)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path), access, offset, len]() -> Result<Mapping>
            {
                const int fd = ::open(p.c_str(), (access == MapAccess::ReadWrite ? O_RDWR : O_RDONLY) | O_CLOEXEC);
                if (fd < 0)
                    return std::unexpected(os_error(errno));
                const File f = File::from_native(fd, access == MapAccess::ReadWrite ? OpenMode::ReadWrite : OpenMode::Read);
                return Mapping::map(f, access, offset, len); // File's destructor closes fd; the mapping survives it
            });
    }

    bool Mapping::page_range(std::size_t off, std::size_t len, std::byte*& addr, std::size_t& n) const noexcept
    {
        if (!this->data_ || off >= this->len_)
            return false;
        if (len == npos || len > this->len_ - off)
            len = this->len_ - off;
        if (len == 0)
            return false;
        const std::size_t ps = page_size_cached();
        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(this->data_ + off) & ~static_cast<std::uintptr_t>(ps - 1);
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(this->data_ + off + len);
        addr = reinterpret_cast<std::byte*>(start);
        n = static_cast<std::size_t>(end - start);
        return true;
    }

    std::size_t Mapping::page_count(std::size_t off, std::size_t len) const noexcept
    {
        std::byte* addr;
        std::size_t n;
        if (!this->page_range(off, len, addr, n))
            return 0;
        const std::size_t ps = page_size_cached();
        return (n + ps - 1) / ps;
    }

    task::Awaitable<Result<void>> Mapping::async_prefetch(std::size_t off, std::size_t len)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        std::byte* addr;
        std::size_t n;
        if (!this->page_range(off, len, addr, n))
            co_return Result<void>{};
        co_return co_await blocking::run(
            [addr, n]() -> Result<void>
            {
                const std::size_t ps = page_size_cached();
#ifdef __linux__
                if (::madvise(addr, n, MADV_POPULATE_READ) == 0)
                    return Result<void>{};
                if (errno != EINVAL) // EINVAL: kernel < 5.14 — fall through to the portable path
                    return std::unexpected(os_error(errno));
#endif
                (void)::madvise(addr, n, MADV_WILLNEED); // start read-ahead for the whole range …
                touch_pages(addr, n, ps);                // ... then wait for it page by page
                return Result<void>{};
            });
    }

    task::Awaitable<Result<void>> Mapping::async_flush(std::size_t off, std::size_t len)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        if (this->access_ != MapAccess::ReadWrite)
            co_return Result<void>{};
        std::byte* addr;
        std::size_t n;
        if (!this->page_range(off, len, addr, n))
            co_return Result<void>{};
        co_return co_await blocking::run(
            [addr, n]() -> Result<void>
            {
                if (::msync(addr, n, MS_SYNC) < 0)
                    return std::unexpected(os_error(errno));
                return Result<void>{};
            });
    }

    Result<std::size_t> Mapping::resident_pages(std::size_t off, std::size_t len) const noexcept
    {
        std::byte* addr;
        std::size_t n;
        if (!this->page_range(off, len, addr, n))
            return std::size_t{0};
        const std::size_t ps = page_size_cached();
        const std::size_t pages = (n + ps - 1) / ps;
        std::vector<unsigned char> vec;
        try
        {
            vec.resize(pages);
        }
        catch (...)
        {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
#ifdef __linux__
        if (::mincore(addr, n, vec.data()) < 0)
#else
        if (::mincore(addr, n, reinterpret_cast<char*>(vec.data())) < 0)
#endif
            return std::unexpected(os_error(errno));
        std::size_t resident = 0;
        for (unsigned char v : vec)
            resident += (v & 1u);
        return resident;
    }

    bool Mapping::is_resident(std::size_t off, std::size_t len) const noexcept
    {
        auto r = this->resident_pages(off, len);
        return r && *r == this->page_count(off, len);
    }

    Result<void> Mapping::advise(MapAdvice advice, std::size_t off, std::size_t len) noexcept
    {
        std::byte* addr;
        std::size_t n;
        if (!this->page_range(off, len, addr, n))
            return Result<void>{};
        int a = MADV_NORMAL;
        switch (advice)
        {
        case MapAdvice::Normal: a = MADV_NORMAL; break;
        case MapAdvice::Sequential: a = MADV_SEQUENTIAL; break;
        case MapAdvice::Random: a = MADV_RANDOM; break;
        case MapAdvice::DontNeed: a = MADV_DONTNEED; break;
        }
        if (::madvise(addr, n, a) < 0)
            return std::unexpected(os_error(errno));
        return Result<void>{};
    }
} // namespace usub::uvent::fs

#endif // !_WIN32
