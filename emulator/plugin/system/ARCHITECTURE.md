# Component: System Plugin

**Role:** Provides system-level implementations backed by QEMU internals.
**Location:** `hardware/generic/goldfish/emulator/plugin/system`
**Namespace:** `goldfish::system`

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `GetQemuVirtualClock()` | `:qemu_virtual_clock` | `include/goldfish/system/qemu_virtual_clock.h` | Stateless `VirtualClock` implementation backed by `QEMU_CLOCK_VIRTUAL`. |

## Critical Infrastructure
* **GetQemuVirtualClock():** Reads `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` as an `absl::Duration` of guest virtual uptime, advancing only while guest vCPUs run and preserving continuity across VM pauses and snapshots.

## Dependencies
* **Upstream:** `@qemu` (`qemu/timer.h`), `@abseil-cpp//absl/base:no_destructor`, `@abseil-cpp//absl/time`.
* **Internal:** `//emulator/libs/system:virtual_clock` (Interface definition).

## Threading Model
* **Thread Safe:** `GetQemuVirtualClock().Now()` is stateless and delegates to `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`, which is thread-safe in QEMU.
