# File system (`uvent/fs/Fs.h`)

Asynchronous file access with one API on every platform and the fastest engine each platform has underneath.

```cpp
#include "uvent/fs/Fs.h"

task::Awaitable<void> demo()
{
    auto f = co_await fs::File::async_open("data.bin", fs::OpenMode::ReadWrite | fs::OpenMode::Create);
    if (!f) { spdlog::error("open: {}", f.error().message()); co_return; }

    std::byte page[4096];
    auto n = co_await f->async_read_at(page, 0);            // Result<size_t>; 0 = end of file
    co_await f->async_write_all_at(std::as_bytes(std::span(page)), 8192);
    co_await f->async_sync_data();                           // fdatasync
    co_await f->async_close();

    auto text = co_await fs::read_to_string("config.toml");   // whole file
    co_await fs::write("out.txt", "done");
    for (auto& e : *co_await fs::read_dir("."))
        spdlog::info("{} {}", e.name, e.type == fs::FileType::Directory ? "/" : "");
}
```

Every function returns `fs::Result<T>` = `std::expected<T, std::error_code>`. A task that is already cancelled when
it calls in gets `std::errc::operation_canceled` without starting the operation; an operation already handed to
io_uring or the pool always runs to completion (the buffer is in use until then). All awaitables work from a fiber
through `fiber::await`.

## `fs::File`

Positional, on `std::span`, no cursor: there is no lock on file state, no internal buffer and no second copy
(`tokio::fs::File` has both). Several coroutines may use one `File` at once as long as they touch different ranges.

|                                                              |                                                                                                                                                                |
|--------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `File::async_open(path, OpenMode, perm = 0644)`              | `Read`, `Write`, `ReadWrite`, `Create`, `CreateNew`, `Truncate`, `Direct` (O_DIRECT / `F_NOCACHE` / `FILE_FLAG_NO_BUFFERING`), `Sync` (O_SYNC / write-through) |
| `async_read_at(span, off)` → `size_t`                        | may be short; 0 at end of file                                                                                                                                 |
| `async_read_exact_at(span, off)`                             | loops; `errc::no_message` if the file ends first                                                                                                               |
| `async_write_at(span, off)` → `size_t`, `async_write_all_at` |                                                                                                                                                                |
| `async_sync_all()` / `async_sync_data()`                     | fsync / fdatasync (fsync on macOS, `FlushFileBuffers` on Windows)                                                                                              |
| `async_set_len(n)`, `async_metadata()`, `async_close()`      |                                                                                                                                                                |
| `from_native(h)`, `native_handle()`, `release()`             | interop; the destructor closes synchronously                                                                                                                   |

Path helpers, one pool job each, as in `tokio::fs`: `read`, `read_to_string`, `write`, `append`, `metadata`,
`symlink_metadata`, `exists`, `remove_file`, `rename`, `copy`, `create_dir`, `create_dir_all`, `remove_dir`,
`remove_dir_all`, `read_dir`, `canonicalize`.

## Engines

The choice is per operation and per platform; the caller never sees it.

| operation                                    | Linux                                                                                                                                                                                                                                                          | macOS / BSD     | Windows |
|----------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------|---------|
| `read_at`, buffered file                     | **1.** `preadv2(RWF_NOWAIT)` inline on the worker – a page-cache hit is one syscall, no hop (`settings::fs_inline_nowait_read`); **2.** on `EAGAIN` (not cached): `IORING_OP_READ` on the worker's ring when built with `UVENT_ENABLE_IO_URING`, else the pool | pool            | pool    |
| `read_at`, `Direct`                          | io_uring / pool                                                                                                                                                                                                                                                | pool            | pool    |
| `write_at`, buffered, not `Sync`             | `pwrite` inline (`settings::fs_inline_buffered_write`)                                                                                                                                                                                                         | `pwrite` inline | pool    |
| `write_at`, `Direct` or `Sync`               | io_uring / pool                                                                                                                                                                                                                                                | pool            | pool    |
| `sync_all`, `sync_data`                      | `IORING_OP_FSYNC` / pool                                                                                                                                                                                                                                       | pool            | pool    |
| open, metadata, set_len, close, path helpers | pool                                                                                                                                                                                                                                                           | pool            | pool    |

Why this shape (numbers from `io_perfomance/fs_bench` on a Xeon E5-2640 v4 with a Kingston NVMe, kernel 6.8, ext4,
1 thread, queue depth 1 unless stated):

- **Cached reads must not leave the worker.** `pread` of a cached 4 KiB block is 1.2 µs; the same through a thread
  pool is 11 µs and 9× the CPU. `RWF_NOWAIT` costs nothing on a hit (1.2 µs, 100 % hits on warm data) and returns
  `EAGAIN` in ~2 µs on a miss, so probing first is free.
- **Misses go to io_uring, not a pool.** Cold 4 KiB reads at queue depth 32: io_uring 50.6k ops/s at 10.8 CPU-µs
  each, pool 28.6k at 20.7. O_DIRECT: 50.8k at 5.6–6.0 vs 29.4k at 15.1. The CQE arrives in the same batch as the
  socket completions; a pool needs a futex and an `eventfd` per operation.
- **Buffered writes stay inline.** A buffered `pwrite` lands in the page cache in 1.2 µs. io_uring cannot do a
  buffered write on ext4 without blocking and punts every one to its io-wq kernel threads (6.9 µs measured);
  `pwritev2(RWF_NOWAIT)` returns `EAGAIN` 100 % of the time. The cost is a stall on the worker when the kernel
  throttles dirty pages; set `settings::fs_inline_buffered_write = false` to route such writes through io_uring /
  the pool instead.
