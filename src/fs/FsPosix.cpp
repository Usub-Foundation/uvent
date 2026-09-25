// fs::File on POSIX (Linux, BSD, macOS). Engines, in order of preference for each operation:
//   read_at  : preadv2(RWF_NOWAIT) inline (Linux, buffered files) -> io_uring READ (UVENT_ENABLE_IO_URING) -> pool
//   write_at : pwrite inline (buffered, non-O_SYNC files, settings::fs_inline_buffered_write) -> io_uring WRITE -> pool
//   sync     : io_uring FSYNC -> pool
//   open / metadata / set_len / close : pool
#ifndef _WIN32

#include "uvent/fs/Fs.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

#include "uvent/blocking/Blocking.h"
#include "uvent/system/Settings.h"
#include "uvent/system/SystemContext.h"

#if defined(__linux__) && defined(UVENT_ENABLE_IO_URING)
#include "uvent/poll/IOUringPoller.h"
#endif

namespace usub::uvent::fs
{
    namespace
    {
        bool cancel_now() noexcept { return system::this_coroutine::cancel_requested(); }

        int open_flags(OpenMode m) noexcept
        {
            int f = O_CLOEXEC;
            const bool r = has(m, OpenMode::Read), w = has(m, OpenMode::Write);
            f |= (r && w) ? O_RDWR : (w ? O_WRONLY : O_RDONLY);
            if (has(m, OpenMode::Create))
                f |= O_CREAT;
            if (has(m, OpenMode::CreateNew))
                f |= O_CREAT | O_EXCL;
            if (has(m, OpenMode::Truncate))
                f |= O_TRUNC;
            if (has(m, OpenMode::Sync))
                f |= O_SYNC;
#ifdef O_DIRECT
            if (has(m, OpenMode::Direct))
                f |= O_DIRECT;
#endif
            return f;
        }

        Metadata from_stat(const struct stat& st) noexcept
        {
            Metadata m;
            m.size = static_cast<uint64_t>(st.st_size);
            if (S_ISREG(st.st_mode))
                m.type = FileType::Regular;
            else if (S_ISDIR(st.st_mode))
                m.type = FileType::Directory;
            else if (S_ISLNK(st.st_mode))
                m.type = FileType::Symlink;
            else
                m.type = FileType::Other;
#ifdef __APPLE__
            m.mtime_ns = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
            m.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
            m.permissions = st.st_mode & 07777;
            return m;
        }

        constexpr std::size_t kMaxChunk = 1u << 30; // one SQE / one pread at a time

#if defined(__linux__) && defined(UVENT_ENABLE_IO_URING)
        /// One-shot READ / WRITE / FSYNC through the worker's ring. Cancellation asks the ring to cancel the SQE but
        /// the coroutine still waits for the CQE (the buffer is in use until then); the result is then -ECANCELED
        /// or, if the op had already completed, its real result.
        struct UringFileAwaiter
        {
            enum Kind : uint8_t
            {
                Read,
                Write,
                Fsync
            };

            core::detail::IoOpBase op{};
            Kind kind{Read};
            int fd{-1};
            void* buf{nullptr};
            unsigned len{0};
            uint64_t off{0};
            bool datasync{false};

            bool await_ready() const noexcept { return false; }

            template <class Promise>
            void await_suspend(std::coroutine_handle<Promise> h)
            {
                this->op.coro = h;
                uvent::detail::frame_of(h).arm_cancel(&UringFileAwaiter::on_cancel, this, "fs.uring");
                auto& pl = static_cast<core::IOUringPoller&>(system::this_thread::detail::pl);
                switch (this->kind)
                {
                case Read: pl.submit_file_read(&this->op, this->fd, this->buf, this->len, this->off); break;
                case Write: pl.submit_file_write(&this->op, this->fd, this->buf, this->len, this->off); break;
                case Fsync: pl.submit_file_fsync(&this->op, this->fd, this->datasync); break;
                }
            }

