# gRPC Services & Reactors (`//emulator/plugin/grpc/...`) — Agent Playbook

**Namespaces:** `goldfish::grpc::v2` (V2 services); `android::emulation::control` (most V1 services, plus `grpc_event_stream_support.h`, `library.h`, `absl_status_translate.h`); incubating V1 services in `android::emulation::control::incubating`; the forwarders in `android::emulation::forwarding`. The reactor mixins in `simple_async_grpc.h` (`SimpleServer*`, `SimpleClient*`, `WithSimple*`, `ErrorServerWriter`) and `EnumTranslate` are in the **global** namespace: write `::SimpleServerLambdaReader<…>`.

How the reactor library works internally (`reactor_lock_`, `Subscribe()` / `Unsubscribe()`): [emulator/libs/grpc_utils/AGENTS.md](../../libs/grpc_utils/AGENTS.md).

---

## 1. Reactor-First: `//emulator/libs/grpc_utils` Catalog

For streaming RPCs, add `WithCallbackMethod_<Method>` to the service base (e.g. `SystemServiceBase` in [system_service_impl.h](services/v2/system/system_service_impl.h)) and return one of these reactors. Don't write blocking `ServerReader` / `ServerWriter` loops or subclass raw `grpc::ServerWriteReactor` / `ServerReadReactor`: hand-written `StartWrite` / `Finish` state machines cause overlapping writes and `OnDone` use-after-free.

