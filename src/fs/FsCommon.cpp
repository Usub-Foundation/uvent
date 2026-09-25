// Platform-independent part of uvent/fs: whole-file and directory helpers (one blocking-pool job each, built on
// C stdio and std::filesystem), read_exact / write_all loops, error-code plumbing.
#include "uvent/fs/Fs.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <utility>

#include "uvent/blocking/Blocking.h"
#include "uvent/system/SystemContext.h"

namespace usub::uvent::fs
{
    namespace detail
    {
        Result<Metadata> stat_path(const std::string& path, bool follow) noexcept; // FsPosix.cpp / FsWin.cpp
    }

    namespace
    {
        namespace stdfs = std::filesystem;

        bool cancel_now() noexcept { return system::this_coroutine::cancel_requested(); }

        std::error_code errc_code(std::errc e) noexcept { return std::make_error_code(e); }

        struct FileCloser
        {
            void operator()(std::FILE* f) const noexcept
            {
                if (f)
                    std::fclose(f);
            }
        };
        using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

        template <class Container>
        Result<Container> read_all(const std::string& path)
        {
            FilePtr f(std::fopen(path.c_str(), "rb"));
            if (!f)
                return std::unexpected(os_error(errno));
            Container out;
            std::error_code ec;
            const auto sz = stdfs::file_size(path, ec);
            if (!ec && sz > 0 && sz < (uint64_t{1} << 40))
                out.reserve(static_cast<std::size_t>(sz) + 1);
            typename Container::value_type chunk[64 * 1024];
            for (;;)
            {
                const std::size_t n = std::fread(chunk, 1, sizeof chunk, f.get());
                if (n > 0)
                    out.insert(out.end(), chunk, chunk + n);
                if (n < sizeof chunk)
                {
                    if (std::ferror(f.get()))
                        return std::unexpected(os_error(errno ? errno : EIO));
                    break;
                }
            }
            return out;
        }

        Result<void> write_all(const std::string& path, std::span<const std::byte> data, const char* mode)
        {
            FilePtr f(std::fopen(path.c_str(), mode));
            if (!f)
                return std::unexpected(os_error(errno));
            std::size_t done = 0;
            while (done < data.size())
            {
                const std::size_t n = std::fwrite(data.data() + done, 1, data.size() - done, f.get());
                if (n == 0)
                    return std::unexpected(os_error(errno ? errno : EIO));
                done += n;
            }
            std::FILE* raw = f.release();
            if (std::fclose(raw) != 0)
                return std::unexpected(os_error(errno));
            return Result<void>{};
        }

        FileType type_of(stdfs::file_type t) noexcept
        {
            switch (t)
            {
            case stdfs::file_type::regular: return FileType::Regular;
            case stdfs::file_type::directory: return FileType::Directory;
            case stdfs::file_type::symlink: return FileType::Symlink;
            default: return FileType::Other;
            }
        }
    } // namespace

    std::error_code os_error(int code) noexcept { return std::error_code(code, std::system_category()); }

    task::Awaitable<Result<void>> File::async_read_exact_at(std::span<std::byte> buf, uint64_t offset)
    {
        std::size_t done = 0;
        while (done < buf.size())
        {
            auto r = co_await this->async_read_at(buf.subspan(done), offset + done);
            if (!r)
                co_return std::unexpected(r.error());
            if (*r == 0)
                co_return std::unexpected(errc_code(std::errc::no_message)); // EOF before buf was full
            done += *r;
        }
        co_return Result<void>{};
    }

    task::Awaitable<Result<void>> File::async_write_all_at(std::span<const std::byte> buf, uint64_t offset)
    {
        std::size_t done = 0;
        while (done < buf.size())
        {
            auto r = co_await this->async_write_at(buf.subspan(done), offset + done);
            if (!r)
                co_return std::unexpected(r.error());
            if (*r == 0)
                co_return std::unexpected(errc_code(std::errc::io_error));
            done += *r;
        }
        co_return Result<void>{};
    }

    task::Awaitable<Result<std::vector<std::byte>>> read(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run([p = std::move(path)] { return read_all<std::vector<std::byte>>(p); });
    }

    task::Awaitable<Result<std::string>> read_to_string(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run([p = std::move(path)] { return read_all<std::string>(p); });
    }

