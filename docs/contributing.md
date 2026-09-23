# Contributing

We welcome contributions to **Uvent**! Whether it's fixing bugs, improving documentation, or adding new features, every contribution helps. Every little bit helps, want to contribute, here is how:

## Ways to Contribute
- **Bug reports** – Open an [issue](https://github.com/Usub-Foundation/uvent/issues) if you find a problem.
- **Feature requests** – [Suggest](https://github.com/Usub-Foundation/uvent/discussions) improvements or new capabilities.
- **Code contributions** – [Fix](https://github.com/Usub-Foundation/uvent/pulls) a bug, [implement](https://github.com/Usub-Foundation/uvent/pulls) a feature, or [improve](https://github.com/Usub-Foundation/uvent/pulls) performance.
- **Documentation** – Help us improve clarity and examples.

## Coding style

**While this is not enforced, we ask you to**

* Try to keep code clean, consistent, and modular. This will not only help you but those who will come later.
* Try writing tests for new features and bug fixes.
* Follow the existing project style for naming and formatting. If you see inconsistency, please open an [issue]()
    - **Functions / Methods** → `functionsLikeThis`
    - **Variables** → `vars_like_this`
    - **Private member variables** → `private_vars_like_this_` (with trailing underscore)
    - **Structs / Classes** → `StructsLikeThis` / `ClassesLikeThisToo`
    - **Enums** → `ENUMS_ARE::LIKE_THIS` (namespace-style) or `ENUMSARELIKE::This` depending on context
    - **Namespaces** → `lowercase_or_compound`
    - **macros** → `ALL_CAPS_WITH_UNDERSCORES`
    - **Templates / Concepts** → `CamelCaseLikeThis`

## Communication

* Issues and pull requests are tracked on [GitHub](https://github.com/Usub-Foundation/uvent).
* For larger changes, open a discussion first to discuss the design before implementing, then we'll create issue to track.

---

By contributing, you help make **Uvent** a robust and modern C++ framework better for everyone. Thank you for your support!

## Testing

* `cmake -S . -B build -DUVENT_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build`
  runs the unit tests; CI runs them on Linux, macOS and Windows (`build.yml`)
  and on Linux in Release, `-DUVENT_TASK_INTROSPECTION=ON`,
  `-DUVENT_ENABLE_REUSEADDR=OFF`, `-DUVENT_ENABLE_FIBERS=OFF`,
  `-DUVENT_RUNTIME_DRAIN=ON`, ASan+UBSan and TSan (`tests.yml`).
- The full list of CMake options, derived macros and CI configurations is on
  the [Build flags](build-flags.md) page.
- Debugging a hanging test (`tests/test_common.h` runs every case in a forked
  child, and `kernel.yama.ptrace_scope=1` blocks `gdb -p` on it):
  `UVENT_TEST=<case>` runs one case, `UVENT_TEST_NOFORK=1` runs it in the
  harness process (plain `gdb --args`), and `UVENT_TEST_HANG_DUMP=<seconds>`
  starts a watchdog that prints a backtrace of every thread and exits with 70,
  no ptrace needed; resolve its `+0x...` offsets with
  `addr2line -e <test binary> -f -C -i`. `UVENT_TEST_SCALE=<factor>` shrinks
  the stress tests. Keep test listeners below port 32768 (the ephemeral range).
- Kill leftover test processes before chasing a flaky socket test. uvent
  listeners set `SO_REUSEPORT`, so an orphaned earlier run still listening on a
  test port silently receives a share of the new run's connections and the test
  hangs at random. Worker threads rename themselves to `uvent_worker_N`, so
  look for them by command line (`ps -eo pid,comm,args`), not by process name,
  and check `ss -tlnp` for the test ports.
- GCC's UBSan (`-fsanitize=null`, -O1 and above) falsely reports "reference binding
  to null pointer" for references bound to `thread_local` objects. CMake defines
  `UVENT_GCC_UBSAN_TLS_WORKAROUND` for GCC + `undefined`, which turns
  `system::this_thread::detail::tls_addr()` into an optimizer barrier; every other
  toolchain compiles it as an identity function. Clang is clean without it.
* Sanitizers: `-DUVENT_TESTS_SANITIZER=address,undefined` or `thread`. On
  kernels with 32-bit mmap randomisation run the binaries under
  `setarch $(uname -m) -R` (or `sysctl vm.mmap_rnd_bits=28`), and use
  `ASAN_OPTIONS=detect_stack_use_after_return=0` with fibers.
* Before changing the scheduler, cancellation or the fiber switch, also run the
  fiber soak (`-DUVENT_BUILD_EXAMPLES=ON`, target `uvent_example_fiber_soak`)
  for at least 60 s in Release, ASan and TSan; it has found real lifetime bugs
  the unit tests could not.
* Listeners in tests and examples use ports **below** the ephemeral range
  (`net.ipv4.ip_local_port_range`, 32768 on Linux); a busy soak leaves tens of
  thousands of client sockets on random high ports, and a listener on such a
  port then fails to bind.
