# Component: Async (Goldfish)

**Role:** Provides platform-independent asynchronous I/O primitives (Sockets, Timers, EventLoops)
backed by libuv or QEMU. **Location:** `emulator/libs/async/` **Namespace:** `goldfish::async`
**Agent Playbook:** [`AGENTS.md`](AGENTS.md)

## Integration Guide

| Class / Interface    | Bazel Target                                | Header Path                                     | Description                                        |
| :------------------- | :------------------------------------------ | :---------------------------------------------- | :------------------------------------------------- |
| `AsyncSocket`        | `//emulator/libs/async:async_api`           | `include/goldfish/async/async_socket.h`         | Abstract interface for async sockets.              |
| `EventLoop`          | `//emulator/libs/async:async_api`           | `include/goldfish/async/event_loop.h`           | Abstract interface for the event loop (`Post`, `PostAndWait`, `Schedule`). Tested in `event_loop_test.cc` (`@goldfish//emulator/libs/async:eventloop_unittests`). |
| `LibuvEventLoop`     | `//emulator/libs/async:libuv_event_loop`    | `include/goldfish/async/libuv_event_loop.h`     | libuv-backed EventLoop implementation (`@goldfish//emulator/libs/async:eventloop_unittests`). |
| `QemuEventLoop`      | `//emulator/libs/async:qemu_event_loop`     | `include/goldfish/async/qemu_event_loop.h`      | QEMU main-loop backed EventLoop adapter (`@goldfish//emulator/libs/async:eventloop_unittests`). |
| `ThreadedEventLoop`  | `//emulator/libs/async:threaded_event_loop` | `include/goldfish/async/threaded_event_loop.h`  | Helper running an EventLoop in a dedicated thread. |
| `AsyncSocketFactory` | `//emulator/libs/async:async_api`           | `include/goldfish/async/async_socket_factory.h` | Factory interface for creating sockets.            |

## Critical Infrastructure

| File                  | Responsibility                                         |
| :-------------------- | :----------------------------------------------------- |
| `include/goldfish/async/event_loop.h` | Defines `EventLoop::Post` and `EventLoop::PostAndWait`, using `android::status::Flatten` / `flatten_status_or_t<T>` from `//emulator/libs/status_macros:flatten` (`emulator/libs/status_macros/include/android/status/flatten.h`, tested in `emulator/libs/status_macros/flatten_test.cc` / `@goldfish//emulator/libs/status_macros:flatten_test`) so callables returning `absl::Status` or `absl::StatusOr<T>` flatten cleanly instead of returning nested `absl::StatusOr<absl::StatusOr<T>>`. |
| `libuv_sockets.cc`    | Implements `AsyncSocket` using libuv primitives.       |
| `libuv_event_loop.cc` | Wraps `uv_loop_t` and handles task scheduling.         |
| `qemu_event_loop.cc`  | Maps QEMU's `AioContext` to the `EventLoop` interface. |

## Dependencies

- **Upstream:** `@libuv` (Core async engine), `@abseil-cpp` (Status, Logging).
- **Hardware:** None directly, but `QemuEventLoop` depends on QEMU internals.

## Threading Model

- **Invariant 1:** All `AsyncSocket` methods **MUST** be called on the associated `EventLoop`
  thread.
- **Invariant 2:** `EventLoop::Post` is the **ONLY** thread-safe method to cross into the loop.

## Flows & Guides

- [EventLoop & Timer Design](DESIGN.md)
- [Socket Read Lifecycle](docs/socket_read_flow.md)
- [Server Connection Lifecycle](docs/server_connection_flow.md)
- [Backpressure & Flow Control](docs/backpressure_flow.md)
- [Socket Write Lifecycle](docs/socket_write_flow.md)
- [Event Loop Primitives (Tasks & Timers)](docs/event_loop_primitives.md)
- [Process Launching](docs/process_launching.md)
