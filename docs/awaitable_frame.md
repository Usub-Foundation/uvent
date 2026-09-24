# Awaitable Frame

Coroutines in **uvent** are structured around a hierarchy of **frames** — the internal “promise” objects that manage
coroutine state, suspension, and chaining in the event loop.

---

## Overview

Each coroutine in `uvent` consists of:

- A **frame** (`AwaitableFrame<T>`, `AwaitableIOFrame<T>`, etc.) — manages execution state, results, exceptions, and
  links to other coroutines.
- An **Awaitable handle** (`task::Awaitable<T, FrameType>`) — the user-facing object returned from coroutine functions.

This split allows **precise control** over coroutine start behavior — whether it begins immediately or waits for an
external trigger.

---

## AwaitableFrameBase

`AwaitableFrameBase` is the foundation for all coroutine frames.  
It manages coroutine lifecycle and linking logic:

- Holds coroutine handles (`coro_`, `prev_`, `next_`).
- Holds parent thread id (`t_id`); can be accessed via `int get_thread_id()` method; can be set up via
  `void set_thread_id()` method.
- Manages exception propagation (`exception_`).
- Tracks whether coroutine is awaited (`is_awaited`, `set_awaited`, `unset_awaited`).
- Provides resumption (`resume()`).
- Handles destruction scheduling (`push_frame_to_be_destroyed`).
- Connects caller and callee coroutines (`set_calling_coroutine`, `set_next_coroutine`).

---

## AwaitableFrame<T>

Default coroutine frame for value-returning coroutines.

- Stores return value (`T`).
- Extracts result via `get()`.
- Uses `std::suspend_always` in `initial_suspend()` — **starts immediately**, queued into runtime task system.
- Exception-safe via `unhandled_exception()` and `get()`.

Lifecycle:

- `initial_suspend()` → coroutine suspends once, then the runtime queues it for execution.
- `final_suspend()` → resumes awaiting coroutine and schedules destruction.
- `yield_value()` → allows mid-coroutine value emission.

This is the default frame used by `task::Awaitable<T>`.

---

## AwaitableFrame<void>

Specialization for coroutines returning `void`.  
Same semantics, but without value storage.

---

## Deferred vs Instant Execution

`uvent` introduces **execution policy at type level**, using a simple tag system.

### Tag mechanism

```cpp
struct deferred_task_tag {}; // marks deferred-start frames

template<class F>
concept DeferredFrame =
    std::derived_from<std::remove_cvref_t<F>, deferred_task_tag>;
```

Frames **that inherit** from `deferred_task_tag` are **deferred coroutines** —
they **do not start immediately** and instead wait for an **external trigger** (like `epoll`, `TimerWheel`, or another
subsystem).

Frames **that don’t inherit** start automatically once awaited — they are **instant coroutines**.

| Frame type            | Trait               | Behavior                                       |
|-----------------------|---------------------|------------------------------------------------|
| `AwaitableFrame<T>`   | —                   | Starts immediately (queued to run)             |
| `AwaitableIOFrame<T>` | `deferred_task_tag` | Deferred — runs only when externally triggered |

---

## AwaitableIOFrame<T>

Special coroutine frame for **I/O-bound or event-driven** operations.
It inherits from `deferred_task_tag`, making it **lazy-start** — it doesn’t run until the poller or timer activates it.

Key points:

* `initial_suspend()` → `std::suspend_never`, coroutine body is prepared but execution waits for an external event.
* Typically used for `async_read`, `async_write`, `async_connect`, etc.
* Execution resumes only when triggered by the runtime (poller, timer, or another coroutine).

This design ensures that I/O coroutines aren’t executed prematurely and stay synchronized with system-level events.

---

## Scheduling Behavior

During `co_await some_task()`:

* If the awaited frame **is not deferred** — coroutine is queued right away (`push_frame_into_task_queue`).
* If it **inherits `deferred_task_tag`** — coroutine **is parked**, and will only resume when triggered externally.

Thus, **deferred frames = passive tasks**, **non-deferred = active tasks**.

---

## Custom Frames

You can define your own coroutine frame and choose how it behaves — instant or deferred — simply by inheriting (or not)
from `deferred_task_tag`.

### Example: instant-start frame

