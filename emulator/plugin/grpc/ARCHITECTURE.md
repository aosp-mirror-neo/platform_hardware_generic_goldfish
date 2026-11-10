# Component: gRPC Plugin

**Role:** Exposes the Emulator Controller gRPC service (`EmulatorControllerService`) as a QEMU
device. **Location:** `hardware/generic/goldfish/emulator/plugin/grpc` **Namespace:**
`goldfish::grpc` (C++), `TYPE_GRPC` (QOM)

> [!IMPORTANT]
> **Agent Playbook:** Before implementing or reviewing gRPC services or streaming reactors, read [`AGENTS.md`](AGENTS.md) for the reactor-first mandate (`//emulator/libs/grpc_utils`), lock-ordering invariants, `populate_fn` buffer-recycling rules, and multithreaded concurrency test requirements.

## Integration Guide

| Class / Interface     | Bazel Target | Header Path  | Description             |
| :-------------------- | :----------- | :----------- | :---------------------- |
| `grpc_register_types` | `:grpc`      | `.../grpc.h` | QEMU type registration. |

## Critical Infrastructure

- **Service Hosting:** Launches the `EmulatorControllerService` and WebRTC `RtcService` (`v2::Rtc`)
  which provide remote control over the emulator (VM ops, screenshots, input injection, and
  zero-copy WebRTC UI streaming).
- **In-Process WebRTC Videobridge:** Hosts WebRTC signaling directly inside the gRPC server using
  `MediaTrackProvider` (`InProcessVideoSource` / `InProcessAudioSource`) and `InProcessInputSender`
  (direct event dispatching), eliminating IPC overhead and external process forwarding.
- **Security:**
  - **TLS:** Configurable via QOM properties (`tls_cer`, `tls_key`, `tls_ca`).
  - **Auth:** Supports token-based authentication (`-grpc-use-token`) and JWTs.
- **Advertisement:** Writes connection details (port, token, certificate paths) to a discovery file
  (INI format) in `discovery_dir`, allowing tools like Android Studio to find the running emulator
  instance.

## Dependencies

- **Core:** `//emulator/plugin/avd` (AVD Info), `//emulator/plugin/display`.
- **Service:** `//emulator/plugin/grpc/services:emulator_controller`,
  `//emulator/plugin/grpc/services:screen_recorder`.
- **Videobridge:** `//emulator/videobridge:rtc_service`,
  `//emulator/videobridge:in_process_media_providers`, `//emulator/videobridge:videobridge_core`.
- **Forwarding:** `//emulator/plugin/grpc/services:service_forwarder`,
  `//emulator/plugin/grpc/services:ui_controller_forwarder`.

## Threading Model

- **gRPC Thread Pool:** The gRPC server runs on its own thread pool.
- **Main Loop Interaction:** Interactions with QEMU core (VM operations, display) are marshalled to
  the main loop via `QemuEventLoop` or `GlobalEventLoop`.

## Lifecycle

- **Shutdown request:** Removes the discovery file so that no new clients find the emulator.
  Connected clients are still served while the exit snapshot (`-savevm`) is saved.
- **Main loop exit:** After the exit snapshot, a `qemu_register_main_loop_exit_notifier` callback
  calls `AvdUniverse::OnPreGrpcShutdown()`, which stops the QEMU event loop and the input
  dispatcher so that RPCs waiting on them do not block the server shutdown, then releases the BQL,
  flushes the global event loop (`WaitUntilIdle()`) so terminal exit-snapshot and run-state events
  reach active streams and LROs, stops the server (100 ms grace), and destroys the services. Some,
  like the WebRTC switchboard, outlive their RPC streams and release QEMU resources such as audio
  capture, so this must happen before `qemu_cleanup()` tears down those subsystems.