            ssize_t await_resume() noexcept
            {
                uvent::detail::frame_of(this->op.coro).disarm_cancel();
                return this->op.res < 0 ? -static_cast<ssize_t>(this->op.err) : this->op.res;
            }

            static void on_cancel(uvent::detail::AwaitableFrameBase*, void* arg) noexcept
            {
                // Runs on the owning worker (cancel kicks are processed there): the ring is ours.
                auto* a = static_cast<UringFileAwaiter*>(arg);
                auto& pl = static_cast<core::IOUringPoller&>(system::this_thread::detail::pl);
                pl.submit_cancel(&a->op);
            }
        };
#endif

        // The "level 2" engine for a read that missed the inline path (or an O_DIRECT read).
        task::Awaitable<ssize_t> read_slow(int fd, void* buf, std::size_t len, uint64_t off)
        {
#if defined(__linux__) && defined(UVENT_ENABLE_IO_URING)
            co_return co_await UringFileAwaiter{.kind = UringFileAwaiter::Read,
                                                .fd = fd,
                                                .buf = buf,
                                                .len = static_cast<unsigned>(len),
                                                .off = off};
#else
            co_return co_await blocking::run(
                [fd, buf, len, off]() -> ssize_t
                {
                    const ssize_t n = ::pread(fd, buf, len, static_cast<off_t>(off));
                    return n < 0 ? -errno : n;
                });
#endif
        }

        task::Awaitable<ssize_t> write_slow(int fd, const void* buf, std::size_t len, uint64_t off)
        {
#if defined(__linux__) && defined(UVENT_ENABLE_IO_URING)
            co_return co_await UringFileAwaiter{.kind = UringFileAwaiter::Write,
                                                .fd = fd,
                                                .buf = const_cast<void*>(buf),
                                                .len = static_cast<unsigned>(len),
                                                .off = off};
#else
            co_return co_await blocking::run(
                [fd, buf, len, off]() -> ssize_t
                {
                    const ssize_t n = ::pwrite(fd, buf, len, static_cast<off_t>(off));
                    return n < 0 ? -errno : n;
                });
#endif
        }

        task::Awaitable<int> sync_slow(int fd, bool datasync)
        {
#if defined(__linux__) && defined(UVENT_ENABLE_IO_URING)
            const ssize_t r = co_await UringFileAwaiter{.kind = UringFileAwaiter::Fsync, .fd = fd, .datasync = datasync};
            co_return static_cast<int>(r);
#else
            co_return co_await blocking::run(
                [fd, datasync]() -> int
                {
#ifdef __APPLE__
                    (void)datasync;
                    const int r = ::fsync(fd);
#else
                    const int r = datasync ? ::fdatasync(fd) : ::fsync(fd);
#endif
                    return r < 0 ? -errno : 0;
                });
#endif
        }
    } // namespace

    File::File(File&& o) noexcept : h_(o.h_), mode_(o.mode_), direct_(o.direct_), sync_(o.sync_) { o.h_ = kInvalid; }

