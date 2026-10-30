# `//emulator/libs/grpc_utils` — Agent Playbook

**Namespaces:** `android::emulation::control` for `grpc_event_stream_support.h`, `library.h`, and `absl_status_translate.h`; the global namespace for `simple_async_grpc.h` and `enum_translate.h`.

Using these reactors in a service (catalog, lock ordering, test patterns): [emulator/plugin/grpc/AGENTS.md](../../plugin/grpc/AGENTS.md). This file covers changes to the library itself.

## Rules When Modifying the Library

1. **One `reactor_lock_` per reactor.** `WithSimpleReader` and `WithSimpleQueueWriter` virtually inherit `WithReactorLock`, so bidirectional streams share one non-reentrant `absl::Mutex`. It is held around `T::StartRead` / `T::StartWrite` and guards `status_`, and it is never held across `T::Finish`, which may run `OnDone()` → `delete this` synchronously (see `SimpleServerLambdaReaderSelfDeletingUafSafety` in [simple_async_grpc_test.cc](simple_async_grpc_test.cc)). A base reactor or fake must therefore never call `OnWriteDone` / `OnReadDone` synchronously from `StartWrite` / `StartRead`.
2. **`Unsubscribe()` calls `RemoveCallback()` outside the lock.** [`BaseEventStreamWriter::Unsubscribe()`](include/android/emulation/control/grpc_event_stream_support.h) swaps `callback_id_` to `kInvalidCallbackId` under `reactor_lock_` and calls `listener_->RemoveCallback()` after releasing it, which avoids an ABBA deadlock with a concurrent `FireEvent()`.
3. **Two-phase subscription.** `BaseEventStreamWriter` never subscribes in its own constructor, which would expose `this` to event threads before the derived vtable and fields exist. All three concrete writers call `this->Subscribe()` at the end of their constructors; `StateStreamWriter` (and its `UniqueStateStreamWriter` alias with `deduplicate = true`) and `UniqueEventStreamWriter` also call `this->Unsubscribe()` in their destructors, while `GenericEventStreamWriter` relies on `~BaseEventStreamWriter()` for that. When `deduplicate = true`, `StateStreamWriter` runs `populate_fn_` outside locks and holds `dedup_mutex` across `MessageDifferencer::Equals` and `Write()` (`dedup_mutex -> reactor_lock_`) so `last_state` always matches the most recently enqueued message.
4. **Known issue (b/576339598): bidi half-close.** In `SimpleServerBidiStream`, a client half-close makes `WithSimpleReader::OnReadDone(false)` call `T::Finish(OK)` directly, bypassing `WithSimpleQueueWriter::SetFinished()`. `IsFinished()` stays false, so a later `Write()` / `Finish()` can start a write or a second `Finish` after `Finish`. Route reader-side completion through the writer's `Finish()` if you touch this composition.
5. **Verify every change** to `simple_async_grpc.h`, `grpc_event_stream_support.h`, or `library.h` with tests in [simple_async_grpc_test.cc](simple_async_grpc_test.cc) / [async_test.cc](async_test.cc). `RealGrpcServerTest` (real-server cancel/finish races and the reader use-after-free tests) is skipped by plain `bazel test`; it runs under `--config=tsan` / `--config=asan` on Linux, or on any platform with `--test_env=RUN_REAL_GRPC_SERVER_TESTS=1`:
   ```bash
   bazel test --test_output=errors @goldfish//emulator/libs/grpc_utils:all
   bazel test --config=tsan --test_output=errors @goldfish//emulator/libs/grpc_utils:async_test
   bazel test --config=asan --test_output=errors @goldfish//emulator/libs/grpc_utils:async_test
   ```
