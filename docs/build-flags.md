# Build flags

Everything that changes what gets compiled: CMake options, the preprocessor
macros they emit, the macros the code derives on its own, and the
configurations CI runs. Runtime knobs (`usub::uvent::settings`, changeable at
startup) are on the [Settings](settings.md) page instead.

Every option is passed as `-DNAME=ON|OFF` to `cmake`. The library exports its
macros as **PUBLIC** compile definitions, so a consumer linking `uvent` sees the
same configuration without repeating the flags.

---

## Runtime shape

| CMake option               | Default | Macro                      | What it does                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
|----------------------------|---------|----------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `UVENT_ENABLE_REUSEADDR`   | **ON**  | `UVENT_ENABLE_REUSEADDR`   | The production mode. Each worker owns a poller and a timer wheel; sockets carry an owner and cross-worker operations (timeout, shutdown, destroy) are forwarded to that owner; cancellation reaches parked tasks through the *cancel kick*. Listeners use `SO_REUSEADDR`/`SO_REUSEPORT`. **OFF** selects the legacy layout: one poller shared under a lock, QSBR-based socket reclamation, no kick (cancellation is cooperative only: a parked task is not woken, see below). |
| `UVENT_ENABLE_FIBERS`      | **ON**  | `UVENT_ENABLE_FIBERS`      | Builds the stackful fibers (`uvent/fiber`, `fiber::run` / `fiber::await` / `fiber::yield`) on top of the coroutine runtime, plus `test_fiber` and the fiber examples. See [Fibers](fibers.md).                                                                                                                                                                                                                                                                                |
| `UVENT_RUNTIME_DRAIN`      | OFF     | `UVENT_RUNTIME_DRAIN`      | Registers every spawned task per worker so `Uvent::stop()` can cancel and wait for parked tasks (`settings::stop_drain_timeout_ms`). Needs the cancel kick, i.e. `UVENT_ENABLE_REUSEADDR=ON`: without it parked tasks are never woken and the drain only ends at the deadline. See [Runtime drain](drain.md).                                                                                                                                                                 |
| `UVENT_TASK_INTROSPECTION` | OFF     | `UVENT_TASK_INTROSPECTION` | Keeps a sharded registry of live coroutine frames for `introspection::snapshot()` / `dump()` (names, trace ids, wait reasons, wait times). Costs about 2–3 % of throughput on a keep-alive HTTP benchmark; leave it off in production unless you need the dumps. See [Introspection](introspection.md).                                                                                                                                                                       |
| `UVENT_ENABLE_IO_URING`    | OFF     | `UVENT_ENABLE_IO_URING`    | Linux only: the io_uring poller and socket implementation (`SocketLinuxIOUring.h`) instead of epoll. Experimental.                                                                                                                                                                                                                                                                                                                                                            |
| `UVENT_PIN_THREADS`        | **ON**  | `UVENT_PIN_THREADS`        | Linux only: pins worker `i` to CPU core `i` at start (`pin_thread_to_core`). Turn it off when the process shares a machine with other services or runs in a CPU-limited container.                                                                                                                                                                                                                                                                                            |

### Cancellation and the REUSEADDR mode

With `UVENT_ENABLE_REUSEADDR=OFF` there is no cancel kick: `JoinHandle::cancel()`,
`TaskScope::cancel()` and `CancellationSource::request_cancel()` only raise the
flag, and a task notices it when it next polls `cancel_requested()` or resumes for
another reason. A task parked on a socket, an event, a channel or `sleep_for`
stays parked. The tests that rely on prompt wake-up are therefore compiled only
with REUSEADDR (`test_drain`, `connect_is_cancellable`, the parked-waiter cases
of `test_wait_cancel`, `test_coro_misc`, `test_sync_prims`, `test_select`,
`test_cancellation`).

In the same mode the shared poller never sleeps longer than
`settings::idle_fallback_ms` (50 ms), a worker drains its inbox before it
blocks on the poller lock, and that lock is a FIFO ticket lock (the previous
binary semaphore let the polling worker starve everyone else). Socket timeouts
and client-side connects still hang in this layout (the inactivity timer path
and `async_connect` were written for the per-worker wheel), so
`test_socket_client` and `test_socket_io` are compiled only with REUSEADDR.
Treat the legacy layout as maintained for portability, not for new work.

---

## Build products

| CMake option              | Default | Effect                                                                                                                                                                                                                                                                                                                                                                   |
|---------------------------|---------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `UVENT_BUILD_TESTS`       | OFF     | Builds `tests/` and registers them with CTest. With a sanitizer selected (below) the **library itself** is instrumented too, otherwise TSan would report the runtime's own hand-offs as races.                                                                                                                                                                           |
| `UVENT_TESTS_SANITIZER`   | `""`    | String passed to `-fsanitize=` for the library, the tests and the fiber soak: `address,undefined` or `thread`. Run sanitized binaries under `setarch x86_64 -R` (ASLR entropy) and, for ASan with fibers, `ASAN_OPTIONS=detect_stack_use_after_return=0`.                                                                                                                |
| `UVENT_BUILD_EXAMPLES`    | OFF     | Builds `examples/`: the HTTP echo server `uvent_exe` (benchmark target), timers, channels, select, structured concurrency, Happy Eyeballs, the fiber examples, the fiber soak (`uvent_example_fiber_soak`) and the cross-worker socket benchmark (`uvent_example_xworker`).                                                                                              |
| `UVENT_COVERAGE`          | OFF     | clang only: instruments the library and the tests with `-fprofile-instr-generate -fcoverage-mapping` and adds the `coverage` target, which runs the suite and writes `coverage/{report.txt,lcov.info,html}` in the build dir (`tools/coverage.sh`). `UVENT_LLVM_SUFFIX` (e.g. `-18`) picks the matching `llvm-cov`/`llvm-profdata`. See [Contributing](contributing.md). |
| `UVENT_ENABLE_SANITIZERS` | OFF     | Extra example executables built with their own sanitizer flags (`uvent_asan_ubsan`, `uvent_tsan`, and `uvent_msan_ubsan` with clang). Independent of `UVENT_TESTS_SANITIZER`.                                                                                                                                                                                            |

