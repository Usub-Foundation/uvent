// fs::Mapping on Windows: CreateFileMapping / MapViewOfFile inline, PrefetchVirtualMemory + page touch and
// FlushViewOfFile + FlushFileBuffers on the blocking pool, QueryWorkingSetEx for residency.
#ifdef _WIN32

#include "uvent/fs/Mapping.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>
#include <vector>

#include "uvent/blocking/Blocking.h"
#include "uvent/system/SystemContext.h"

namespace usub::uvent::fs
{
    namespace
    {
        bool cancel_now() noexcept { return system::this_coroutine::cancel_requested(); }

        std::error_code last_error() noexcept { return os_error(static_cast<int>(::GetLastError())); }

        const SYSTEM_INFO& sysinfo() noexcept
        {
            static const SYSTEM_INFO si = []
            {
                SYSTEM_INFO s{};
                ::GetSystemInfo(&s);
                return s;
            }();
            return si;
        }

        std::wstring widen(const std::string& s)
        {
            if (s.empty())
                return {};
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        // Resolved at run time so the library links on toolchains whose headers predate Windows 8.
        struct WIN32_MEMORY_RANGE_ENTRY_
        {
            PVOID VirtualAddress;
            SIZE_T NumberOfBytes;
        };
        using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, WIN32_MEMORY_RANGE_ENTRY_*, ULONG);
        using QueryWsFn = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);