- **`fsync` is the device's time** (~320 µs per commit on this NVMe for every mechanism); io_uring saves ~15 % CPU,
  the pool adds ~18 % latency.
- **macOS / Windows are development platforms here**; kqueue has no file readiness (libuv uses a pool there too) and
  the Windows path is synchronous handles on the pool for now – overlapped `ReadFile` / `WriteFile` through IOCP is
  a later step.

io_uring in containers: Docker's default seccomp profile blocks `io_uring_*`; on a cluster it depends on the
`RuntimeDefault` profile. The library is built without io_uring by default (`UVENT_ENABLE_IO_URING=OFF`) and the
pool path is always there, so nothing changes functionally – only the miss path costs more.

## `fs::Mapping` – a file range as a span (`uvent/fs/Mapping.h`)

For hot, read-mostly data (an index, a dictionary, an LMDB-style store) a memory mapping beats every read call:
a resident 4 KiB page costs ~0.5 µs to access with no syscall and no copy, a sequential scan runs at 10 GB/s
against 7 GB/s for `pread` (fs_bench, same host). It is a separate type rather than the engine behind
`async_read_at` on purpose – see the header comment for the three reasons (a non-resident page blocks the worker
inside a load instruction; shared writes can stall in dirty-page throttling; I/O errors and a shortened file arrive
as SIGBUS, not as `error_code`).

```cpp
#include "uvent/fs/Mapping.h"

auto m = co_await fs::Mapping::async_map("index.bin");         // Read, whole file; or Mapping::map(file, ...)
co_await m->async_prefetch();                                   // bring it in on the pool: no faults on the worker later
std::span<const std::byte> bytes = m->data();                   // use it
if (!m->is_resident(off, len)) co_await m->async_prefetch(off, len);   // before touching a possibly cold range

auto w = fs::Mapping::map(*file, fs::MapAccess::ReadWrite, 0, 1 << 20);
std::memcpy(w->data().data() + 4096, rec, sizeof rec);
co_await w->async_flush(4096, sizeof rec);                      // msync(MS_SYNC): durable
```

|                                                                                                             |                                                                                                                     |
|-------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|
| `Mapping::map(const File&, MapAccess, offset = 0, len = 0)`                                                 | synchronous (mmap does no I/O); `len = 0` → to EOF; unaligned `offset` is fine; range past EOF → `invalid_argument` |
| `Mapping::async_map(path, MapAccess, offset, len)`                                                          | open + map + close on the pool                                                                                      |
| `MapAccess::Read` / `ReadWrite` (shared, writes reach the file) / `Private` (copy-on-write, file untouched) |                                                                                                                     |
| `data()`, `size()`, `offset()`, `is_mapped()`, `unmap()`                                                    |                                                                                                                     |
| `async_prefetch(off, len)`                                                                                  | pool: `MADV_POPULATE_READ` (Linux ≥ 5.14), else `MADV_WILLNEED` + touch; `PrefetchVirtualMemory` + touch on Windows |
| `async_flush(off, len)`                                                                                     | pool: `msync(MS_SYNC)`; `FlushViewOfFile` + `FlushFileBuffers` on Windows; no-op for `Read` / `Private`             |
| `resident_pages(off, len)`, `is_resident`, `page_count`                                                     | `mincore` / `QueryWorkingSetEx`: one syscall, a hint (pages can be evicted right after)                             |
| `advise(MapAdvice::Sequential / Random / Normal / DontNeed, off, len)`                                      | `madvise`; on Windows only `DontNeed` does anything                                                                 |

Rules: the file must not be shortened while mapped (SIGBUS); growing needs `File::async_set_len` and a new
mapping; the destructor unmaps without flushing. A page that was prefetched can still be evicted under memory
pressure, so a mapping is the right tool when the data set fits comfortably in RAM, and `File::async_read_at` is
the right tool otherwise. `IORING_OP_MADVISE` was measured at 6.7 µs per call (forced to io-wq) and is not used –
the pool hop costs the same and works everywhere.

Tests: `tests/test_mapping.cpp` (whole file / unaligned sub-range / to-EOF mappings, range validation, `async_map`,
`ReadWrite` writes visible through `File` after `async_flush`, `Private` writes invisible in the file, prefetch after
`POSIX_FADV_DONTNEED` making every page resident, `DontNeed` + re-prefetch, move / unmap, cancellation before start,
fiber). ASan / TSan clean; MinGW and osxcross builds.

## Cancellation

- Before start: `operation_canceled`, no syscall.
- Inline (`RWF_NOWAIT`, `pwrite`): nothing to cancel, it has already completed.
- io_uring: cancellation submits `ASYNC_CANCEL` for the SQE, then still waits for the CQE (an op that io-wq is
  already executing completes with its real result, one that had not started with `-ECANCELED`). The span stays
  in use until `co_await` returns.
- Pool: not interruptible; `co_await` returns after the job.

So a buffer that lives in the awaiting coroutine's frame is always safe – the frame cannot be destroyed before the
`co_await` returns, even under drain.

## Tests

`tests/test_fs.cpp`: round trip with short reads at EOF and past EOF, `read_exact` / `write_all` loops, metadata,
`set_len` grow / shrink, sync, every path helper and its error codes (`ENOENT`, `EISDIR`, `ENOTDIR`, `EEXIST`,
`ENOTEMPTY`), `CreateNew`, `release` / `from_native`, an 8 MiB file evicted with `POSIX_FADV_DONTNEED` so the slow
engine is exercised (64 concurrent scattered 4 KiB reads), O_DIRECT with an aligned buffer, cancellation before
start, and use from a fiber. Runs under ASan / UBSan and TSan; builds with MinGW (Windows) and osxcross (macOS).
