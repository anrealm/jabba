# Cancellation: what jabba can and cannot stop

jabba cancels a node in four situations: its own `timeout` expires, the graph deadline passes, another non-optional node fails (fail-fast), or the caller cancels the run. The exact rules are in [`spec/semantics.md`](../spec/semantics.md) §3–§4. In short:

- The node's status is recorded **when jabba decides to cancel it** (`timeout` / `skipped`), and the run moves on.
- After a deadline or a failure, jabba waits at most **50 ms** for cancelled nodes; after a per-node timeout it does not wait at all.
- A node that keeps running after that finishes in the background. Its result is discarded and it cannot change the record.

Cancellation is **cooperative**: jabba asks, and your code decides when it actually stops. Whatever runs in the background still holds connections, threads and CPU. This page lists what that means per language.

## Python

`task.cancel()` raises `CancelledError` inside the node at its next `await`. The node keeps running when:

1. **It catches the cancellation.** A bare `except:` or `except BaseException:` swallows `CancelledError` (a plain `except Exception:` does not — `CancelledError` is a `BaseException` since Python 3.8):

   ```python
   while True:
       try:
           return await client.get(url)
       except BaseException:          # also swallows CancelledError: the retry loop never stops
           await asyncio.sleep(0.1)
   ```
   Catch `Exception` in retry loops, or re-raise `asyncio.CancelledError` explicitly.

2. **Its cleanup is slow.** `await conn.close()` in a `finally` block runs after the cancellation and may take a while. The node stops — just later. jabba does not wait for it beyond the grace period.

3. **Work went to a thread.** With `asyncio.to_thread(...)` or `loop.run_in_executor(...)` the coroutine is cancelled immediately, but the thread runs to completion: Python threads cannot be interrupted. Bound such work with its own timeout.

4. **`asyncio.shield(...)`** protects its inner awaitable from cancellation — by design.

5. **Blocking code without `await`.** `time.sleep`, a long CPU loop, a synchronous HTTP call: there is nowhere to deliver the cancellation, and the whole event loop — every other node and jabba's own timers — stands still until the call returns. jabba cannot do anything about this. Move such work to a thread (case 3) or make it async.

## Rust

jabba cancels a node by **dropping its future**. Destructors run right away, so an async node cannot keep running on its own: whatever it was awaiting is gone. What can still keep going:

1. **Work handed to other threads or tasks.** `std::thread::spawn`, `tokio::task::spawn_blocking`, `tokio::spawn` — dropping the node's future does not stop them. Watch `ctx.cancel_token()` there (`is_cancelled()`, or `cancelled().await`): jabba cancels it when it stops waiting for the node. Writes such work makes to `ctx` after the run has returned are dropped.

2. **Blocking code inside an async node.** `std::thread::sleep`, a long CPU loop, blocking I/O: a run is one future, so a node that blocks holds every other node of the run and the deadline check until it returns. Keep async nodes non-blocking; use `run_sync` for short CPU work and a blocking pool for anything long.

3. **Slow `Drop`.** A destructor that blocks (joins a thread, flushes synchronously) runs when jabba drops the node and delays the run's return.

4. **Synchronous nodes** (`run_sync`) run to completion and cannot be interrupted at all.

## C++

Added together with the C++ implementation.

## Recommendations

- Give long operations their own timeout inside the node; the node timeout is a safety net, not a replacement.
- In retry loops, never catch cancellation.
- Keep `finally` / destructor cleanup short.