    File& File::operator=(File&& o) noexcept
    {
        if (this != &o)
        {
            if (this->h_ != kInvalid)
                ::close(this->h_);
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
            ::close(this->h_);
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

    task::Awaitable<Result<File>> File::async_open(std::string path, OpenMode mode, int perm)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const int flags = open_flags(mode);
        const int fd = co_await blocking::run(
            [p = std::move(path), flags, perm]() -> int
            {
                const int fd = ::open(p.c_str(), flags, perm);
                return fd < 0 ? -errno : fd;
            });
        if (fd < 0)
            co_return std::unexpected(os_error(-fd));
#ifdef __APPLE__
        if (has(mode, OpenMode::Direct))
            ::fcntl(fd, F_NOCACHE, 1); // no O_DIRECT on Darwin
#endif
        co_return File::from_native(fd, mode);
    }

    task::Awaitable<Result<std::size_t>> File::async_read_at(std::span<std::byte> buf, uint64_t offset)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        if (buf.empty())
            co_return std::size_t{0};
        const std::size_t len = buf.size() < kMaxChunk ? buf.size() : kMaxChunk;
#if defined(__linux__) && defined(RWF_NOWAIT)
        if (!this->direct_ && settings::fs_inline_nowait_read)
        {
            iovec iov{buf.data(), len};
            const ssize_t r = ::preadv2(this->h_, &iov, 1, static_cast<off_t>(offset), RWF_NOWAIT);
            if (r >= 0)
                co_return static_cast<std::size_t>(r);
            if (errno != EAGAIN && errno != EOPNOTSUPP && errno != ENOSYS)
                co_return std::unexpected(os_error(errno));
            // EAGAIN: not in the page cache — go to the engine that can wait without blocking the worker.
        }
#endif
        const ssize_t r = co_await read_slow(this->h_, buf.data(), len, offset);
        if (r < 0)
            co_return std::unexpected(os_error(static_cast<int>(-r)));
        co_return static_cast<std::size_t>(r);
    }

    task::Awaitable<Result<std::size_t>> File::async_write_at(std::span<const std::byte> buf, uint64_t offset)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        if (buf.empty())
            co_return std::size_t{0};
        const std::size_t len = buf.size() < kMaxChunk ? buf.size() : kMaxChunk;
        if (!this->direct_ && !this->sync_ && settings::fs_inline_buffered_write)
        {
            const ssize_t r = ::pwrite(this->h_, buf.data(), len, static_cast<off_t>(offset));
            if (r >= 0)
                co_return static_cast<std::size_t>(r);
            if (errno != EAGAIN && errno != EINTR)
                co_return std::unexpected(os_error(errno));
        }
        const ssize_t r = co_await write_slow(this->h_, buf.data(), len, offset);
        if (r < 0)
            co_return std::unexpected(os_error(static_cast<int>(-r)));
        co_return static_cast<std::size_t>(r);
    }

    task::Awaitable<Result<void>> File::async_sync_all()
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const int r = co_await sync_slow(this->h_, false);
        if (r < 0)
            co_return std::unexpected(os_error(-r));
        co_return Result<void>{};
    }

    task::Awaitable<Result<void>> File::async_sync_data()
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const int r = co_await sync_slow(this->h_, true);
        if (r < 0)
            co_return std::unexpected(os_error(-r));
        co_return Result<void>{};
    }

    task::Awaitable<Result<void>> File::async_set_len(uint64_t size)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const int fd = this->h_;
        const int r = co_await blocking::run([fd, size]() -> int
                                             { return ::ftruncate(fd, static_cast<off_t>(size)) < 0 ? -errno : 0; });
        if (r < 0)
            co_return std::unexpected(os_error(-r));
        co_return Result<void>{};
    }

    task::Awaitable<Result<Metadata>> File::async_metadata()
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        const int fd = this->h_;
        co_return co_await blocking::run(
            [fd]() -> Result<Metadata>
            {
                struct stat st{};
                if (::fstat(fd, &st) < 0)
                    return std::unexpected(os_error(errno));
                return from_stat(st);
            });
    }

    task::Awaitable<Result<void>> File::async_close()
    {
        if (this->h_ == kInvalid)
            co_return Result<void>{};
        const int fd = std::exchange(this->h_, kInvalid);
        const int r = co_await blocking::run([fd]() -> int { return ::close(fd) < 0 ? -errno : 0; });
        if (r < 0)
            co_return std::unexpected(os_error(-r));
        co_return Result<void>{};
    }

    namespace detail
    {
        Result<Metadata> stat_path(const std::string& path, bool follow) noexcept
        {
            struct stat st{};
            const int r = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
            if (r < 0)
                return std::unexpected(os_error(errno));
            return from_stat(st);
        }
    } // namespace detail
} // namespace usub::uvent::fs

#endif // !_WIN32
