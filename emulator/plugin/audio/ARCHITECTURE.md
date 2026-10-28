# Component: QEMU Audio Plugin

**Role:** Captures guest speaker output (`SWVoiceOut`) and injects external microphone PCM audio (`SWVoiceIn`) into QEMU's `AudioBackend` subsystem via pull callbacks.
**Location:** `hardware/generic/goldfish/emulator/plugin/audio`
**Namespace:** `goldfish::audio`

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `QemuAudioCapture` | `:qemu_audio_capture` | `include/goldfish/audio/qemu_audio_capture.h` | Low-level QEMU speaker output (`CaptureVoiceOut`) hook forwarding guest PCM to a callback. |
| `QemuAudioSource` | `:qemu_audio_source` | `include/goldfish/audio/qemu_audio_source.h` | WebRTC audio source adapter delivering captured guest speaker samples to WebRTC tracks. |
| `QemuAudioInject` | `:qemu_audio_inject` | `include/goldfish/audio/qemu_audio_inject.h` | Pull-based microphone audio injector supporting arbitrary caller formats, channel remixing, and resampling. |
| `GoldfishAudioBackend` | `:goldfish_audio_mux` | `include/goldfish/audio/goldfish_audio_mux.h` | QOM `AudioBackend` decorator (`goldfish-audio-mux`) wrapping `/audiodevs/hostaudiodev` as `/audiodevs/mainaudiodev`. |
| `AudioInputMux` | `:audio_input_mux` | `include/goldfish/audio/audio_input_mux.h` | Pure C++ state machine multiplexing guest `SWVoiceIn` streams between host microphone and `QemuAudioInject`. |
| `PcmPullPipeline` | `:pcm_pull_pipeline` | `include/goldfish/audio/pcm_pull_pipeline.h` | Zero-allocation pull converter supporting `U8/S16/S32/F32`, channel remixing, guest `Volume`, and `webrtc::SincResampler`. |
| `VirtualAudioPacer` | `:virtual_audio_pacer` | `include/goldfish/audio/virtual_audio_pacer.h` | Drift-free 128-bit `QEMU_CLOCK_VIRTUAL` byte-quota pacer with snapshot clock-jump recovery. |

## Critical Infrastructure
* **Zero-Patch QOM Decoration:** `goldfish-audio-mux` wraps `/audiodevs/hostaudiodev` and exposes `/audiodevs/mainaudiodev`, delegating all output/capture calls directly to the host backend while intercepting `SWVoiceIn` operations.
* **Host OS Mic Release:** Activating `QemuAudioInject` immediately deactivates the wrapped host input voice (`set_active_in(..., false)`), releasing CoreAudio/PulseAudio/DirectSound capture and clearing host OS microphone indicators.
* **Virtual-Clock Pacing:** Paces injected (or host-mic-less silence) reads on a 10 ms `QEMU_CLOCK_VIRTUAL` timer so `virtio-snd` ring buffers are never overrun.
* **Teardown Ordering Safety:** `GoldfishAudioBackend::unparent` calls `AudioInputMux::Shutdown()` during `audio_cleanup()`, allowing late `~QemuAudioInject()` calls during `deletable_cleanup()` to exit safely without touching finalized QOM state.

## Dependencies
* **QEMU Headers:** `@qemu//:aemu_func_defs`, `@qemu//:qemu-headers-exported`.
* **WebRTC Resampler:** `@webrtc//common_audio` (`webrtc::SincResampler`).
* **Videobridge:** `//emulator/videobridge:in_process_audio_source`, `//emulator/videobridge:videobridge_core`.
* **VM Interface:** `//emulator/plugin/vminterface:vm_lock`.

## Threading Model
* **Lock Hierarchy:** `QEMU BQL (RecursiveScopedVmLock)` -> `QemuAudioInject::lifecycle_mutex_` -> `AudioInputMux::host_dispatch_mutex_` -> `VoicePipeline::pull_mutex_` -> `AudioInputMux::mux_state_mutex_`.
* **Callback Discipline:** `ReadCallback` and `AudioCallback` run synchronously on QEMU's main-loop / vCPU thread under the BQL and must never perform blocking I/O. `mux_state_mutex_` is a pure leaf lock and is never held across callbacks.
