# Component: gRPC Async Reactors, Event Streams & Abseil Utilities (`grpc_utils`)

**Role:** Provides thread-safe gRPC callback reactors (`WithSimpleQueueWriter`, `WithSimpleReader`, `SimpleServerLambdaWriter`, `SimpleServerLambdaReader`), reactive event-stream bridges (`BaseEventStreamWriter`, `StateStreamWriter`, `UniqueStateStreamWriter`, `UniqueEventStreamWriter`, `GenericEventStreamWriter`), borrowed-object tracking (`Library<T>`), enum translation (`EnumTranslate`), and Abseil/gRPC status conversion.
**Location:** `hardware/generic/goldfish/emulator/libs/grpc_utils`
**Namespace:** `android::emulation::control`

> [!IMPORTANT]
> **Agent Playbook:** Before modifying `grpc_utils` or implementing gRPC services/reactors, read [`AGENTS.md`](AGENTS.md) and [`emulator/plugin/grpc/AGENTS.md`](../../plugin/grpc/AGENTS.md) for mandatory reactor selection rules, lock-ordering invariants (`ServiceMutex` vs. `EventSourceMutex` vs. `reactor_lock_`), `populate_fn` buffer-recycling rules, and multithreaded concurrency test patterns.

## Integration Guide

| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `StateStreamWriter<W, Event, Source, max_q, recycle, Reactor, deduplicate>` | `//emulator/libs/grpc_utils:event-stream` | `include/android/emulation/control/grpc_event_stream_support.h` | Server write reactor that sends an immediate initial snapshot via `populate_fn(W*)` and reactively streams updates on `CallbackEventSource<Event>` notifications (`deduplicate` defaults to `false`). |
| `UniqueStateStreamWriter<W, Event, Source, max_q, recycle, Reactor>` | `//emulator/libs/grpc_utils:event-stream` | `include/android/emulation/control/grpc_event_stream_support.h` | Alias of `StateStreamWriter<..., /*deduplicate=*/true>` suppressing consecutive identical `W` snapshots via `MessageDifferencer::Equals`. |
| `UniqueEventStreamWriter<W, Source, max_q, recycle, Reactor>` | `//emulator/libs/grpc_utils:event-stream` | `include/android/emulation/control/grpc_event_stream_support.h` | Server write reactor with initial snapshot and consecutive duplicate suppression via `MessageDifferencer::Equals` for sources emitting `W` directly. |
| `GenericEventStreamWriter<W, max_q, recycle>` | `//emulator/libs/grpc_utils:event-stream` | `include/android/emulation/control/grpc_event_stream_support.h` | Server write reactor forwarding `CallbackEventSource<W>` events without an initial snapshot. |
| `BaseEventStreamWriter<W, Event, Source, ...>` | `//emulator/libs/grpc_utils:event-stream` | `include/android/emulation/control/grpc_event_stream_support.h` | Base class bridging `CallbackEventSource<Event>` to `WithSimpleQueueWriter`; requires two-phase `Subscribe()` at end of derived constructor. |
| `WithSimpleQueueWriter<T, max_q, recycle>` / `SimpleServerWriter<W>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Thread-safe queued async writer mixin (`Write()`, `AcquireMessage()`, `RecycleMessage()`, idempotent `Finish()`, `SetCancelled()`) guarded by `WithReactorLock::reactor_lock_`. |
| `SimpleServerLambdaWriter<W, Base, max_q, recycle>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Server write reactor driven by `startFn`, `cancelFn`, and `doneFn` lambdas; self-deletes in `OnDone()`. |
| `WithSimpleReader<T>` / `SimpleServerReader<R>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Thread-safe async reader mixin (`StartRead()`, `OnReadDone()`, `Read(const R*)`) guarded by `WithReactorLock::reactor_lock_`. |
| `SimpleServerLambdaReader<R, Base>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Server read reactor invoking `readFn(const R*)`; returning non-OK `grpc::Status` calls `Finish(status)` without calling `StartRead()`. Self-deletes in `OnDone()`. |
| `SimpleServerBidiStream<R, W, max_q, recycle>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Bidirectional server stream combining `WithSimpleQueueWriter` and `WithSimpleReader`. |
| `ErrorServerWriter<W>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Server write reactor that immediately finishes with a given `grpc::Status` and self-deletes in `OnDone()`. |
| `SimpleClientLambdaReader<R>` / `SimpleClientWriter<W>` / `SimpleClientBidiStream<R, W>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/simple_async_grpc.h` | Client-side async reactor wrappers holding `std::shared_ptr<grpc::ClientContext>`. |
| `Library<T>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/library.h` | Thread-safe borrowed-object registry (`Acquire()`, `ForEach()`, `WaitUntilLibraryIsClear(timeout)`). |
| `EnumTranslate::translate` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/enum_translate.h` | `constexpr` bidirectional translation between Protobuf and internal C++ enums. |
| `createGrpcRequestContext` / `grpcCallCompletionHandler` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/generic_callback_functions.h` | Async client request/response/context lifecycle helpers. |
| `SyncToAsyncBidiAdapter<R, W>` / `BidiRunner<T>` | `//emulator/libs/grpc_utils:async` | `include/android/emulation/control/sync_to_async_adapter.h` | Adapter wrapping synchronous `grpc::ServerReaderWriter` streams with async `Read`/`Write` callbacks. |
| `AbslStatusToGrpcStatus` / `GrpcStatusToAbslStatus` | `//emulator/libs/grpc_utils:absl_translate` | `include/android/emulation/control/absl_status_translate.h` | Converts between `absl::Status` and `grpc::Status`. |

## Threading Model & Synchronization Invariants
- **`WithReactorLock::reactor_lock_`:** Protects `write_queue_`, `recycled_`, `status_.writing`, `status_.finished`, and `status_.cancelled` so at most one `StartWrite()` or `StartRead()` is in flight on the wire and no write/read starts after `Finish()`.
- **Deadlock-Free Unsubscription:** `BaseEventStreamWriter::Unsubscribe()` swaps `callback_id_` under `reactor_lock_` and invokes `listener_->RemoveCallback()` *outside* `reactor_lock_` so concurrent `FireEvent()` calls cannot ABBA-deadlock with `OnCancel()` or `Finish()`.
- **Self-Deletion UAF Safety:** Never hold `reactor_lock_` across `Base::Finish()`, which may synchronously trigger `OnDone()` $\rightarrow$ `delete this;`.