```cpp
struct MyInstantFrame : usub::uvent::detail::AwaitableFrameBase {
  std::suspend_always initial_suspend() noexcept { return {}; } // queued immediately
  std::suspend_always final_suspend() noexcept { push_frame_to_be_destroyed(); return {}; }
  void unhandled_exception() { exception_ = std::current_exception(); }
  void return_void() {}

  auto get_return_object() {
    coro_ = std::coroutine_handle<MyInstantFrame>::from_promise(*this);
    return task::Awaitable<void, MyInstantFrame>{this};
  }
};
```

### Example: deferred frame

```cpp
struct MyDeferredFrame : usub::uvent::detail::AwaitableFrameBase,
                         usub::uvent::detail::deferred_task_tag {
  std::suspend_never initial_suspend() noexcept { return {}; } // deferred: no auto-run
  std::suspend_always final_suspend() noexcept { push_frame_to_be_destroyed(); return {}; }
  void unhandled_exception() { exception_ = std::current_exception(); }
  void return_void() {}

  auto get_return_object() {
    coro_ = std::coroutine_handle<MyDeferredFrame>::from_promise(*this);
    return task::Awaitable<void, MyDeferredFrame>{this};
  }
};
```

### Example usage

```cpp
task::Awaitable<void, MyInstantFrame> active_task() {
    std::cout << "Runs immediately" << std::endl;
    co_return;
}

task::Awaitable<void, MyDeferredFrame> passive_task() {
    std::cout << "Will only run after external trigger" << std::endl;
    co_return;
}

task::Awaitable<void> main_coro() {
    co_await active_task();   // executes now
    co_await passive_task();  // waits for runtime signal
}
```

---

## Typical Use Cases

| Frame Type                   | Start Policy | Common Usage                                     | Description                                                                                                |
|------------------------------|--------------|--------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| `AwaitableFrame<T>`          | **Instant**  | Compute tasks, coroutine pipelines, async chains | Default frame — starts as soon as awaited and queued in thread’s task system.                              |
| `AwaitableIOFrame<T>`        | **Deferred** | Sockets, timers, poll-based waits                | Used by all system-level async operations (`async_read`, `async_write`, etc.) that rely on `epoll/kqueue`. |
| Custom + `deferred_task_tag` | **Deferred** | Custom I/O or event subsystems                   | Extendable for domain-specific triggers (GPU jobs, message queues, RPC dispatchers).                       |
| Custom (no tag)              | **Instant**  | Background compute, scheduler workers            | For immediate task launch within runtime queues.                                                           |

---

## Summary

* **AwaitableFrameBase** — core coroutine control block.
* **AwaitableFrame<T>** — standard, instant coroutine frame.
* **AwaitableIOFrame<T>** — deferred, externally triggered coroutine frame.
* **deferred_task_tag** — compile-time marker that enables lazy-start behavior.
* **DeferredFrame** concept — used internally to detect frame behavior at compile time.

This makes **uvent’s coroutine system both type-safe and policy-driven**:
you can decide whether a coroutine should **start instantly or wait for an external trigger**, without any runtime
checks or extra branching.

```

## Frame allocation and alignment

Every promise type derives from `AwaitableFrameBase`, whose `operator new`
takes the coroutine frame from the per-thread `IOFramePool` (size classes of
64 bytes up to 1 KiB, bigger frames go to `frame_alloc` directly). Every block
is 64-byte aligned (`detail::kFrameAlign`, the cache-line size): C++20 passes
only the size to a promise's `operator new`, so without this a frame holding an
over-aligned local (channels and queues carry `alignas(CACHELINE_SIZE)`
members) would come out of a 16-byte-aligned allocation: formally misaligned,
and a crash once the compiler emits aligned vector stores for it (`-march`
with AVX). Verified with clang `-march=native -fsanitize=alignment`.

The aligned memory comes from `frame_alloc`: plain `malloc` of the size plus
72 bytes, aligned by hand, with the raw pointer stored in the word before the
block. An aligned `operator new` would bypass glibc's thread cache and cost
about three times a plain `malloc` (135 ns against 47 ns for a fresh lazy
frame, measured); the manual variant costs about 7 ns and 72 bytes per frame.
Frames recycled through the pool cost a pointer pop. Set
`UVENT_NO_IO_FRAME_POOL` to bypass the pool (frames still come from
`frame_alloc`).
