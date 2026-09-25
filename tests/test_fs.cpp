// uvent/fs — positional File I/O and the path helpers, on a scratch directory under the system temp dir.
// Covers: create/write/read round trip incl. short reads at EOF and reads past EOF, read_exact/write_all loops,
// metadata / set_len / sync, whole-file helpers, directory helpers and read_dir, error codes (ENOENT, EISDIR,
// EEXIST), CreateNew, a large file that exercises the slow path (cold read after DONTNEED on Linux), O_DIRECT
// with an aligned buffer, cancellation before start, and use from a fiber.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/fs/Fs.h"
#include "uvent/tasks/Task.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    std::string scratch_dir()
    {
        auto p = std::filesystem::temp_directory_path() / ("uvent_fs_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(p);
        return p.string();
    }

    std::span<const std::byte> bytes(std::string_view s) { return std::as_bytes(std::span(s.data(), s.size())); }

    std::string to_string(std::span<const std::byte> b)
    {
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }

    // ------------------------------------------------------------ round trip

    task::Awaitable<void> roundtrip_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string path = dir + "/a.txt";

        auto f = co_await fs::File::async_open(path, fs::OpenMode::ReadWrite | fs::OpenMode::Create | fs::OpenMode::Truncate);
        CHECK(f.has_value());
        CHECK(f->is_open());

        auto w = co_await f->async_write_all_at(bytes("hello, "), 0);
        CHECK(w.has_value());
        w = co_await f->async_write_all_at(bytes("world"), 7);
        CHECK(w.has_value());

        std::byte buf[64];
        auto n = co_await f->async_read_at(buf, 0);
        CHECK(n.has_value());
        CHECK_EQ(*n, 12u); // short read at EOF
        CHECK(to_string({buf, *n}) == "hello, world");

        n = co_await f->async_read_at(buf, 7);
        CHECK(n.has_value() && *n == 5u);
        CHECK(to_string({buf, *n}) == "world");

        n = co_await f->async_read_at(buf, 100); // past EOF
        CHECK(n.has_value() && *n == 0u);

        auto ex = co_await f->async_read_exact_at(std::span(buf, 12), 0);
        CHECK(ex.has_value());
        ex = co_await f->async_read_exact_at(std::span(buf, 13), 0);
        CHECK(!ex.has_value() && ex.error() == std::errc::no_message); // EOF before 13 bytes

        auto md = co_await f->async_metadata();
        CHECK(md.has_value());
        CHECK_EQ(md->size, 12u);
        CHECK(md->is_file());
        CHECK(md->mtime_ns > 0);

        CHECK((co_await f->async_set_len(5)).has_value());
        md = co_await f->async_metadata();
        CHECK(md.has_value() && md->size == 5u);
        CHECK((co_await f->async_set_len(4096)).has_value()); // grow: zero-filled
        n = co_await f->async_read_at(std::span(buf, 8), 5);
        CHECK(n.has_value() && *n == 8u);
        CHECK(std::all_of(buf, buf + 8, [](std::byte b) { return b == std::byte{0}; }));

        CHECK((co_await f->async_sync_data()).has_value());
        CHECK((co_await f->async_sync_all()).has_value());
        CHECK((co_await f->async_close()).has_value());
        CHECK(!f->is_open());
        CHECK((co_await f->async_close()).has_value()); // idempotent

        // reopen read-only, empty write must fail
        auto ro = co_await fs::File::async_open(path, fs::OpenMode::Read);
        CHECK(ro.has_value());
        auto bad = co_await ro->async_write_at(bytes("x"), 0);
        CHECK(!bad.has_value());
        CHECK(bad.error() == std::errc::bad_file_descriptor);

        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void file_roundtrip()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(roundtrip_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ open modes & errors

    task::Awaitable<void> modes_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        auto missing = co_await fs::File::async_open(dir + "/nope", fs::OpenMode::Read);
        CHECK(!missing.has_value());
        CHECK(missing.error() == std::errc::no_such_file_or_directory);

        auto a = co_await fs::File::async_open(dir + "/n.bin", fs::OpenMode::Write | fs::OpenMode::CreateNew);
        CHECK(a.has_value());
        auto again = co_await fs::File::async_open(dir + "/n.bin", fs::OpenMode::Write | fs::OpenMode::CreateNew);
        CHECK(!again.has_value());
        CHECK(again.error() == std::errc::file_exists);

        CHECK((co_await a->async_write_all_at(bytes("0123456789"), 0)).has_value());
        a = fs::File{}; // destructor closes
        auto tr = co_await fs::File::async_open(dir + "/n.bin", fs::OpenMode::Write | fs::OpenMode::Truncate);
        CHECK(tr.has_value());
        auto md = co_await tr->async_metadata();
        CHECK(md.has_value() && md->size == 0u);

        auto rel = tr->release();
        CHECK(!tr->is_open());
        auto adopted = fs::File::from_native(rel, fs::OpenMode::Write);
        CHECK(adopted.is_open());
        CHECK((co_await adopted.async_close()).has_value());

        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void open_modes_and_errors()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(modes_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ whole-file + directory helpers

    task::Awaitable<void> helpers_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        CHECK((co_await fs::create_dir(dir + "/sub")).has_value());
        auto dup = co_await fs::create_dir(dir + "/sub");
        CHECK(!dup.has_value() && dup.error() == std::errc::file_exists);
        CHECK((co_await fs::create_dir_all(dir + "/sub/deep/er")).has_value());
        CHECK((co_await fs::create_dir_all(dir + "/sub/deep/er")).has_value()); // idempotent

        CHECK((co_await fs::write(dir + "/sub/one.txt", "one")).has_value());
        CHECK((co_await fs::append(dir + "/sub/one.txt", bytes(" two"))).has_value());
        auto s = co_await fs::read_to_string(dir + "/sub/one.txt");
        CHECK(s.has_value() && *s == "one two");
        auto v = co_await fs::read(dir + "/sub/one.txt");
        CHECK(v.has_value() && v->size() == 7u);

        auto miss = co_await fs::read(dir + "/sub/none");
        CHECK(!miss.has_value() && miss.error() == std::errc::no_such_file_or_directory);

        auto ex = co_await fs::exists(dir + "/sub/one.txt");
        CHECK(ex.has_value() && *ex);
        ex = co_await fs::exists(dir + "/sub/none");
        CHECK(ex.has_value() && !*ex);

        auto md = co_await fs::metadata(dir + "/sub");
        CHECK(md.has_value() && md->is_dir());
        md = co_await fs::metadata(dir + "/sub/one.txt");
        CHECK(md.has_value() && md->is_file() && md->size == 7u);

        auto copied = co_await fs::copy(dir + "/sub/one.txt", dir + "/sub/two.txt");
        CHECK(copied.has_value() && *copied == 7u);
        CHECK((co_await fs::rename(dir + "/sub/two.txt", dir + "/sub/three.txt")).has_value());

        auto entries = co_await fs::read_dir(dir + "/sub");
        CHECK(entries.has_value());
        std::vector<std::string> names;
        for (auto& e : *entries)
            names.push_back(e.name);
        std::sort(names.begin(), names.end());
        CHECK(names == std::vector<std::string>({"deep", "one.txt", "three.txt"}));
        for (auto& e : *entries)
            CHECK(e.type == (e.name == "deep" ? fs::FileType::Directory : fs::FileType::Regular));

        auto canon = co_await fs::canonicalize(dir + "/sub/./deep/../one.txt");
        CHECK(canon.has_value());
        CHECK(canon->ends_with("one.txt") && canon->find("..") == std::string::npos);

        auto isdir = co_await fs::remove_file(dir + "/sub/deep");
        CHECK(!isdir.has_value() && isdir.error() == std::errc::is_a_directory);
        auto notdir = co_await fs::remove_dir(dir + "/sub/one.txt");
        CHECK(!notdir.has_value() && notdir.error() == std::errc::not_a_directory);
        auto notempty = co_await fs::remove_dir(dir + "/sub");
        CHECK(!notempty.has_value());
        CHECK((co_await fs::remove_dir(dir + "/sub/deep/er")).has_value());
        CHECK((co_await fs::remove_file(dir + "/sub/one.txt")).has_value());
        auto gone = co_await fs::remove_file(dir + "/sub/one.txt");
        CHECK(!gone.has_value() && gone.error() == std::errc::no_such_file_or_directory);
        auto removed = co_await fs::remove_dir_all(dir);
        CHECK(removed.has_value() && *removed >= 3u);
        ex = co_await fs::exists(dir);
        CHECK(ex.has_value() && !*ex);
        rt->stop();
    }

    void whole_file_and_directory_helpers()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(helpers_body(&rt), 1);
        rt.run();
    }

    // ------------------------------------------------------------ larger file, slow path, O_DIRECT

    task::Awaitable<void> large_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string path = dir + "/big.bin";
        constexpr std::size_t kSize = 8u << 20; // 8 MiB
        std::vector<std::byte> data(kSize);
        std::mt19937_64 rng(7);
        for (std::size_t i = 0; i < kSize; i += 8)
        {
            const uint64_t v = rng();
            std::memcpy(&data[i], &v, 8);
        }
        auto f = co_await fs::File::async_open(path, fs::OpenMode::ReadWrite | fs::OpenMode::Create | fs::OpenMode::Truncate);
        CHECK(f.has_value());
        CHECK((co_await f->async_write_all_at(data, 0)).has_value());
        CHECK((co_await f->async_sync_all()).has_value());

#ifdef __linux__
        // evict from the page cache so the NOWAIT probe misses and the slow engine (io_uring or pool) is used
        ::posix_fadvise(f->native_handle(), 0, 0, POSIX_FADV_DONTNEED);
#endif
        std::vector<std::byte> back(kSize);
        CHECK((co_await f->async_read_exact_at(back, 0)).has_value());
        CHECK(back == data);

        // scattered 4 KiB reads at random offsets, some concurrent
        task::TaskScope scope;
        std::atomic<int> bad{0};
        for (int i = 0; i < 64; ++i)
        {
            const uint64_t off = (rng() % (kSize / 4096)) * 4096;
            scope.spawn(
                [](fs::File* file, const std::vector<std::byte>* ref, uint64_t o, std::atomic<int>* b) -> task::Awaitable<void>
                {
                    std::byte chunk[4096];
                    auto r = co_await file->async_read_exact_at(chunk, o);
                    if (!r || std::memcmp(chunk, ref->data() + o, 4096) != 0)
                        b->fetch_add(1);
                }(&*f, &data, off, &bad));
        }
        co_await scope.join();
        CHECK_EQ(bad.load(), 0);
        CHECK((co_await f->async_close()).has_value());

#ifdef __linux__
        auto d = co_await fs::File::async_open(path, fs::OpenMode::Read | fs::OpenMode::Direct);
        if (d) // tmpfs refuses O_DIRECT (EINVAL): skip silently there
        {
            void* raw = nullptr;
            CHECK(::posix_memalign(&raw, 4096, 8192) == 0);
            std::span<std::byte> aligned(static_cast<std::byte*>(raw), 8192);
            auto n = co_await d->async_read_at(aligned, 4096 * 3);
            CHECK(n.has_value());
            CHECK_EQ(*n, 8192u);
            CHECK(std::memcmp(raw, data.data() + 4096 * 3, 8192) == 0);
            std::free(raw);
        }
#endif
        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void large_file_slow_path_and_direct()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(large_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ cancellation before start

    task::Awaitable<void> cancel_child(fs::File* f, std::atomic<int>* code)
    {
        // parent cancels us while we sleep; the next fs call must not start and must report cancellation
        co_await system::this_coroutine::sleep_for(50ms);
        std::byte b[16];
        auto r = co_await f->async_read_at(b, 0);
        code->store(!r && r.error() == std::errc::operation_canceled ? 1 : 2);
    }

    task::Awaitable<void> cancel_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        auto f = co_await fs::File::async_open(dir + "/c.txt", fs::OpenMode::ReadWrite | fs::OpenMode::Create);
        CHECK(f.has_value());
        std::atomic<int> code{0};
        task::TaskScope scope;
        scope.spawn(cancel_child(&*f, &code));
        co_await system::this_coroutine::sleep_for(5ms);
        scope.cancel();
        co_await scope.join();
        CHECK_EQ(code.load(), 1);
        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void cancelled_task_does_not_start_io()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(cancel_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ from a fiber

#ifdef UVENT_ENABLE_FIBERS
    task::Awaitable<void> fiber_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string ok = co_await fiber::run(
            [dir]
            {
                fiber::await(fs::write(dir + "/f.txt", "from a fiber"));
                auto s = fiber::await(fs::read_to_string(dir + "/f.txt"));
                return s ? *s : std::string("ERR");
            });
        CHECK(ok == "from a fiber");
        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void works_from_fiber()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(fiber_body(&rt), 0);
        rt.run();
    }
#endif
} // namespace

int main()
{
    return run_tests({
        {"file_roundtrip", file_roundtrip},
        {"open_modes_and_errors", open_modes_and_errors},
        {"whole_file_and_directory_helpers", whole_file_and_directory_helpers},
        {"large_file_slow_path_and_direct", large_file_slow_path_and_direct},
        {"cancelled_task_does_not_start_io", cancelled_task_does_not_start_io},
#ifdef UVENT_ENABLE_FIBERS
        {"works_from_fiber", works_from_fiber},
#endif
    });
}
