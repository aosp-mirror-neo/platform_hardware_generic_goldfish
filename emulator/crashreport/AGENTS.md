# Crash Reporting & Hang Detection (`//emulator/crashreport/...`) — Agent Playbook

**Namespace:** `android::crashreport`

---

## 1. Key Targets & Files

| Component | Bazel Target | Files | Responsibility |
| :--- | :--- | :--- | :--- |
| `CrashSystem` | `//emulator/crashreport:crash_system` | [crash_system.h](include/android/crashreport/crash_system.h), [crash_system.cc](crash_system.cc) | `CrashSystem::get().initialize()` starts the out-of-process `crashpad_handler` (path from `AEMU_CRASHPAD_HANDLER`). Reporting is on by default and `ANDROID_EMU_ENABLE_CRASH_REPORTING=0` disables it (the header comment saying unset means disabled is stale). The minidump database is `ANDROID_EMU_CRASH_REPORTING_DATABASE` (`/tmp/crash-report.db` under `bazel run //emulator/launcher` and the crashreport tools). `initialize()` first creates the database directory and any missing parents (`ensureDatabaseDirectory()`; on Windows `/tmp/crash-report.db` means `\tmp` on the current drive), and returns an `absl::Status` for the caller to log, because Crashpad's own mini_chromium logging is dropped on Windows. `uploadEntries(Consent)` uploads only with `Consent::ALWAYS`; `ASK` and `NEVER` delete every stored report (completed and pending). The launcher calls it on every start, so without upload consent a relaunch wipes the database: inspect minidumps first. |
| `CrashReporter` | `//emulator/crashreport:crash_reporter` | [crash_reporter.h](include/android/crashreport/crash_reporter.h), [crash_reporter.cc](crash_reporter.cc) | `CrashReporter::Get()`: thread-safe `SetAnnotation(std::string_view name, std::string_view value)` (≤ 16384 bytes; updates in place or clears retired buckets without freeing registered `Annotation` nodes), `GetAttachedData(std::string_view)`, `AddMessage(std::string_view)` (8 KB `internal-msg` annotation), `Die(std::string_view)` (forces a dump). `CrashReporter::GetCrashingHangDetector()` returns the process-wide `HangDetector`, whose callback calls `Die()`. |
| `HangDetector` | `//emulator/crashreport:hang_detector` | [hang_detector.h](include/android/crashreport/hang_detector.h), [hang_detector.cc](hang_detector.cc) | `AddWatchedLooper(std::string loop_name, ::goldfish::async::EventLoop& event_loop, absl::Duration task_timeout, ActivePredicate is_active)` / `RemoveWatchedLooper(::goldfish::async::EventLoop& event_loop)`. Posts heartbeats to each watched loop and fires after two missed checks. The target is package-private: from other packages depend on `//emulator/crashreport:crash_reporter` and use `GetCrashingHangDetector()`. |
| Breadcrumbs & annotations | `//emulator/crashreport:breadcrumb`, `//emulator/crashreport:breadcrumb_proto`, `//emulator/crashreport:binary_annotation`, `//emulator/crashreport:looper_registrations`, `//emulator/crashreport:thread` | [breadcrumb.h](include/android/crashreport/breadcrumb.h), [breadcrumb_proto.h](include/android/crashreport/breadcrumb_proto.h) | Ring-buffer breadcrumbs (`CRUMB(x)`, `TCRUMB()`; the oldest data is overwritten) and `BinaryAnnotation<MaxSize>`, a raw inline buffer (`Data()`) that is always captured in full; the proto breadcrumb logs write a `RawCircularLog` into a 16 KB one. `UpdatableStringAnnotation` (`:simple_string_annotation`) is private to `//emulator/crashreport:crash_reporter`. |
| `crashreport` CLI | `//emulator/crashreport:crashreport`, `//emulator/crashreport/tool:crashreport`, `//emulator/crashreport/tool:bazel_crashreport` | [crashreport.cc](tool/crashreport.cc), [bazel_crashreport.py](tool/bazel_crashreport.py) | Inspects local Crashpad databases, extracts breadcrumbs and annotations, and symbolizes stack traces. |

---

## 2. Lifetime & Concurrency Rules

1. **Unregister a watched loop before it stops.** Remove every loop added with `GetCrashingHangDetector().AddWatchedLooper(...)` via `RemoveWatchedLooper(loop)` before the loop stops or is destroyed. The detector keeps an `EventLoop&` and a timer on it, so a destroyed loop is a use-after-free, and a stopped loop never answers its heartbeat, so the detector calls `Die()`. `AvdExtendedUniverse` adds the `QemuEventLoop` and each `goldfish::avd::VCpuEventLoop` (watched as `"QemuCpuLoop:<i>"`) in `Initialize()` and removes them in `~AvdExtendedUniverse()` before `WaitUntilEventLoopsIdle()` / `ShutdownQemuLoop()` ([avd_extended_universe.cc](../plugin/avd/avd_extended_universe.cc)).
2. **`is_active` predicates** (`HangDetector::ActivePredicate`, a `std::function<bool()>`; null means always active) run on the detector's worker thread and once on the caller's thread inside `AddWatchedLooper()`, always under the detector's mutexes. They must not take the BQL, block on the watched loop, or call back into the `HangDetector`; `VmOperations::IsRunning()` is safe.
3. **Windows include order:** Crashpad's compat `getopt.h` / `sys/types.h` shadow AEMU's. `:crash_reporter` and `:crash_system` isolate `@crashpad//*` in `implementation_deps`, so downstream consumers do not need workarounds; targets that depend directly on `:breadcrumb`, `:binary_annotation`, or `@crashpad//*` headers must put `select({"@platforms//os:windows": ["@aemu//windows:compat-hdrs"], "//conditions:default": []})` before those entries in `deps`, marked `# buildifier: leave-alone` (see `//emulator/libs/async:async_api` in `emulator/libs/async/BUILD.bazel`).

---

## 3. Testing

- **Hang detection:** use `ThreadedEventLoop::Create(LibuvEventLoop::Create())` and a private detector from `HangDetector::Create(callback, HangDetector::Timing{...}, simulated_clock)`, advancing time with `absl::SimulatedClock::AdvanceTime()` (see [hang_detector_test.cc](hang_detector_test.cc)). Never use `CrashReporter::GetCrashingHangDetector()` in tests: its callback calls `Die()`.
- **Minidumps:** [crash_dump_test.cc](crash_dump_test.cc) (`//emulator/crashreport:crash_dump_test`) runs `:crash-me` with `AEMU_CRASHPAD_HANDLER` set and `ANDROID_EMU_CRASH_REPORTING_DATABASE` pointing at a temporary directory, and asserts that a new report appears (Bazel only; skipped on Windows). Database directory preparation, breadcrumb, and annotation coverage live in `:crash_system_test`, `:breadcrumb_proto_test`, `:binary_annotation_test`, `:annotation_buffer_test`, and `:crash_reporter_test`.

```bash
bazel test --test_output=errors @goldfish//emulator/crashreport:all
```