`CMAKE_BUILD_TYPE=Debug` additionally defines `UVENT_DEBUG` for the examples and
sanitizer executables (verbose spdlog tracing inside the runtime) and links
spdlog; release builds do not depend on spdlog at all.

---

## Macros derived by the code

These are not CMake options; they follow from the toolchain or from another flag.

| Macro                                                              | Defined when                                                                                      | Meaning                                                                                                                                                                                                                                                                                         |
|--------------------------------------------------------------------|---------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `UVENT_SOCKET_OWNER_FORWARDING`                                    | `UVENT_ENABLE_REUSEADDR` (in `SocketMetadata.h`)                                                  | Sockets remember their owning worker; timeout / shutdown / destroy from another worker are forwarded to it, and a coroutine woken by the owner's poller is resumed on its own worker (`system::resume_waiter`).                                                                                 |
| `UVENT_GCC_UBSAN_TLS_WORKAROUND`                                   | GCC **and** `undefined` in `UVENT_TESTS_SANITIZER` or in `CMAKE_CXX_FLAGS` (set by CMake, PUBLIC) | Turns `system::this_thread::detail::tls_addr()` into an optimizer barrier. GCC's `-fsanitize=null` at `-O1`+ reports a false "reference binding to null pointer" for references bound to `thread_local` objects; clang is clean. Every other build compiles the helper as an identity function. |
| `UVENT_FIBER_ARCH_X86_64` / `UVENT_FIBER_ARCH_AARCH64`             | Target architecture (`ContextPosix.cpp`)                                                          | Selects the context-switch assembly (`switch_x86_64_sysv.S`, `switch_aarch64.S`).                                                                                                                                                                                                               |
| `UVENT_FIBER_ASAN` / `UVENT_FIBER_TSAN`                            | `__SANITIZE_ADDRESS__` / `__SANITIZE_THREAD__` or the clang `__has_feature` equivalents           | Annotates fiber stack switches for the sanitizer runtime (`__sanitizer_start_switch_fiber` and friends).                                                                                                                                                                                        |
| `UVENT_NO_IO_FRAME_POOL`                                           | Only if you define it yourself                                                                    | Disables the per-thread free-list allocator for I/O coroutine frames (`IOFramePool`); frames then come from plain `operator new`. Diagnostic switch.                                                                                                                                            |
| `UVENT_DEBUG`, `UVENT_CHANNEL_DEBUG`, `UVENT_ERROR`, `UVENT_TRACE` | Debug configurations of the examples, or by hand                                                  | Verbosity levels of the spdlog tracing in the runtime, channels and the Windows socket layer.                                                                                                                                                                                                   |

---

## Configurations CI runs (`tests.yml`)

| Job                     | Flags                                                                                                                                                                                   |
|-------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `release`               | `-DCMAKE_BUILD_TYPE=Release`                                                                                                                                                            |
| `release-introspection` | `… -DUVENT_TASK_INTROSPECTION=ON`                                                                                                                                                       |
| `release-no-reuseaddr`  | `… -DUVENT_ENABLE_REUSEADDR=OFF`                                                                                                                                                        |
| `release-drain`         | `… -DUVENT_RUNTIME_DRAIN=ON`                                                                                                                                                            |
| `release-fibers-off`    | `… -DUVENT_ENABLE_FIBERS=OFF`                                                                                                                                                           |
| `asan-ubsan`            | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DUVENT_TESTS_SANITIZER=address,undefined`                                                                                                           |
| `tsan`                  | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DUVENT_TESTS_SANITIZER=thread`                                                                                                                      |
| `coverage`              | clang-18, `-DCMAKE_BUILD_TYPE=Debug -DUVENT_COVERAGE=ON -DUVENT_LLVM_SUFFIX=-18 -DUVENT_TASK_INTROSPECTION=ON -DUVENT_RUNTIME_DRAIN=ON`, target `coverage`, artifact `lcov.info` + HTML |

All jobs add `-DUVENT_BUILD_TESTS=ON`. `build.yml` compiles the library on
Linux, macOS and Windows with the defaults.

---

## Recipes

```sh
# production defaults, library only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# full test run, release
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release -DUVENT_BUILD_TESTS=ON
cmake --build build-rel -j && (cd build-rel && ctest -j4)

# thread sanitizer (library instrumented as well)
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUVENT_BUILD_TESTS=ON \
      -DUVENT_TESTS_SANITIZER=thread
cmake --build build-tsan -j && (cd build-tsan && setarch x86_64 -R ctest -j4)

# everything optional switched on (what the coverage run uses)
cmake -S . -B build-all -DUVENT_BUILD_TESTS=ON -DUVENT_BUILD_EXAMPLES=ON \
      -DUVENT_TASK_INTROSPECTION=ON -DUVENT_RUNTIME_DRAIN=ON

# legacy shared-poller layout
cmake -S . -B build-legacy -DUVENT_BUILD_TESTS=ON -DUVENT_ENABLE_REUSEADDR=OFF
```

Note for clang 18 with libstdc++ 13: `std::expected` needs
`-D__cpp_concepts=202002L -Wno-builtin-macro-redefined` in `CMAKE_CXX_FLAGS`.
