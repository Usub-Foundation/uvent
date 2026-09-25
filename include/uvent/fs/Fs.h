#ifndef UVENT_FS_FS_H
#define UVENT_FS_FS_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "uvent/system/Defines.h"
#include "uvent/tasks/Awaitable.h"

/**
 * \file Fs.h
 * \brief Asynchronous file system access: positional `fs::File` plus whole-file / directory helpers.
 *
 * One API on every platform, a different engine underneath (see docs/fs.md):
 *
 *   read_at   Linux: `preadv2(RWF_NOWAIT)` inline on the worker – a page-cache hit costs one syscall and no hop.
 *             Miss (or O_DIRECT): io_uring READ when the library is built with UVENT_ENABLE_IO_URING, otherwise
 *             the blocking pool. macOS / BSD / Windows: the blocking pool.
 *   write_at  buffered writes inline with pwrite (settings::fs_inline_buffered_write); O_DIRECT / O_SYNC writes
 *             through io_uring or the pool.
 *   sync      io_uring FSYNC or the pool.
 *   open, metadata, set_len, close, and every path-based helper: the blocking pool (one hop, like tokio::fs).
 *
 * Results are `std::expected<T, std::error_code>`; a task cancelled before an operation starts gets
 * `std::errc::operation_canceled`. An operation already handed to io_uring or the pool always runs to
 * completion — buffers passed as spans must stay alive until `co_await` returns, which they do when they live
 * in the awaiting coroutine's frame. Every awaitable here works from a fiber through `fiber::await`.
 *
 * File offsets are explicit: there is no cursor, no lock, no internal buffer, so several coroutines may use one
 * File concurrently as long as they touch different ranges.
 */
namespace usub::uvent::fs
{
    template <class T>
    using Result = std::expected<T, std::error_code>;

    /// `std::error_code` for an errno / GetLastError value.
    std::error_code os_error(int code) noexcept;

    /// Result of a cancelled operation.
    inline std::error_code cancelled() noexcept { return std::make_error_code(std::errc::operation_canceled); }

    enum class OpenMode : uint32_t
    {
        Read = 1u << 0,
        Write = 1u << 1,
        ReadWrite = Read | Write,
        /// Create if missing (needs Write).
        Create = 1u << 2,
        /// Fail if it exists (with Create).
        CreateNew = 1u << 3,
        /// Cut the file to zero length on open (needs Write).
        Truncate = 1u << 4,
        /// O_DIRECT / FILE_FLAG_NO_BUFFERING: aligned buffers, offsets and sizes; never inline.
        Direct = 1u << 5,
        /// O_SYNC / FILE_FLAG_WRITE_THROUGH: every write is durable when it completes.
        Sync = 1u << 6,
    };

