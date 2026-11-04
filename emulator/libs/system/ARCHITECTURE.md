# Component: Android System

**Role:** Abstraction layer for Operating System services and specialized monotonic/virtual clocks.
**Location:** `hardware/generic/goldfish/emulator/libs/system`
**Namespaces:** `android::base`, `goldfish::system`

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `System` | `:system` | `include/android/base/system.h` | Abstract interface for Env vars, Time, Paths, OS Info. |
| `SteadyClock` | `:steady_clock` | `include/goldfish/system/steady_clock.h` | Pure interface and `GetRealClock()` accessor for host monotonic elapsed duration (`std::chrono::steady_clock`). |
| `VirtualClock` | `:virtual_clock` | `include/goldfish/system/virtual_clock.h` | Pure interface for guest virtual uptime (`absl::Duration`). |
| `FakeSteadyClock`, `FakeVirtualClock` | `:test-headers` | `test-headers/goldfish/system/testing/fake_clocks.h` | Thread-safe fake clocks for deterministic unit tests. |
| `FdUtil` | `:fd_util` | `include/android/base/fd_util.h` | File descriptor utilities and non-blocking helpers. |

## Critical Infrastructure
* **System Singleton:** `System::Get()` provides access to the global OS interface. This can be swapped for a mock (`TestSystem`) during unit tests to simulate environment variables, time, or file system states.
* **Storage Capacity:** `StorageCapacity` class provides type-safe handling of byte sizes (MiB, GiB) with user-defined literals (e.g., `512_MiB`).
* **Platform Independence:** Hides differences between Windows (`_wgetenv`, `Sleep`) and POSIX (`getenv`, `usleep`).

## Dependencies
* **Core:** `@abseil-cpp//absl/log`, `@abseil-cpp//absl/time`, `@abseil-cpp//absl/status:statusor`.
* **Libraries:** `//emulator/libs/file`, `//emulator/libs/process`.

## Threading Model
* **Thread Safe:** `System` methods are generally thread-safe.
