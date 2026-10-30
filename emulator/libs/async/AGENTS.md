# Core Async & Eventing (`//emulator/libs/async/...` & `//emulator/libs/eventing/...`) — Agent Playbook

**Namespaces:** `goldfish::async` (test doubles: `goldfish::async::testing`); `goldfish::eventing` (`ObservableValue` and its trigger policies only); `android::base::eventing` (`MakeScopedCallback`, `ScopedEventCallback`, `CallbackEventSource`, `WithCallbacks`).

---

## 1. Key Targets & Headers

| Primitive | Bazel Target | `#include` | Notes |
| :--- | :--- | :--- | :--- |
| `EventLoop` | `//emulator/libs/async:async_api` | `"goldfish/async/event_loop.h"` | `Post`, `PostAndWait`, `ScheduleDelayed(Task, absl::Duration)` and `ScheduleRepeating(RepeatingTask, absl::Duration, absl::Duration)` (both return `EventLoop::ScopedTimer`; dropping or resetting it cancels the timer), `CreateTimer`, `bool IsOnLoopThread() const`, `size_t WaitUntilIdle()`. |
| `LibuvEventLoop`, `ThreadedEventLoop` | `//emulator/libs/async:libuv_event_loop`, `//emulator/libs/async:threaded_event_loop` | `"goldfish/async/libuv_event_loop.h"`, `"goldfish/async/threaded_event_loop.h"` | Production libuv loop and its background-thread wrapper, e.g. `ThreadedEventLoop::Create(LibuvEventLoop::Create())`. |
| `QemuEventLoop` | `//emulator/libs/async:qemu_event_loop` | `"goldfish/async/qemu_event_loop.h"` | Runs tasks on the QEMU main loop under the BQL (one bottom half). Its timers use `QEMU_CLOCK_VIRTUAL` and stall while the VM is paused; use a libuv loop for wall-clock timers. |
| `TestEventLoop`, `FakeAsyncSocket`, `MockAsyncSocketFactory` | `//emulator/libs/async:async_testing` | `"goldfish/async/testing/test_event_loop.h"`, `"goldfish/async/testing/fake_async_socket.h"` | Manually stepped test loop (see §3). `FakeAsyncSocket` is a gmock fake that captures callbacks: call `setEventLoop(loop)` before `SimulateRead` / `SimulateError` / `SimulateClose` / `SimulateConnected`. |
| `ScopedAsyncResource<T>`, `EventLoop::ScopedTimer` | `//emulator/libs/async:async_api` | `"goldfish/async/scoped_async_resource.h"`, `"goldfish/async/event_loop.h"` | `ScopedAsyncResource<T>` (e.g. `ScopedAsyncSocket`) runs `T::Close()` on the owning loop, with a blocking `PostAndWait` when destroyed elsewhere. `EventLoop::ScopedTimer` (`std::unique_ptr<Timer, TimerDeleter>`) cancels its timer on destruction; `CreateTimer()`, `ScheduleDelayed()`, and `ScheduleRepeating()` all return one. |
| `AsyncSocket`, `LibuvAsyncSocketFactory` | `//emulator/libs/async:async_api`, `//emulator/libs/async:libuv_sockets` | `"goldfish/async/async_socket.h"`, `"goldfish/async/libuv_socket_factory.h"` | Non-blocking TCP/UDS sockets with flow control. `LibuvAsyncSocketFactory` needs a libuv-backed loop (`LibuvEventLoop` or `ThreadedEventLoop`); `UvProcessLauncher(LibuvEventLoop&)` (`//emulator/libs/async:libuv_process_launcher`) needs a `LibuvEventLoop`. Never hand a `QemuEventLoop` to the libuv factory: create sockets on a libuv-backed loop and `Post` to the QEMU loop. In tests on `TestEventLoop`, use `MockAsyncSocketFactory` + `FakeAsyncSocket` (`testonly`). |
| `UvErrToAbslStatus`, `ErrnoToAbslStatus` | `//emulator/libs/async:uv_to_absl`, `//emulator/libs/async:errno_to_absl` | `"goldfish/async/uv_to_absl.h"`, `"goldfish/async/errno_to_absl.h"` | libuv and `errno` codes → canonical `absl::Status`. |
| `ObservableValue<T, TRIGGER>`, `MakeScopedCallback` | `//emulator/libs/eventing:eventing` | `"goldfish/eventing/observable_value.h"`, `"goldfish/eventing/with_callbacks.h"` | `TRIGGER` is `ObservableValueTriggerAlways` or `ObservableValueTriggerOnUpdate` (no default). `MakeScopedCallback(observable, fn)` returns `ObservableValue<T, TRIGGER>::ScopedCallbackHandle` (a `std::unique_ptr` to a `ScopedEventCallback`); store it as a member. |

