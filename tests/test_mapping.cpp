// fs::Mapping — a file range as a span. Covers: read mapping of a whole file and of an unaligned sub-range,
// async_map(path), range validation, ReadWrite writes reaching the file after flush, Private (copy-on-write)
// writes staying local, prefetch making every page resident (after page-cache eviction on Linux), advise,
// move semantics / unmap, cancellation before start, and use from a fiber.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#include <process.h>
#endif

#include "test_common.h"
#include "uvent/Uvent.h"
#include "uvent/fs/Fs.h"
#include "uvent/fs/Mapping.h"
#include "uvent/tasks/Task.h"

using namespace usub::uvent;
using namespace std::chrono_literals;

namespace
{
    std::string scratch_dir()
    {
        auto p = std::filesystem::temp_directory_path() / ("uvent_map_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(p);
        return p.string();
    }

    std::vector<std::byte> pattern(std::size_t n, uint64_t seed)
    {
        std::vector<std::byte> v(n);
        std::mt19937_64 rng(seed);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = static_cast<std::byte>(rng());
        return v;
    }

    constexpr std::size_t kSize = 3 * 4096 + 100; // three pages and a tail

    // ------------------------------------------------------------ read mappings

    task::Awaitable<void> read_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string path = dir + "/r.bin";
        const auto data = pattern(kSize, 1);
        CHECK((co_await fs::write(path, data)).has_value());

        auto f = co_await fs::File::async_open(path, fs::OpenMode::Read);
        CHECK(f.has_value());

        auto whole = fs::Mapping::map(*f);
        CHECK(whole.has_value());
        CHECK(whole->is_mapped());
        CHECK_EQ(whole->size(), kSize);
        CHECK_EQ(whole->offset(), 0u);
        CHECK(std::equal(whole->data().begin(), whole->data().end(), data.begin()));
        CHECK_EQ(whole->page_count(), 4u);
        CHECK_EQ(whole->page_count(0, 4096), 1u);
        CHECK_EQ(whole->page_count(4095, 2), 2u);
        CHECK_EQ(whole->page_count(kSize, 10), 0u); // past the end: empty
        CHECK((co_await whole->async_prefetch()).has_value());
        CHECK(whole->is_resident());
        CHECK((co_await whole->async_flush()).has_value()); // no-op on a read mapping
        CHECK(whole->advise(fs::MapAdvice::Sequential).has_value());
        CHECK(whole->advise(fs::MapAdvice::Random, 4096, 4096).has_value());

        // unaligned sub-range: data() starts at the requested byte
        auto part = fs::Mapping::map(*f, fs::MapAccess::Read, 4096 + 10, 500);
        CHECK(part.has_value());
        CHECK_EQ(part->size(), 500u);
        CHECK_EQ(part->offset(), 4096u + 10u);
        CHECK(std::memcmp(part->data().data(), data.data() + 4096 + 10, 500) == 0);
        CHECK_EQ(part->page_count(), 1u);

        // to the end of file from an offset
        auto tail = fs::Mapping::map(*f, fs::MapAccess::Read, 3 * 4096);
        CHECK(tail.has_value());
        CHECK_EQ(tail->size(), 100u);

        // range validation
        auto past = fs::Mapping::map(*f, fs::MapAccess::Read, 0, kSize + 1);
        CHECK(!past.has_value() && past.error() == std::errc::invalid_argument);
        auto at_end = fs::Mapping::map(*f, fs::MapAccess::Read, kSize);
        CHECK(!at_end.has_value() && at_end.error() == std::errc::invalid_argument);
        auto beyond = fs::Mapping::map(*f, fs::MapAccess::Read, kSize + 5, 1);
        CHECK(!beyond.has_value() && beyond.error() == std::errc::invalid_argument);
        fs::File closed;
        auto nofile = fs::Mapping::map(closed);
        CHECK(!nofile.has_value() && nofile.error() == std::errc::bad_file_descriptor);

        // by path (open + map + close on the pool); the mapping outlives the descriptor
        auto by_path = co_await fs::Mapping::async_map(path);
        CHECK(by_path.has_value());
        CHECK(std::equal(by_path->data().begin(), by_path->data().end(), data.begin()));
        auto missing = co_await fs::Mapping::async_map(dir + "/none");
        CHECK(!missing.has_value() && missing.error() == std::errc::no_such_file_or_directory);

        // move / unmap
        fs::Mapping moved = std::move(*whole);
        CHECK(!whole->is_mapped());
        CHECK(moved.is_mapped() && moved.size() == kSize);
        moved.unmap();
        CHECK(!moved.is_mapped());
        CHECK_EQ(moved.size(), 0u);
        CHECK_EQ(moved.page_count(), 0u);
        auto r = moved.resident_pages();
        CHECK(r.has_value() && *r == 0u);
        moved.unmap(); // idempotent

        CHECK((co_await f->async_close()).has_value());
        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void read_mappings()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(read_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ write-back and copy-on-write

    task::Awaitable<void> write_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string path = dir + "/w.bin";
        auto data = pattern(kSize, 2);
        CHECK((co_await fs::write(path, data)).has_value());

        auto rw = co_await fs::File::async_open(path, fs::OpenMode::ReadWrite);
        CHECK(rw.has_value());
        auto shared = fs::Mapping::map(*rw, fs::MapAccess::ReadWrite);
        CHECK(shared.has_value());
        std::memset(shared->data().data() + 4096, 0xAB, 16);
        std::memset(shared->data().data() + kSize - 8, 0xCD, 8);
        CHECK((co_await shared->async_flush(4096, 16)).has_value());
        CHECK((co_await shared->async_flush()).has_value());
        shared->unmap();

        std::byte back[16];
        auto n = co_await rw->async_read_at(back, 4096);
        CHECK(n.has_value() && *n == 16u);
        CHECK(std::all_of(back, back + 16, [](std::byte b) { return b == std::byte{0xAB}; }));
        n = co_await rw->async_read_at(std::span(back, 8), kSize - 8);
        CHECK(n.has_value() && *n == 8u);
        CHECK(std::all_of(back, back + 8, [](std::byte b) { return b == std::byte{0xCD}; }));

        // a read-only file cannot be mapped ReadWrite
        auto ro = co_await fs::File::async_open(path, fs::OpenMode::Read);
        CHECK(ro.has_value());
        auto denied = fs::Mapping::map(*ro, fs::MapAccess::ReadWrite);
        CHECK(!denied.has_value());

        // copy-on-write: the change is visible through the span, never in the file
        auto priv = fs::Mapping::map(*ro, fs::MapAccess::Private);
        CHECK(priv.has_value());
        priv->data()[0] = std::byte{0x77};
        priv->data()[4096 * 2 + 1] = std::byte{0x78};
        CHECK(priv->data()[0] == std::byte{0x77});
        CHECK((co_await priv->async_flush()).has_value()); // no-op
        priv->unmap();
        auto first = co_await ro->async_read_at(std::span(back, 1), 0);
        CHECK(first.has_value() && *first == 1u);
        CHECK(back[0] == data[0]); // pattern(…, 2)[0] – untouched
        auto via_path = co_await fs::Mapping::async_map(path, fs::MapAccess::ReadWrite);
        CHECK(via_path.has_value());
        via_path->data()[1] = std::byte{0x5A};
        CHECK((co_await via_path->async_flush(0, 1)).has_value());
        via_path->unmap();
        auto second = co_await ro->async_read_at(std::span(back, 2), 0);
        CHECK(second.has_value() && *second == 2u);
        CHECK(back[1] == std::byte{0x5A});

        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void write_back_and_cow()
    {
        usub::Uvent rt(2);
        system::co_spawn_static(write_body(&rt), 1);
        rt.run();
    }

    // ------------------------------------------------------------ prefetch / residency

    task::Awaitable<void> prefetch_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string path = dir + "/p.bin";
        constexpr std::size_t kBig = 4u << 20; // 1024 pages
        CHECK((co_await fs::write(path, pattern(kBig, 3))).has_value());

        auto f = co_await fs::File::async_open(path, fs::OpenMode::Read);
        CHECK(f.has_value());
        CHECK((co_await f->async_sync_all()).has_value());
#ifdef __linux__
        ::posix_fadvise(f->native_handle(), 0, 0, POSIX_FADV_DONTNEED); // evict before mapping (no-op on tmpfs)
#endif
        auto m = fs::Mapping::map(*f);
        CHECK(m.has_value());
        const std::size_t total = m->page_count();
        CHECK_EQ(total, kBig / fs::Mapping::page_size());
        auto before = m->resident_pages();
        CHECK(before.has_value());
        CHECK(*before <= total); // 0 on ext4 after DONTNEED, anything on tmpfs / a hot page cache

        CHECK((co_await m->async_prefetch(0, 8 * 4096)).has_value()); // a prefix …
        CHECK(m->is_resident(0, 8 * 4096));
        CHECK((co_await m->async_prefetch()).has_value()); // … then all of it
        auto after = m->resident_pages();
        CHECK(after.has_value());
        CHECK_EQ(*after, total);
        CHECK(m->is_resident());

        // touching every page now cannot fault to disk: cheap checksum over the span
        uint64_t sum = 0;
        for (std::size_t i = 0; i < kBig; i += 4096)
            sum += static_cast<uint8_t>(m->data()[i]);
        CHECK(sum > 0);

        CHECK(m->advise(fs::MapAdvice::DontNeed).has_value());
        auto dropped = m->resident_pages();
        CHECK(dropped.has_value());
        CHECK(*dropped <= total);
        CHECK((co_await m->async_prefetch()).has_value());
        CHECK(m->is_resident());

        CHECK((co_await m->async_prefetch(kBig, 100)).has_value()); // empty range: ok, nothing to do
        CHECK((co_await f->async_close()).has_value());
        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void prefetch_and_residency()
    {
        usub::Uvent rt(1);
        system::co_spawn_static(prefetch_body(&rt), 0);
        rt.run();
    }

    // ------------------------------------------------------------ cancellation before start

    task::Awaitable<void> cancel_child(fs::Mapping* m, const std::string* path, std::atomic<int>* code)
    {
        co_await system::this_coroutine::sleep_for(50ms);
        auto p = co_await m->async_prefetch();
        auto q = co_await fs::Mapping::async_map(*path);
        const bool ok = !p && p.error() == std::errc::operation_canceled && !q && q.error() == std::errc::operation_canceled;
        code->store(ok ? 1 : 2);
    }

    task::Awaitable<void> cancel_body(usub::Uvent* rt)
    {
        const std::string dir = scratch_dir();
        const std::string path = dir + "/c.bin";
        CHECK((co_await fs::write(path, pattern(4096, 4))).has_value());
        auto m = co_await fs::Mapping::async_map(path);
        CHECK(m.has_value());
        std::atomic<int> code{0};
        task::TaskScope scope;
        scope.spawn(cancel_child(&*m, &path, &code));
        co_await system::this_coroutine::sleep_for(5ms);
        scope.cancel();
        co_await scope.join();
        CHECK_EQ(code.load(), 1);
        std::filesystem::remove_all(dir);
        rt->stop();
    }

    void cancelled_task_does_not_start()
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
        const std::string path = dir + "/f.bin";
        CHECK((co_await fs::write(path, "mapped from a fiber")).has_value());
        const std::string got = co_await fiber::run(
            [path]
            {
                auto m = fiber::await(fs::Mapping::async_map(path));
                if (!m)
                    return std::string("ERR");
                fiber::await(m->async_prefetch());
                return std::string(reinterpret_cast<const char*>(m->data().data()), m->size());
            });
        CHECK(got == "mapped from a fiber");
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
        {"read_mappings", read_mappings},
        {"write_back_and_cow", write_back_and_cow},
        {"prefetch_and_residency", prefetch_and_residency},
        {"cancelled_task_does_not_start", cancelled_task_does_not_start},
#ifdef UVENT_ENABLE_FIBERS
        {"works_from_fiber", works_from_fiber},
#endif
    });
}
