// fs::File on Windows: every operation runs on the blocking pool (there is no NOWAIT probe and no io_uring;
// overlapped ReadFile/WriteFile through IOCP is a later step). Positional I/O uses the OVERLAPPED offset on a
// synchronous handle, which Windows honours without FILE_FLAG_OVERLAPPED.
#ifdef _WIN32

#include "uvent/fs/Fs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

#include "uvent/blocking/Blocking.h"
#include "uvent/system/SystemContext.h"

namespace usub::uvent::fs
{
    namespace
    {
        bool cancel_now() noexcept { return system::this_coroutine::cancel_requested(); }

        std::wstring widen(const std::string& s)
        {
            if (s.empty())
                return {};
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        int64_t filetime_ns(const FILETIME& ft) noexcept
        {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            // 100 ns ticks since 1601-01-01 -> ns since 1970-01-01
            return (static_cast<int64_t>(u.QuadPart) - 116444736000000000LL) * 100;
        }

        Metadata from_attrs(DWORD attrs, uint64_t size, const FILETIME& mtime, bool reparse_is_symlink) noexcept
        {
            Metadata m;
            m.size = size;
            if (reparse_is_symlink && (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
                m.type = FileType::Symlink;
            else if (attrs & FILE_ATTRIBUTE_DIRECTORY)
                m.type = FileType::Directory;
            else
                m.type = FileType::Regular;
            m.mtime_ns = filetime_ns(mtime);
            m.permissions = (attrs & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
            return m;
        }

        Result<Metadata> metadata_of(HANDLE h) noexcept
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(h, &info))
                return std::unexpected(os_error(static_cast<int>(::GetLastError())));
            const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            return from_attrs(info.dwFileAttributes, size, info.ftLastWriteTime, false);
        }
    } // namespace

    File::File(File&& o) noexcept : h_(o.h_), mode_(o.mode_), direct_(o.direct_), sync_(o.sync_) { o.h_ = kInvalid; }

    File& File::operator=(File&& o) noexcept
    {
        if (this != &o)
        {
            if (this->h_ != kInvalid)
                ::CloseHandle(this->h_);
            this->h_ = std::exchange(o.h_, kInvalid);
            this->mode_ = o.mode_;
            this->direct_ = o.direct_;
            this->sync_ = o.sync_;
        }
        return *this;
    }

    File::~File()
    {
        if (this->h_ != kInvalid)
            ::CloseHandle(this->h_);
    }

    File File::from_native(native_handle_type h, OpenMode mode) noexcept
    {
        File f;
        f.h_ = h;
        f.mode_ = mode;
        f.direct_ = has(mode, OpenMode::Direct);
        f.sync_ = has(mode, OpenMode::Sync);
        return f;
    }

    File::native_handle_type File::release() noexcept { return std::exchange(this->h_, kInvalid); }

    task::Awaitable<Result<File>> File::async_open(std::string path, OpenMode mode, int)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        auto r = co_await blocking::run(
            [p = std::move(path), mode]() -> Result<HANDLE>
            {
                DWORD access = 0;
                if (has(mode, OpenMode::Read))
                    access |= GENERIC_READ;
                if (has(mode, OpenMode::Write))
                    access |= GENERIC_WRITE;
                DWORD creation;
                const bool create = has(mode, OpenMode::Create), trunc = has(mode, OpenMode::Truncate);
                if (has(mode, OpenMode::CreateNew))
                    creation = CREATE_NEW;
                else if (create && trunc)
                    creation = CREATE_ALWAYS;
                else if (create)
                    creation = OPEN_ALWAYS;
                else if (trunc)
                    creation = TRUNCATE_EXISTING;
                else
                    creation = OPEN_EXISTING;
                DWORD flags = FILE_ATTRIBUTE_NORMAL;
                if (has(mode, OpenMode::Direct))
                    flags |= FILE_FLAG_NO_BUFFERING;
                if (has(mode, OpenMode::Sync))
                    flags |= FILE_FLAG_WRITE_THROUGH;
                HANDLE h = ::CreateFileW(widen(p).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, creation, flags, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    return std::unexpected(os_error(static_cast<int>(::GetLastError())));
                return h;
            });
        if (!r)
            co_return std::unexpected(r.error());
        co_return File::from_native(*r, mode);
    }

    task::Awaitable<Result<std::size_t>> File::async_read_at(std::span<std::byte> buf, uint64_t offset)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        if (buf.empty())
            co_return std::size_t{0};
        const HANDLE h = this->h_;
        co_return co_await blocking::run(
            [h, buf, offset]() -> Result<std::size_t>
            {
                OVERLAPPED ov{};
                ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
                ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
                DWORD n = 0;
                const DWORD want = buf.size() > 0x7fffffffu ? 0x7fffffffu : static_cast<DWORD>(buf.size());
                if (!::ReadFile(h, buf.data(), want, &n, &ov))
                {
                    const DWORD e = ::GetLastError();
                    if (e == ERROR_HANDLE_EOF)
                        return std::size_t{0};
                    return std::unexpected(os_error(static_cast<int>(e)));
                }
                return static_cast<std::size_t>(n);
            });
    }