| Reactor | Bazel Target | Header (`android/emulation/control/...`) | Exact Constructor & When to Use |
| :--- | :--- | :--- | :--- |
| `StateStreamWriter<T, Event, Source, max_q, recycle, Reactor, deduplicate>` | `//emulator/libs/grpc_utils:event-stream` | `grpc_event_stream_support.h` | `StateStreamWriter(ChangeSupport* source, PopulateStateFn populate_fn, FilterPredicate filter_fn = nullptr)` where `ChangeSupport = Source` (`Source*` pointer), `PopulateStateFn = absl::AnyInvocable<void(T*) const>`, and `FilterPredicate = absl::AnyInvocable<bool(const typename EventParam<Event>::type&) const>`. **First choice for server state streams:** sends an initial snapshot via `populate_fn(T*)` on connect, then a fresh snapshot whenever `source` fires (`deduplicate` defaults to `false`). |
| `UniqueStateStreamWriter<T, Event, Source, max_q, recycle, Reactor>` | `//emulator/libs/grpc_utils:event-stream` | `grpc_event_stream_support.h` | Alias for `StateStreamWriter<..., /*deduplicate=*/true>` with the same `(source, populate_fn, filter_fn)` constructor. Suppresses consecutive identical `T` snapshots via `MessageDifferencer::Equals` (returning suppressed messages to the recycle pool when `recycle > 0`). Use for compact control-plane state snapshots driven by non-`T` notification events (e.g. display topology or system state); prefer `StateStreamWriter` for large payloads (`ImageFrame`). |
| `UniqueEventStreamWriter<T, Source, max_q, recycle, Reactor>` | `//emulator/libs/grpc_utils:event-stream` | `grpc_event_stream_support.h` | `explicit UniqueEventStreamWriter(ChangeSupport* source, PopulateStateFn populate_fn = nullptr, FilterPredicate filter_fn = nullptr)` or `UniqueEventStreamWriter(ChangeSupport* source, std::vector<T> initial_events, FilterPredicate filter_fn = nullptr)`, where `ChangeSupport = Source` and `PopulateStateFn = absl::AnyInvocable<void(T*)>`. Suppresses consecutive duplicate `T` messages (`MessageDifferencer::Equals`). Use for compact control-plane messages (`VmState`, `ClipData`); prefer `StateStreamWriter` for large payloads (`ImageFrame`). |
| `GenericEventStreamWriter<T, max_q, recycle>` | `//emulator/libs/grpc_utils:event-stream` | `grpc_event_stream_support.h` | `explicit GenericEventStreamWriter(ChangeSupport* listener)` where `ChangeSupport = CallbackEventSource<T>`. Forwards events without an initial snapshot. |
| `BaseEventStreamWriter<T, Event, Source, ...>` | `//emulator/libs/grpc_utils:event-stream` | `grpc_event_stream_support.h` | `explicit BaseEventStreamWriter(ChangeSupport* listener)` where `ChangeSupport = Source`. Base for custom event writers; subclasses call `this->Subscribe()` at the end of their constructor. |
| `SimpleServerLambdaWriter<W, Base, max_q, recycle>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | `SimpleServerLambdaWriter(StartCallback startFn, OnCancelCallback cancelFn = nullptr, OnDoneCallback doneFn = nullptr)` where `StartCallback = absl::AnyInvocable<void(SimpleServerLambdaWriter*)>` is required. Custom server write stream; deletes itself in `OnDone()`. |
| `SimpleServerLambdaReader<R, Base>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | `SimpleServerLambdaReader(ReadCallback readFn, OnDoneCallback doneFn = nullptr)` where `ReadCallback = absl::AnyInvocable<grpc::Status(const R*)>` and `OnDoneCallback = absl::AnyInvocable<void() &&>`. **First choice for client streaming (`stream Req`):** returning a non-OK status finishes the stream with it. Deletes itself in `OnDone()`. |
| `WithSimpleQueueWriter<T, max_q, recycle>` / `SimpleServerWriter<W, max_q, recycle>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | Queued writer mixin: public `Write()`, `AcquireMessage()`, `QueueSize()`, `IsFinished()`, `IsCancelled()`, idempotent `Finish(Args&&...)`; protected `SetCancelled()` / `SetFinished()`. It overrides only `OnWriteDone`, so subclasses add `OnDone() { delete this; }` and `OnCancel() { if (this->SetCancelled()) this->Finish(grpc::Status::CANCELLED); }`. |
| `WithSimpleReader<T>` / `SimpleServerReader<R>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | Implement `bool Read(const R*)`; returning `false` only stops reading, so a server must then call `Finish()` itself. Servers call `StartRead()` in the constructor and `Finish(OK)` on half-close, so never call `StartRead()` yourself; clients call `StartRead()` and then `StartCall()`. The subclass supplies `OnDone()`. |
| `SimpleServerBidiStream<R, W, max_q, recycle>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | Writer and reader mixins over one `reactor_lock_`; no `OnCancel()` / `OnDone()` handling. Known issue (b/576339598): client half-close calls `Finish(OK)` behind the writer's back; see [emulator/libs/grpc_utils/AGENTS.md](../../libs/grpc_utils/AGENTS.md). |
| `ErrorServerWriter<W>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | `explicit ErrorServerWriter(grpc::Status status)`: finishes a server stream immediately with a non-OK status (e.g. `FAILED_PRECONDITION`) and deletes itself in `OnDone()`. |
| `SimpleClientLambdaReader<R>`, `SimpleClientWriter<W>`, `SimpleClientBidiStream<R, W>` | `//emulator/libs/grpc_utils:async` | `simple_async_grpc.h` | `SimpleClientLambdaReader(std::shared_ptr<grpc::ClientContext>, ReadCallback, OnDoneCallback = nullptr)` and `SimpleClientWriter(std::shared_ptr<grpc::ClientContext>)` own the context. `SimpleClientBidiStream<R, W>` is a bare alias over `ClientBidiReactor<W, R>` (note the order). |
| `AbslStatusToGrpcStatus` / `GrpcStatusToAbslStatus` | `//emulator/libs/grpc_utils:absl_translate` | `absl_status_translate.h` | Status translation. |
| `Library<T>` | `//emulator/libs/grpc_utils:async` | `library.h` | Thread-safe borrowed-object registry (`Acquire()`, `ForEach()`, `WaitUntilLibraryIsClear(timeout)`) for tracking live streams across shutdown. |
| `EnumTranslate::translate` | `//emulator/libs/grpc_utils:async` | `enum_translate.h` | `constexpr` lookup between proto enums and C++ enums. |

With `max_q > 0`, a full backlog replaces the newest pending message (latest wins), so `QueueSize()` can reach `max_q + 1` (one in flight plus `max_q` pending).

---

## 2. Threading & Lock-Ordering Rules for Service Code

