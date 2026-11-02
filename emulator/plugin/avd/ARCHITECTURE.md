# Component: AVD Info Plugin

**Role:** The "Bootstrap" QEMU device (`avdstart`). It initializes the Goldfish emulator
environment, configures hardware, and starts all HAL services. **Location:**
`hardware/generic/goldfish/emulator/plugin/avd` **Namespace:** `goldfish::avd` (C++),
`TYPE_AVD` (QOM)

## Integration Guide

| Class / Interface          | Bazel Target             | Header Path                 | Description                                                      |
| :------------------------- | :----------------------- | :-------------------------- | :--------------------------------------------------------------- |
| `AvdProperties`            | `:avd_properties`        | `.../avd/avd_properties.h`  | Configuration and launch properties for the AVD.                 |
| `AvdUniverse`              | `:avd_universe`          | `.../avd/avd_universe.h`    | Container for global AVD state (Properties, Sensors, Clipboard). |
| `GetAvd()`                 | `:avd_info`              | `.../avd/avd_info.h`        | Singleton accessor for the `AvdUniverse`.                        |
| `AvdExtendedUniverse`      | `:avd_extended_universe` | `avd_extended_universe.h`   | Concrete `AvdUniverse` implementation owning HALs, loops and the `VmOperations`; constructs these services and injects them into consumers. |
| `avd_info_register_types`  | `:avd_start`             | `.../avd/avd_start.h`       | QOM type registration for the `avdstart` bootstrap device.       |
| `globalEventLoop()`        | `:global_event_loop`     | `.../avd/global_event_loop.h` | Accessor for the global client `EventLoop`.                    |

## Critical Infrastructure

- **Lifecycle Manager:**
  - **Realize:** Parses `hardware-qemu.ini`, initializes the `AvdUniverse`, creates the
    `QemuEventLoop`, and registers all HALs (`sensors`, `clipboard`, `camera`, `gps`, etc.) with the
    connector registry.
  - **Unrealize:** Shuts down the event loop and cleans up resources.
- **Property Parsing:** Exposes QOM properties (e.g., `avd_name`, `adb_port`) that are set by the
  emulator launcher command line.
- **Global State:** Holds the canonical "Truth" for the device configuration (`HardwareConfig`) and
  mutable state (`ObservableValue`s in `AvdUniverse`).

## Dependencies

- **Core:** `//emulator/plugin/avd_universe` (State definitions).
- **HALs:** Depends on _every_ HAL library to register them.
- **Config:** `//emulator/libs/hardware_config:hardware_config`.
- **Async:** `//emulator/libs/async:qemu_event_loop`.

## Threading Model

- **Main Thread:** `realize` runs on the QEMU main thread. It establishes the `QemuEventLoop` which
  wraps the QEMU AioContext, allowing thread-safe interaction from other threads via `Post`.
- **VmOperations:** created during `realize` with the global event loop (which runs its run-state
  callbacks) and `QemuEventLoop` (which executes its VM and snapshot operations via `PostAndWait`).
  Teardown order: the main-loop exit hook destroys the gRPC server and its services (the run-state
  subscribers) before `qemu_cleanup()` destroys the universe and its `VmOperations`, and
  `GF_SHUTDOWN_FUNC` shuts down the global event loop after `qemu_cleanup()`.
- **Input dispatcher:** `GetInputDispatcher()` returns the dispatcher built in `realize`, before
  QEMU has created any console. `avdend` (`UniverseBuildComplete()`) gives it the keyboard on
  console 0, after the display devices exist. The main-loop exit hook calls
  `OnPreGrpcShutdown()`, which shuts down the `QemuEventLoop` and then the dispatcher, before it
  destroys the gRPC server. It is shared: WebRTC channels, which a static vector keeps alive past
  `qemu_cleanup()`, hold it, so the universe's destructor shuts it down again before destroying the
  loop, touch devices and keyboard it uses.