        template <class Fn>
        Fn kernel32_fn(const char* name) noexcept
        {
            static const Fn fn = reinterpret_cast<Fn>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), name));
            return fn;
        }

        void touch_pages(const std::byte* addr, std::size_t n, std::size_t ps) noexcept
        {
            const volatile std::byte* p = addr;
            for (std::size_t i = 0; i < n; i += ps)
                (void)p[i];
            if (n)
                (void)p[n - 1];
        }
    } // namespace

    std::size_t Mapping::page_size() noexcept { return sysinfo().dwPageSize; }

    Mapping::Mapping(Mapping&& o) noexcept
        : base_(o.base_), base_len_(o.base_len_), data_(o.data_), len_(o.len_), file_offset_(o.file_offset_),
          access_(o.access_), file_(o.file_), section_(o.section_)
    {
        o.base_ = o.data_ = nullptr;
        o.base_len_ = o.len_ = 0;
        o.file_ = INVALID_HANDLE_VALUE;
        o.section_ = nullptr;
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
            this->file_ = std::exchange(o.file_, static_cast<void*>(INVALID_HANDLE_VALUE));
            this->section_ = std::exchange(o.section_, nullptr);
        }
        return *this;
    }

    Mapping::~Mapping() { this->unmap(); }

    void Mapping::unmap() noexcept
    {
        if (this->base_)
            ::UnmapViewOfFile(this->base_);
        if (this->section_)
            ::CloseHandle(this->section_);
        if (this->file_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(this->file_);
        this->base_ = this->data_ = nullptr;
        this->base_len_ = this->len_ = 0;
        this->section_ = nullptr;
        this->file_ = INVALID_HANDLE_VALUE;
    }

    Result<Mapping> Mapping::map(const File& file, MapAccess access, uint64_t offset, std::size_t len) noexcept
    {
        if (!file.is_open())
            return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
        const HANDLE h = file.native_handle();
        LARGE_INTEGER sz{};
        if (!::GetFileSizeEx(h, &sz))
            return std::unexpected(last_error());
        const uint64_t size = static_cast<uint64_t>(sz.QuadPart);
        if (offset > size)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        if (len == 0)
            len = static_cast<std::size_t>(size - offset);
        if (len == 0 || offset + len > size)
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        const uint64_t gran = sysinfo().dwAllocationGranularity;
        const uint64_t aligned_off = offset & ~(gran - 1);
        const std::size_t delta = static_cast<std::size_t>(offset - aligned_off);
        const std::size_t base_len = delta + len;

        DWORD protect, view;
        switch (access)
        {
        case MapAccess::ReadWrite: protect = PAGE_READWRITE; view = FILE_MAP_READ | FILE_MAP_WRITE; break;
        case MapAccess::Private: protect = PAGE_WRITECOPY; view = FILE_MAP_COPY; break;
        default: protect = PAGE_READONLY; view = FILE_MAP_READ; break;
        }
        HANDLE section = ::CreateFileMappingW(h, nullptr, protect, 0, 0, nullptr);
        if (!section)
            return std::unexpected(last_error());
        void* p = ::MapViewOfFile(section, view, static_cast<DWORD>(aligned_off >> 32),
                                  static_cast<DWORD>(aligned_off & 0xffffffffu), base_len);
        if (!p)
        {
            const auto ec = last_error();
            ::CloseHandle(section);
            return std::unexpected(ec);
        }
        Mapping m;
        m.base_ = static_cast<std::byte*>(p);
        m.base_len_ = base_len;
        m.data_ = m.base_ + delta;
        m.len_ = len;
        m.file_offset_ = offset;
        m.access_ = access;
        m.section_ = section;
        if (access == MapAccess::ReadWrite)
        {
            HANDLE dup = INVALID_HANDLE_VALUE;
            if (::DuplicateHandle(::GetCurrentProcess(), h, ::GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
                m.file_ = dup; // for FlushFileBuffers in async_flush; without it flush is view-only
        }
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
                const DWORD acc = GENERIC_READ | (access == MapAccess::ReadWrite ? GENERIC_WRITE : 0);
                HANDLE h = ::CreateFileW(widen(p).c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    return std::unexpected(last_error());
                const File f = File::from_native(h, access == MapAccess::ReadWrite ? OpenMode::ReadWrite : OpenMode::Read);
                return Mapping::map(f, access, offset, len);
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
        const std::size_t ps = sysinfo().dwPageSize;
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
        const std::size_t ps = sysinfo().dwPageSize;
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
                if (auto fn = kernel32_fn<PrefetchFn>("PrefetchVirtualMemory"))
                {
                    WIN32_MEMORY_RANGE_ENTRY_ e{addr, n};
                    (void)fn(::GetCurrentProcess(), 1, &e, 0); // asynchronous hint; touching below waits for it
                }
                touch_pages(addr, n, sysinfo().dwPageSize);
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
        const HANDLE file = this->file_;
        co_return co_await blocking::run(
            [addr, n, file]() -> Result<void>
            {
                if (!::FlushViewOfFile(addr, n))
                    return std::unexpected(last_error());
                if (file != INVALID_HANDLE_VALUE && !::FlushFileBuffers(file))
                    return std::unexpected(last_error());
                return Result<void>{};
            });
    }

    Result<std::size_t> Mapping::resident_pages(std::size_t off, std::size_t len) const noexcept
    {
        std::byte* addr;
        std::size_t n;
        if (!this->page_range(off, len, addr, n))
            return std::size_t{0};
        auto fn = kernel32_fn<QueryWsFn>("K32QueryWorkingSetEx");
        if (!fn)
            return std::unexpected(std::make_error_code(std::errc::function_not_supported));
        const std::size_t ps = sysinfo().dwPageSize;
        const std::size_t pages = (n + ps - 1) / ps;
        struct Entry // PSAPI_WORKING_SET_EX_INFORMATION without pulling in psapi.h
        {
            PVOID VirtualAddress;
            ULONG_PTR VirtualAttributes;
        };
        std::vector<Entry> v;
        try
        {
            v.resize(pages);
        }
        catch (...)
        {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
        for (std::size_t i = 0; i < pages; ++i)
            v[i].VirtualAddress = addr + i * ps;
        if (!fn(::GetCurrentProcess(), v.data(), static_cast<DWORD>(v.size() * sizeof(Entry))))
            return std::unexpected(last_error());
        std::size_t resident = 0;
        for (const auto& e : v)
            resident += (e.VirtualAttributes & 1u); // bit 0: Valid
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
        if (advice == MapAdvice::DontNeed)
        {
            // Drop the pages from the working set; they are re-faulted from the section on next touch.
            if (!::VirtualUnlock(addr, n) && ::GetLastError() != ERROR_NOT_LOCKED)
                return std::unexpected(last_error());
        }
        return Result<void>{}; // Sequential / Random: no per-range equivalent on Windows
    }
} // namespace usub::uvent::fs

#endif // _WIN32