    task::Awaitable<Result<std::size_t>> File::async_write_at(std::span<const std::byte> buf, uint64_t offset)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        if (buf.empty())
            co_return std::size_t{0};
        const HANDLE h = this->h_;
        co_return co_await blocking::run(
            [h, buf, offset]() -> Result<std::size_t>
            {
                OVERLAPPED ov{};
                ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
                ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
                DWORD n = 0;
                const DWORD want = buf.size() > 0x7fffffffu ? 0x7fffffffu : static_cast<DWORD>(buf.size());
                if (!::WriteFile(h, buf.data(), want, &n, &ov))
                    return std::unexpected(os_error(static_cast<int>(::GetLastError())));
                return static_cast<std::size_t>(n);
            });
    }

    task::Awaitable<Result<void>> File::async_sync_all()
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const HANDLE h = this->h_;
        co_return co_await blocking::run(
            [h]() -> Result<void>
            {
                if (!::FlushFileBuffers(h))
                    return std::unexpected(os_error(static_cast<int>(::GetLastError())));
                return Result<void>{};
            });
    }

    task::Awaitable<Result<void>> File::async_sync_data() { co_return co_await this->async_sync_all(); }

    task::Awaitable<Result<void>> File::async_set_len(uint64_t size)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const HANDLE h = this->h_;
        co_return co_await blocking::run(
            [h, size]() -> Result<void>
            {
                FILE_END_OF_FILE_INFO info{};
                info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
                if (!::SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof info))
                    return std::unexpected(os_error(static_cast<int>(::GetLastError())));
                return Result<void>{};
            });
    }

    task::Awaitable<Result<Metadata>> File::async_metadata()
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const HANDLE h = this->h_;
        co_return co_await blocking::run([h]() -> Result<Metadata> { return metadata_of(h); });
    }

    task::Awaitable<Result<void>> File::async_close()
    {
        if (this->h_ == kInvalid)
            co_return Result<void>{};
        const HANDLE h = std::exchange(this->h_, kInvalid);
        co_return co_await blocking::run(
            [h]() -> Result<void>
            {
                if (!::CloseHandle(h))
                    return std::unexpected(os_error(static_cast<int>(::GetLastError())));
                return Result<void>{};
            });
    }

    namespace detail
    {
        Result<Metadata> stat_path(const std::string& path, bool follow) noexcept
        {
            const std::wstring w = widen(path);
            if (!follow)
            {
                WIN32_FILE_ATTRIBUTE_DATA d{};
                if (!::GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d))
                    return std::unexpected(os_error(static_cast<int>(::GetLastError())));
                const uint64_t size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
                return from_attrs(d.dwFileAttributes, size, d.ftLastWriteTime, true);
            }
            HANDLE h = ::CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return std::unexpected(os_error(static_cast<int>(::GetLastError())));
            auto r = metadata_of(h);
            ::CloseHandle(h);
            return r;
        }
    } // namespace detail
} // namespace usub::uvent::fs

#endif // _WIN32