---

## 2. Concurrency & Lifetime Rules

1. **Socket loop affinity:** `Send`, `Connect`, `Close`, `Connected`, and the `SetOn*Callback` setters must run on the socket's loop (`LibuvSocket` DCHECKs `event_loop_->IsOnLoopThread()`); only `OnFlowControlEvent()` and `GetLoop()` are thread-safe. Call `Close()` before destruction (the destructor DCHECKs `uv_is_closing`). To call a socket from another thread, `Post` to its loop; hold it in a `ScopedAsyncSocket` so `Close()` runs on the loop when it is destroyed.
2. **`PostAndWait` deadlocks:** calling it on the loop's own thread is `LOG(FATAL)`, and once the loop has finished it returns `FailedPrecondition`. Never call `loopA->PostAndWait()` from a task on `loopB` if `loopA` can wait on `loopB`; for example, a task on `goldfish::avd::globalEventLoop()` waiting on the QEMU loop deadlocks while the QEMU thread waits for the global loop to go idle at teardown. Dropping or cancelling a timer off its loop is also a blocking `PostAndWait` (`QemuEventLoop` temporarily releases the BQL for it), so the same rule applies to timer teardown.
3. **Return types:** `PostAndWait(f)` flattens results via `android::status::flatten_status_or_t` (`//emulator/libs/status_macros:flatten`): void or `absl::Status` → `absl::Status`; `T` or `absl::StatusOr<T>` → `absl::StatusOr<T>`. `Post(f)` does not flatten: it returns `absl::Status` for a void `f`, otherwise `absl::StatusOr<std::future<R>>`.
4. **`CallbackEventSource` / `ObservableValue` listeners run synchronously on the firing thread** (`goldfish::async::LoopBoundCallbackSource`, e.g. behind `VmOperations::RunStateEvents()`, delivers on its loop instead). `ObservableValue::SetValue()` and `OnPostLoad()` hold the value's non-recursive `std::mutex` (also taken by `GetValue()`) while firing; `CallbackEventSource::FireEvent()` holds no lock while dispatching. `RemoveCallback()`, and therefore destroying or `Reset()`ting a `ScopedEventCallback`, blocks until in-flight invocations finish. So:
   - a listener must never call `GetValue()` / `SetValue()` on the same `ObservableValue`, and must never destroy its own handle;
   - never take a lock inside a listener if any path holds that lock while calling `SetValue()` / `OnPostLoad()` or while destroying the handle (`MakeScopedCallback()` itself does not block);
   - for the `weak_ptr` capture pattern used by HAL devices, see [emulator/plugin/hal/AGENTS.md](../../plugin/hal/AGENTS.md) §3.

---

## 3. Testing with `TestEventLoop`

`TestEventLoop::Create()` returns a `std::unique_ptr<TestEventLoop>`. Tasks run on the loop's own internal thread, but only while the test calls `RunAll()` / `RunOne()` / `RunMany(n)` (immediately posted tasks) or `AdvanceClock(absl::Duration)` (timers that become due). `IsOnLoopThread()` is false on the test thread, so a `PostAndWait()` from the test body, including the one in `~ScopedAsyncResource`, blocks forever unless another thread pumps the loop.

```cpp
auto loop = goldfish::async::testing::TestEventLoop::Create();
auto timer = loop->ScheduleDelayed(fn, absl::Seconds(5));  // Keep the handle; dropping it cancels.
loop->AdvanceClock(absl::Seconds(5));                       // Runs due timers only.
loop->RunAll();                                             // Runs immediately posted tasks only.
```
- `Post(fn, delay)` never fires on `TestEventLoop` (b/576339538); use `ScheduleDelayed`.
- `eventing_unittests` does not cover `observable_value.h`; add a test when you change it.

```bash
bazel test --test_output=errors @goldfish//emulator/libs/async:all @goldfish//emulator/libs/eventing:all
bazel test --config=tsan --test_output=errors @goldfish//emulator/libs/async:eventloop_unittests @goldfish//emulator/libs/async:sockets_unittests @goldfish//emulator/libs/eventing:eventing_unittests
```
