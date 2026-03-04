# Async I/O Framework Architecture

This repository contains a minimal coroutine-based async I/O runtime for C++20.

- Core runtime: `include/async_io.hpp`
- Example usage: `examples/main.cpp`

## Goals

- Provide a small, understandable async runtime.
- Use C++20 coroutines as the async abstraction.
- Use OS readiness notification (`poll`) for non-blocking file descriptors.
- Keep scheduling single-threaded and deterministic.

## High-Level Design

The framework has four layers:

1. `task<T>` coroutine type
2. `io_context` scheduler/event loop
3. Awaitables (`readable`, `writable`, `sleep_for`)
4. Async syscall wrappers (`async_read`, `async_write`)

Control flow is cooperative: coroutines run until they suspend (waiting on fd/timer), then `io_context` resumes them when the awaited condition is ready.

## Core Components

### `task<T>` / `task<void>`

`task` is the coroutine return type. It owns a coroutine handle and defines the await protocol.

Key behavior:

- `initial_suspend = suspend_always`: newly created tasks do not run immediately.
- `await_suspend`: stores the awaiting coroutine as `continuation`, then transfers execution to the task coroutine.
- `final_suspend`: resumes stored `continuation` when task completes.
- Exceptions are captured in the promise and rethrown in `await_resume`.

Why this matters:

- Tasks can be composed with `co_await` naturally.
- Parent/child coroutine chaining works without extra callback plumbing.

### `io_context`

`io_context` is the runtime scheduler and event loop.

Primary state:

- `ready_`: queue of coroutine handles ready to resume now.
- `timers_`: min-heap of timer deadlines.
- `read_waiters_`: `fd -> coroutine` waiting for readable.
- `write_waiters_`: `fd -> coroutine` waiting for writable.
- `active_tasks_`: number of spawned top-level tasks still alive.

Core operations:

- `post(handle)`: enqueue coroutine for immediate execution.
- `co_spawn(task<void>)`: spawn detached top-level coroutine work.
- `run()`: main loop that:
  - drains ready queue,
  - fires expired timers,
  - waits in `poll` for fd readiness (with timer-derived timeout),
  - requeues awakened waiters.

Stop conditions:

- `stop()` requested, or
- no ready handles, no timers, no fd waiters, and no active tasks.

### Awaitables

Awaitables bridge coroutine suspension to runtime registration.

- `readable(fd)` / `writable(fd)`:
  - suspend current coroutine,
  - register it in the corresponding waiter map.
- `sleep_for(duration)`:
  - suspend current coroutine,
  - insert timer entry in deadline heap.

These awaitables are lightweight and non-owning; `io_context` owns wake-up scheduling.

### Async I/O wrappers

`async_read` and `async_write` wrap POSIX syscalls in retry/suspend loops.

Pattern:

- attempt syscall immediately,
- on `EINTR`: retry,
- on `EAGAIN`/`EWOULDBLOCK`: `co_await` fd readiness, then retry,
- on other errors: throw `std::system_error`.

`async_write` additionally handles partial writes by advancing a pointer until all bytes are sent or an error occurs.

## Execution Flow (Example)

In `examples/main.cpp`:

1. Create a `socketpair`.
2. Set both fds non-blocking.
3. Spawn `writer` and `reader` tasks.
4. Call `ctx.run()`.

Runtime behavior:

- `writer` sleeps for 100ms (`sleep_for` -> timer heap).
- `reader` starts `async_read`; if no data yet, it suspends on `readable(fd)`.
- Timer expires; `writer` resumes and writes data.
- `poll` marks reader fd readable; runtime resumes reader.
- Reader prints message; tasks complete; loop exits when no work remains.

## Scheduling and Error Semantics

- Single-threaded: no internal locking.
- Fairness: ready queue is FIFO.
- One waiter per fd per direction in current implementation (`read_waiters_` and `write_waiters_` maps).
- `co_spawn` swallows task exceptions in detached mode to keep the loop alive.
- Syscall failures propagate as exceptions from `async_read/async_write`.

## Current Constraints

- POSIX-only (uses `poll`, `fcntl`, `read`, `write`).
- No cancellation API yet.
- No multi-waiter list per fd direction.
- No built-in networking helpers beyond generic fd read/write.

## Extension Points

Natural next additions:

- `async_accept`, `async_connect`, and `async_recvfrom`/`async_sendto`.
- Cancellation tokens and timeout combinators.
- Multiple waiters per fd direction.
- Backends for `epoll`/`kqueue` while preserving awaitable API.
- Structured concurrency primitives (`when_all`, join handles, scoped tasks).

## Design Tradeoffs

Why this design is useful:

- Small surface area makes coroutine mechanics and scheduling behavior explicit.
- Easy to inspect and debug.
- Good base for custom runtimes.

What it gives up:

- Fewer production features than mature libraries (Boost.Asio, libuv-based frameworks).
- No thread pool or cross-platform readiness abstraction yet.

## File Map

- `include/async_io.hpp`: runtime, coroutine types, awaitables, async wrappers.
- `examples/main.cpp`: minimal demonstration of timer + async socket I/O.
- `CMakeLists.txt`: C++20 build setup.
