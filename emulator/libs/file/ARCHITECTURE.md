# Component: File System Primitives

**Role:** Cross-platform filesystem operations, atomic file writes, cross-process file locking, path manipulation, and disk capacity units.
**Location:** `hardware/generic/goldfish/emulator/libs/file`
**Namespace:** `android::base::file`

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `File` | `:file` | `include/goldfish/file/file.h` | Helpers for file and path operations (`mkdir`, `rm`, `exists`, `read_symlink`, `touch`, `free_space`, `kDiskPressureLimit`). |
| `AtomicFile` | `:file` | `include/goldfish/file/file_atomic.h` | Atomic write replacement using temporary files. |
| `StorageCapacity` | `:file` | `include/goldfish/file/storage_capacity.h` | Type-safe handling of byte sizes (MiB, GiB, TiB) with literals (e.g. `512_MiB`). |
| `ScopedFileLock` | `:file_lock` | `include/goldfish/file/file_lock.h` | Exclusive, PID-advertising cross-process lock (`<target>.lock` / `<target>.lock\pid`) compatible with Android Studio and legacy QEMU2, with stale-PID recovery. |
| `ScopedSharedFileLock` | `:shared_file_lock` | `include/goldfish/file/shared_file_lock.h` | Cross-process 1-Writer XOR N-Readers lock (`flock` / Win32 share modes, non-inheritable handles) compatible with `multiinstance.lock`. |

## Critical Infrastructure
* **Cross-Platform Path Handling:** Normalizes directory separators, path concatenation, and Windows UNC prefixes.
* **Atomic File Writes:** Guarantees that written files are flushed to disk before atomically replacing target files.
* **Non-Inheritable Cross-Process Locks:** `ScopedFileLock` and `ScopedSharedFileLock` open all descriptors/handles with `O_CLOEXEC` / `bInheritHandle = FALSE` so long-lived daemons (e.g. `netsimd`) never inherit active AVD locks.
* **Storage Capacity Types:** Strongly-typed representation of byte capacities with arithmetic and string formatting.

## Dependencies
* **Base:** `//emulator/libs/base`, `//emulator/libs/status_macros`, `//emulator/libs/system:eintr_wrapper`.
* **Abseil:** `@abseil-cpp//absl/status:statusor`, `@abseil-cpp//absl/strings`, `@abseil-cpp//absl/log`, `@abseil-cpp//absl/synchronization`, `@abseil-cpp//absl/time`, `@abseil-cpp//absl/time:clock_interface`.

## Threading Model
* **Thread Safe:** File helper functions and cross-process lock acquisitions are thread-safe. Move-only `ScopedFileLock` and `ScopedSharedFileLock` instances follow standard single-owner RAII semantics.