    task::Awaitable<Result<void>> write(std::string path, std::span<const std::byte> data)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run([p = std::move(path), data] { return write_all(p, data, "wb"); });
    }

    task::Awaitable<Result<void>> write(std::string path, std::string_view data)
    {
        co_return co_await write(std::move(path), std::as_bytes(std::span(data.data(), data.size())));
    }

    task::Awaitable<Result<void>> append(std::string path, std::span<const std::byte> data)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run([p = std::move(path), data] { return write_all(p, data, "ab"); });
    }

    task::Awaitable<Result<Metadata>> metadata(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run([p = std::move(path)] { return detail::stat_path(p, true); });
    }

    task::Awaitable<Result<Metadata>> symlink_metadata(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run([p = std::move(path)] { return detail::stat_path(p, false); });
    }

    task::Awaitable<Result<bool>> exists(std::string path)
    {
        auto m = co_await metadata(std::move(path));
        if (m)
            co_return true;
        // error_code == errc compares through the category's equivalence: ENOENT from system_category matches
        if (m.error() == std::errc::no_such_file_or_directory || m.error() == std::errc::not_a_directory)
            co_return false;
        co_return std::unexpected(m.error());
    }

    task::Awaitable<Result<void>> remove_file(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<void>
            {
                std::error_code ec;
                const auto st = stdfs::symlink_status(p, ec);
                if (ec)
                    return std::unexpected(ec);
                if (st.type() == stdfs::file_type::directory)
                    return std::unexpected(errc_code(std::errc::is_a_directory));
                if (!stdfs::remove(p, ec) && !ec)
                    return std::unexpected(errc_code(std::errc::no_such_file_or_directory));
                if (ec)
                    return std::unexpected(ec);
                return Result<void>{};
            });
    }

    task::Awaitable<Result<void>> rename(std::string from, std::string to)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [a = std::move(from), b = std::move(to)]() -> Result<void>
            {
                std::error_code ec;
                stdfs::rename(a, b, ec);
                if (ec)
                    return std::unexpected(ec);
                return Result<void>{};
            });
    }

    task::Awaitable<Result<uint64_t>> copy(std::string from, std::string to)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [a = std::move(from), b = std::move(to)]() -> Result<uint64_t>
            {
                std::error_code ec;
                stdfs::copy_file(a, b, stdfs::copy_options::overwrite_existing, ec);
                if (ec)
                    return std::unexpected(ec);
                const auto sz = stdfs::file_size(b, ec);
                if (ec)
                    return std::unexpected(ec);
                return static_cast<uint64_t>(sz);
            });
    }

    task::Awaitable<Result<void>> create_dir(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<void>
            {
                std::error_code ec;
                const bool created = stdfs::create_directory(p, ec);
                if (ec)
                    return std::unexpected(ec);
                if (!created)
                    return std::unexpected(errc_code(std::errc::file_exists));
                return Result<void>{};
            });
    }

    task::Awaitable<Result<void>> create_dir_all(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<void>
            {
                std::error_code ec;
                stdfs::create_directories(p, ec);
                if (ec)
                    return std::unexpected(ec);
                return Result<void>{};
            });
    }

    task::Awaitable<Result<void>> remove_dir(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<void>
            {
                std::error_code ec;
                const auto st = stdfs::symlink_status(p, ec);
                if (ec)
                    return std::unexpected(ec);
                if (st.type() != stdfs::file_type::directory)
                    return std::unexpected(errc_code(std::errc::not_a_directory));
                stdfs::remove(p, ec); // fails with ENOTEMPTY / directory_not_empty on a non-empty directory
                if (ec)
                    return std::unexpected(ec);
                return Result<void>{};
            });
    }

    task::Awaitable<Result<uint64_t>> remove_dir_all(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<uint64_t>
            {
                std::error_code ec;
                const auto n = stdfs::remove_all(p, ec);
                if (ec)
                    return std::unexpected(ec);
                return static_cast<uint64_t>(n);
            });
    }

    task::Awaitable<Result<std::vector<DirEntry>>> read_dir(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<std::vector<DirEntry>>
            {
                std::error_code ec;
                stdfs::directory_iterator it(p, ec);
                if (ec)
                    return std::unexpected(ec);
                std::vector<DirEntry> out;
                for (const auto& e : it)
                {
                    DirEntry d;
                    d.name = e.path().filename().string();
                    d.path = e.path().string();
                    d.type = type_of(e.symlink_status(ec).type());
                    out.push_back(std::move(d));
                }
                return out;
            });
    }

    task::Awaitable<Result<std::string>> canonicalize(std::string path)
    {
        if (cancel_now())
            co_return std::unexpected(cancelled());
        co_return co_await blocking::run(
            [p = std::move(path)]() -> Result<std::string>
            {
                std::error_code ec;
                auto c = stdfs::canonical(p, ec);
                if (ec)
                    return std::unexpected(ec);
                return c.string();
            });
    }
} // namespace usub::uvent::fs