1. **Listeners run synchronously on the firing thread.** `CallbackEventSource::FireEvent()` calls listeners on the caller's thread without holding a source lock; `ObservableValue::SetValue()` holds its non-recursive `std::mutex` across all listeners; `RemoveCallback()` blocks until in-flight invocations return. Every writer's listener ends in `Write()` (which takes `reactor_lock_`); `StateStreamWriter` runs `populate_fn` first.
   - **Release service locks before firing.** `SystemServiceImpl::OnGuestClipboardUpdated` ([system_service_impl.cc](services/v2/system/system_service_impl.cc)) unlocks `clipboard_mutex_` before `clipboard_events_.FireEvent(clip)`, because the writer's `populate_fn` (`PopulateClipboardState()`) takes `clipboard_mutex_` again.
   - **Inside an `ObservableValue` callback** (e.g. on `ClipboardChannel::guest_to_host`), and in anything it triggers synchronously, never call `GetValue()` / `SetValue()` on the same value; use the callback argument.
2. **Never call `Finish()` from a writer's own event path** (`EventArrived`, `populate_fn`, `filter_fn`): `Finish()` → `Unsubscribe()` → `RemoveCallback()` waits for in-flight callbacks, including the current one, and deadlocks. Post the `Finish()` to another thread or loop.
3. **`populate_fn` runs on several threads:** synchronously on the gRPC thread inside the writer's constructor (initial snapshot), then on whichever thread fires the event (`qemu_loop`, `client_loop`, the WebRTC signaling thread, other gRPC threads). Guard the service state it reads with a mutex. With `recycle_size > 0` it receives a recycled message with stale fields: overwrite every scalar and clear stale sub-fields.
4. **Custom reactors:** never hold a member mutex across `Base::Finish()` (it may run `OnDone()` → `delete this` synchronously), `delete this` only in `OnDone()`, and in a `BaseEventStreamWriter` subclass call `this->Subscribe()` at the end of the constructor and `this->Unsubscribe()` in the destructor. Details: [emulator/libs/grpc_utils/AGENTS.md](../../libs/grpc_utils/AGENTS.md).
5. **VM events:** `RunStateEvents()` / `SnapshotEvents()` callbacks have their own rules; see [emulator/plugin/vminterface/AGENTS.md](../vminterface/AGENTS.md) §2.
6. **Teardown:** the service destructor removes every listener it registered on long-lived `avd_universe` channels (e.g. `RemoveCallback(guest_clipboard_callback_id_)`).

---

## 3. Service Wiring & Long-Running Operations

- **V2 services** are defined in [services/v2/BUILD.bazel](services/v2/BUILD.bazel) (`//emulator/plugin/grpc/services/v2:capabilities_service`, `//emulator/plugin/grpc/services/v2:sensors_service`, `//emulator/plugin/grpc/services/v2:system_service`, `//emulator/plugin/grpc/services/v2:operations_service`, `//emulator/plugin/grpc/services/v2:hypervisor_service`) and created by `CreateV2Services` ([v2_services.cc](services/v2/v2_services.cc), `//emulator/plugin/grpc/services/v2:v2_services`); Operations and Hypervisor are created only when `vm_operations != nullptr`. `CreateServices` ([grpc.cc](grpc.cc)) concatenates the V1, WebRTC, and V2 lists, and `grpc_realize` registers them with the server builder.
- **RTC** (`//emulator/plugin/grpc/services/v2:rtc_service`) is not created by `CreateV2Services`: `CreateInProcessRtcServices()` (`//emulator/plugin/grpc/services:in_process_rtc_service`) builds it in the `webrtc` QEMU device, and `WebrtcGetServices()` returns it.
- **AIP-151 operations:** snapshot create/restore (`HypervisorServiceImpl::RunSnapshotRpc` in [hypervisor_service_impl.cc](services/v2/hypervisor/hypervisor_service_impl.cc)) register an operation with `OperationsServiceImpl::StartOperation(prefix, metadata)` and finish it with `CompleteOperation()` / `FailOperation()`. The RPC still blocks until the snapshot finishes; the operation lets other clients poll `GetOperation` / `WaitOperation` (live progress via `SetMetadataRefresher`). After `StartOperation`, report errors through `FailOperation` and return `Status::OK`, because a non-OK status discards the operation. `DeleteSnapshot` is a plain unary RPC.
- **Exemplars:** state streaming with mutex discipline in [system_service_impl.h](services/v2/system/system_service_impl.h), [system_service_impl.cc](services/v2/system/system_service_impl.cc), and [system_service_impl_test.cc](services/v2/system/system_service_impl_test.cc); client streaming with `SimpleServerLambdaReader`, `ErrorServerWriter`, and a filtered `StateStreamWriter` in [sensors_service_impl.cc](services/v2/environment/sensors_service_impl.cc).

