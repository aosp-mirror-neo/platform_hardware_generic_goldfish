# Component: EmulatorController Method Implementations (`emulator_controller_methods`)

**Role:** Domain Method Handlers for `EmulatorController::Service`
**Location:** `emulator/plugin/grpc/services/emulator_controller_methods/`
**Build Targets:** `@goldfish//emulator/plugin/grpc/services/emulator_controller_methods:emulator_controller_methods`, `@goldfish//emulator/plugin/grpc/services/emulator_controller_methods:emulator_controller_methods_test`, `@goldfish//emulator/plugin/grpc/services/emulator_controller_methods:tidy`

## Overview
Implements the domain-specific sub-services composed by `EmulatorControllerImpl` (`@goldfish//emulator/plugin/grpc/services:emulator_controller`), including display screenshot streaming, audio capture, microphone state, battery, clipboard, fingerprint, GPS, sensors, status, and VM lifecycle control.

## Key Files & Classes (`namespace android::emulation::control`)
- **`display_service.h`**, **`display_service.cc`**, **`display_service_test.cc`**:
  - `DisplayServiceImpl` (`getScreenshot`, `streamScreenshot`, `getDisplayConfigurations`, `setDisplayConfigurations`, `setDisplayMode`, `setDisplayPose`) — Interfaces with `goldfish::display::IMultiDisplay` and `goldfish::display::IDisplay` (`@goldfish//emulator/plugin/display`).
  - In `DisplayServiceImpl::streamScreenshot`, tracks frame sequence continuity (`last_seq`, `current_seq`) and emits Perfetto instrumentation under category `"display"` (`TRACE_EVENT_CATEGORY_ENABLED("display")`):
    - Counter `"dropped_frames"` (`TRACE_COUNTER`) and instant event `"FrameDropped"` (`TRACE_EVENT_INSTANT` with `dropped_count`, `last_seq`, `current_seq`).
    - Counter `"grpc_frame_interval_ms"` (`TRACE_COUNTER`) for inter-frame intervals.
    - Slice `"grpc::ServerWriter::Write"` (`TRACE_EVENT`) wrapping `writer->Write(reply)`.
  - In `DisplayServiceImpl::getScreenshot`, traces `"DisplayServiceImpl::getScreenshot"`, `"WaitForFrameArrival"`, and `"display->GetPixels"`.
- **Other Domain Handlers (`:emulator_controller_methods`)**:
  - `audio_stream_writer.h` / `audio_stream_writer.cc` (`audio_stream_writer_test.cc`)
  - `battery_service.h` / `battery_service.cc`
  - `clipboard_service.h` / `clipboard_service.cc`
  - `fingerprint_service.h` / `fingerprint_service.cc` (`fingerprint_service_test.cc`)
  - `gps_service.h` / `gps_service.cc`
  - `microphone_service.h` / `microphone_service.cc` (`microphone_service_test.cc`): `MicrophoneServiceImpl` (`getMicrophoneState`, `setMicrophoneState`) maps `MicrophoneState.realAudioEnabled` onto the mute flag of the `goldfish::audio::AudioInputMux` `"host"` input, the control v2 `AudioService.UpdateAudioState` also exposes. It finds the mixer on first use (it may be created after the gRPC server), and again once it shuts down, and from then on fires a sticky `MicrophoneState` notification for every `"host"` mute change, from v1 or v2. The mixer observer runs with the BQL held, so it only posts to the injected event loop (`goldfish::avd::globalEventLoop()` in production), where notifications fire; bursts coalesce to the latest state, and `Publish()` `DCHECK`s that the BQL is not held. `FAILED_PRECONDITION` without a live mixer.
  - `notification_store.h` / `notification_stream_writer.h` (`notification_store_test.cc`)
  - `sensor_service.h` / `sensor_service.cc` (`sensor_service_test.cc`)
  - `status_service.h` / `status_service.cc` (`status_service_test.cc`)
  - `vm_service.h` / `vm_service.cc` (`vm_service_test.cc`)
