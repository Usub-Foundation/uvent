# Cancellation

uvent's cancellation is cooperative and hierarchical: a `CancelState` forms a
node in a tree, `request_cancel()` marks the whole subtree, and every blocking
primitive in the library observes it. Tasks (`task::spawn`, `TaskScope`) are
built on the same tree – see [Tasks](tasks.md).

Header: `uvent/sync/AsyncCancellation.h`.

---

## CancellationSource / CancellationToken

```cpp
sync::CancellationSource src;                 // root
sync::CancellationSource child{src.token()};  // linked under src

auto tok = src.token();                       // cheap, ref-counted handle

src.request_cancel();                         // marks src, child, and every task under them

tok.stop_requested();                         // true
child.token().stop_requested();               // true (propagated)
```

* Sources are move-only; tokens are freely copyable and may outlive the
  source (state is reference-counted).
* A source created under an already-cancelled parent is born cancelled.
* `request_cancel()` is idempotent and safe from any thread, including
  non-worker threads.

## Waiting for cancellation

```cpp
task::Awaitable<void> watcher(sync::CancellationToken tok)
{
    const bool fired = co_await tok.on_cancel();
    if (fired)
        shutdown_everything();
}
```

`on_cancel()` resumes when the token is cancelled. It returns `false` if the
*awaiting task itself* was cancelled through a different token first.

In a `select`, use the token as a branch:

```cpp
auto r = co_await sync::select(ch.recv_op(), tok.on_cancel_op(), sync::sleep_op(5s));
```

## Observing cancellation from inside a coroutine

Every coroutine inherits the cancel state of the task it runs under
(`spawn`ed tasks; plain `co_spawn` coroutines have none):

```cpp
system::this_coroutine::cancel_requested();   // relaxed flag check, ~1ns
sync::current_token();                        // token for the current task
```

## What reacts to cancellation

| Operation                                                    | Result when cancelled                                                 |
|--------------------------------------------------------------|-----------------------------------------------------------------------|
| `this_coroutine::sleep_for(d)`                               | returns `false` immediately                                           |
| `AsyncChannel` / `AsyncUnboundedChannel` `recv`, `recv_into` | `std::nullopt` / `false`                                              |
| `AsyncChannel::send`, `send_tuple`                           | `false`                                                               |
| `AsyncEvent::wait`                                           | `false`                                                               |
| `AsyncSemaphore::acquire`                                    | `false` (no token taken)                                              |
| `WaitGroup::wait`                                            | `false`                                                               |
| `AsyncMutex::lock`                                           | empty `Guard` (`owns_lock() == false`)                                |
| `sync::select(...)`                                          | `SelectResult::cancelled() == true`                                   |
| `socket.async_read` / `async_write`                          | `-1`, `errno == ECANCELED`                                            |
| `socket.async_accept`                                        | `std::nullopt` (handle-returning form), plain return (callback form)  |
| `socket.async_connect*`                                      | `ConnectError::Cancelled`                                             |
| `send_aux`                                                   | `SendError::Cancelled`                                                |
| `JoinHandle` / `TaskScope::join`                             | **not interruptible** – they complete when the awaited tasks complete |

Always check the return value: a cancelled `acquire`/`lock` did **not** take
the resource.

## Delivery semantics

* A wait that is *about to start* after cancellation was requested does not
  suspend at all – every primitive re-checks the flag while registering.
* A wait that is *already parked* is woken by the task kick (sharded runtime,
  `UVENT_ENABLE_REUSEADDR`) – see [Tasks](tasks.md#how-cancellation-reaches-a-task).
  In the shared runtime, parked waits wake on their normal event or timeout
  and then observe cancellation (lazy delivery).
* Cancellation never destroys a running coroutine and never skips
  destructors: the coroutine continues from the cancelled wait with an error
  result and unwinds normally.

## Cost

The whole mechanism is pay-for-what-you-use:

* coroutines without a task: two thread-local reads at frame creation, one
  null check per cancellable suspension;
* `stop_requested()` / `cancel_requested()`: one relaxed atomic load;
* arming a cancellable wait: two plain stores in the coroutine frame;
* `request_cancel()`: the only place that takes locks (tree spinlocks +
  waiter lists), off the hot path by construction.

## Cooperative rules

* Check `cancel_requested()` in loops, or look at the result of the awaited
  operation: after cancellation `sleep_for` returns `false` immediately, without
  suspending. A loop that keeps calling it without looking spins the worker.
* `this_coroutine::yield()` always suspends and re-queues the task, so it is
  the safe polling point for CPU-bound loops.
* A parent that cancels its children (`TaskScope::cancel_and_join()`) walks
  them under the tree lock. Children that finish on their own during that walk
  are skipped safely: the runtime never resurrects a task whose last reference
  is already gone (`CancelState::try_add_ref`).
* With `UVENT_RUNTIME_DRAIN`, `Uvent::stop()` is one more source of
  cancellation for every live task (see `docs/drain.md`).