---

## 4. Testing

### Tier A: deterministic reactor tests
Expose a templated factory whose `Reactor` defaults to the real gRPC reactor (from `SystemServiceImpl`):
```cpp
template <typename Reactor = ::grpc::ServerWriteReactor<SystemState>>
using SystemStateWriter = ::android::emulation::control::StateStreamWriter<
        SystemState, GrpcNotification, GrpcNotificationEventSource, 0, 0, Reactor>;

template <typename Reactor = ::grpc::ServerWriteReactor<SystemState>>
auto* CreateSystemStateWriter() {
    DCHECK(notification_source_);
    return new SystemStateWriter<Reactor>(
            notification_source_, [this](SystemState* state) { PopulateSystemState(state); },
            [](const GrpcNotification& notif) { return notif.has_booted(); });
}
```
In the test, use `goldfish::grpc::v2::test::FakeWriteReactor` from [test/grpc_v2_service_test.h](services/v2/test/grpc_v2_service_test.h) (fields: `start_write_called`, `finish_called`, `finish_status`, `last_write_msg`, `written_messages`). The header has no `cc_library`: list it in the test's `srcs` with `includes = ["."]` (see `:system_service_test` in `services/v2/BUILD.bazel`). The fake never calls `OnWriteDone()` / `OnDone()` itself:
```cpp
auto* writer = service_->CreateSystemStateWriter<test::FakeWriteReactor<SystemState>>();
ASSERT_EQ(writer->written_messages.size(), 1);  // Initial snapshot from the constructor.
writer->OnWriteDone(true);                      // Only one write is in flight; release it.
absl::Time start = absl::UnixEpoch();
// Fires because the fixture calls guest_status_.SetGrpcNotificationChannel(&notification_source_).
guest_status_.SetBootComplete(start + absl::Seconds(12));
ASSERT_EQ(writer->written_messages.size(), 2);
writer->OnDone();                               // Unsubscribes and deletes; don't touch writer afterwards.
```

### Tier B: concurrency tests (streaming or stateful code)
Model on [simple_async_grpc_test.cc](../../libs/grpc_utils/simple_async_grpc_test.cc): four or more producer threads behind a `std::atomic<bool>` start barrier call `FireEvent()` / `Write()` while another thread calls `OnCancel()` or `Finish()`. Its `FakeWriteReactor` (adds `start_write_called_count`, `start_write_after_finish`, `finish_called_count`; `OnWriteDone` is pure virtual there), the read-side `FakeReactor`, and `SelfDeletingFakeReactor` are file-local (namespace `android::emulation::control`): copy them, or add those fields to `test/grpc_v2_service_test.h`. Assert reactor state *before* `OnDone()`; afterwards only `EXPECT_EQ(source.CallbackCount(), 0)` is safe.

```bash
bazel test --test_output=errors @goldfish//emulator/plugin/grpc/...
bazel test --config=tsan --test_output=errors @goldfish//emulator/plugin/grpc/...  # also --config=asan
```

### `BUILD.bazel`
- New `cc_library` / `cc_test` targets in `services/v2/BUILD.bazel` set `local_defines = select({"@platforms//os:windows": ["NOGDI"], "//conditions:default": []})` (prevents `<wingdi.h>` macro clashes); WebRTC-linked targets use the `-fno-rtti` / `/GR-` copts instead.
- Each package has `clang_tidy_test(name = "tidy", targets = [...], tidy_config_file = "//:clang_tidy_config")`.
