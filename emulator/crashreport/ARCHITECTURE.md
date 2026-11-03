# Component: Crash Reporting & Hang Detection

**Role:** Manages out-of-process crash reporting (via Crashpad) and monitors for emulator hangs.
**Location:** `hardware/generic/goldfish/emulator/crashreport`
**Namespace:** `android::crashreport`
**Agent Playbook:** [`AGENTS.md`](AGENTS.md)

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `crashhandler_init` | `:crash_system` | `include/android/crashreport/crash_system.h` | C-entry point to initialize the crash reporting system at startup. |
| `CrashReporter` | `:crash_reporter` | `include/android/crashreport/crash_reporter.h` | Singleton for adding or updating annotations (`SetAnnotation`, `AddMessage`) or triggering a crash (`Die`). |
| `BreadcrumbTracker` | `:breadcrumb` | `include/android/crashreport/breadcrumb.h` | Tracker for leaving "breadcrumbs" in circular buffers within crash reports. |
| `HangDetector` | `:hang_detector` | `include/android/crashreport/hang_detector.h` | Service to monitor event loops for hangs. |
| `upload_crashes` | `:crash_system` | `include/android/crashreport/crash_system.h` | Starts the background upload of pending reports. |

## Critical Infrastructure
* **Crashpad Integration:** Starts an out-of-process `crashpad_handler` to monitor the emulator. If the emulator crashes, the handler saves a minidump to a local database.
* **Annotations:** `CrashReporter::SetAnnotation` allows attaching and updating key-value pairs (e.g., version, GPU driver, lifecycle phase) in the crash dump.
* **Hang Detection:**
    *   **Looper Watcher:** Registers `goldfish::async::EventLoop` instances. It posts heartbeat tasks to the loop; if they don't complete within a timeout, it triggers a "hang" crash.
    *   **Predicates:** Allows registering custom lambda functions that return true if a specific subsystem is stuck.
* **Consent:** Logic to handle user consent before uploading dumps to Google's crash servers.

## Dependencies
* **Upstream:** `@crashpad` (Core engine), `@abseil-cpp`.
* **Internal:** `//emulator/libs/async` (For loop monitoring), `//emulator/libs/system`.

## Threading Model
* **CrashSystem:** Initialization happens on the main thread. Uploads are spawned in detached threads.
* **HangDetector:** Runs a dedicated worker thread that periodically polls watched loopers and evaluates predicates.
* **CrashReporter:** Annotations are thread-safe (guarded by Crashpad's internal mechanics).

## Flows & Guides
* [Hang Detection Lifecycle](docs/hang_detection_flow.md)