    constexpr OpenMode operator|(OpenMode a, OpenMode b) noexcept
    {
        return static_cast<OpenMode>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr bool has(OpenMode m, OpenMode f) noexcept
    {
        return (static_cast<uint32_t>(m) & static_cast<uint32_t>(f)) != 0;
    }

    enum class FileType : uint8_t
    {
        Regular,
        Directory,
        Symlink,
        Other
    };

    struct Metadata
    {
        uint64_t size{0};
        FileType type{FileType::Other};
        /// Modification time, nanoseconds since the Unix epoch.
        int64_t mtime_ns{0};
        /// POSIX permission bits (0644 …); on Windows 0444 or 0666 from the read-only attribute.
        uint32_t permissions{0};

        [[nodiscard]] bool is_file() const noexcept { return this->type == FileType::Regular; }
        [[nodiscard]] bool is_dir() const noexcept { return this->type == FileType::Directory; }
        [[nodiscard]] bool is_symlink() const noexcept { return this->type == FileType::Symlink; }
    };

    struct DirEntry
    {
        std::string name; ///< last path component
        std::string path; ///< directory path joined with name
        FileType type{FileType::Other};
    };

    /**
     * \brief An open file with positional asynchronous I/O.
     *
     * Move-only. The destructor closes the descriptor synchronously (close(2) on a regular file is cheap; call
     * `async_close()` first when the file may live on a network mount).
     */
    class File
    {
    public:
#ifdef _WIN32
        using native_handle_type = void*; // HANDLE
        static inline const native_handle_type kInvalid = reinterpret_cast<void*>(-1);
#else
        using native_handle_type = int;
        static constexpr native_handle_type kInvalid = -1;
#endif

        File() = default;
        File(File&& o) noexcept;
        File& operator=(File&& o) noexcept;
        File(const File&) = delete;
        File& operator=(const File&) = delete;
        ~File();

        /// Adopt an already open descriptor / handle (owned from now on).
        static File from_native(native_handle_type h, OpenMode mode = OpenMode::ReadWrite) noexcept;

        /// Open (and possibly create, `perm` = POSIX mode bits for a new file).
        static task::Awaitable<Result<File>> async_open(std::string path, OpenMode mode, int perm = 0644);

        /// Read up to buf.size() bytes at `offset`; returns the count (0 at end of file). May be short.
        task::Awaitable<Result<std::size_t>> async_read_at(std::span<std::byte> buf, uint64_t offset);

        /// Read exactly buf.size() bytes at `offset` (loops); `std::errc::no_message` (EOF) when the file ends first.
        task::Awaitable<Result<void>> async_read_exact_at(std::span<std::byte> buf, uint64_t offset);

        /// Write up to buf.size() bytes at `offset`; returns the count. May be short.
        task::Awaitable<Result<std::size_t>> async_write_at(std::span<const std::byte> buf, uint64_t offset);

        /// Write all of `buf` at `offset` (loops).
        task::Awaitable<Result<void>> async_write_all_at(std::span<const std::byte> buf, uint64_t offset);

        /// fsync / FlushFileBuffers.
        task::Awaitable<Result<void>> async_sync_all();

        /// fdatasync (fsync where there is no fdatasync).
        task::Awaitable<Result<void>> async_sync_data();

        /// ftruncate / SetEndOfFile (grow or shrink).
        task::Awaitable<Result<void>> async_set_len(uint64_t size);

        task::Awaitable<Result<Metadata>> async_metadata();

        /// Close; the File is empty afterwards.
        task::Awaitable<Result<void>> async_close();

        [[nodiscard]] bool is_open() const noexcept { return this->h_ != kInvalid; }
        [[nodiscard]] native_handle_type native_handle() const noexcept { return this->h_; }
        [[nodiscard]] OpenMode mode() const noexcept { return this->mode_; }

        /// Release ownership of the handle without closing it.
        native_handle_type release() noexcept;

    private:
        native_handle_type h_{kInvalid};
        OpenMode mode_{OpenMode::Read};
        bool direct_{false};
        bool sync_{false};
    };

    // ---- whole-file and path helpers (each is one blocking-pool job) ------------------------------------------

    task::Awaitable<Result<std::vector<std::byte>>> read(std::string path);
    task::Awaitable<Result<std::string>> read_to_string(std::string path);
    /// Create or replace the file and write everything.
    task::Awaitable<Result<void>> write(std::string path, std::span<const std::byte> data);
    task::Awaitable<Result<void>> write(std::string path, std::string_view data);
    /// Create if missing, append everything.
    task::Awaitable<Result<void>> append(std::string path, std::span<const std::byte> data);

    task::Awaitable<Result<Metadata>> metadata(std::string path);
    /// Like metadata() but does not follow a final symlink.
    task::Awaitable<Result<Metadata>> symlink_metadata(std::string path);
    task::Awaitable<Result<bool>> exists(std::string path);

    task::Awaitable<Result<void>> remove_file(std::string path);
    task::Awaitable<Result<void>> rename(std::string from, std::string to);
    /// Copy a regular file (overwrites); returns bytes copied.
    task::Awaitable<Result<uint64_t>> copy(std::string from, std::string to);

    task::Awaitable<Result<void>> create_dir(std::string path);
    task::Awaitable<Result<void>> create_dir_all(std::string path);
    /// Remove an empty directory.
    task::Awaitable<Result<void>> remove_dir(std::string path);
    /// Remove a directory tree; returns the number of entries removed.
    task::Awaitable<Result<uint64_t>> remove_dir_all(std::string path);
    task::Awaitable<Result<std::vector<DirEntry>>> read_dir(std::string path);
    task::Awaitable<Result<std::string>> canonicalize(std::string path);
} // namespace usub::uvent::fs

#endif // UVENT_FS_FS_H
