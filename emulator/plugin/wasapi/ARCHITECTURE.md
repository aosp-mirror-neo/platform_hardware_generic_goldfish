# Component: Windows WASAPI Audio Backend Plugin

**Role:** Implements QEMU's `AudioMixengBackend` (`audio-wasapi`) on Windows using shared-mode, event-driven WASAPI (`IMMDeviceEnumerator`, `IAudioClient`, `IAudioRenderClient`, `IAudioCaptureClient`), with non-fatal startup and dynamic endpoint recovery when playback or recording hardware is unavailable.
**Location:** `hardware/generic/goldfish/emulator/plugin/wasapi`
**Namespace:** `goldfish::wasapi`

## Integration Guide
| Class / Interface | Bazel Target | Header Path | Description |
| :--- | :--- | :--- | :--- |
| `WasapiRegisterTypes` | `:wasapi` | `include/goldfish/wasapi/audio_wasapi.h` | Registers the `audio-wasapi` QOM `AudioMixengBackend` subclass during goldfish plugin QOM initialization. |
| `ParseWaveFormat` | `:pcm_converter` | `pcm_converter.h` | Host-agnostic `WAVEFORMATEX` parser and QEMU ring-buffer channel-count adapter. |
| `WasapiNotificationClient` | `:wasapi_win32` | `wasapi_win32.h` | COM `IMMNotificationClient` and RAII (`IntrusivePtr`, `UniqueHandle`) helpers for Windows audio endpoints. |

## Critical Infrastructure
* **Non-Fatal Hardware Absence:** Missing playback or microphone endpoints (`E_NOTFOUND` or disabled audio service) log an actionable error without failing `init_out` / `init_in`, pacing wall-clock drain/silence until a default endpoint becomes available.
* **Format & Channel Adaptation:** Initializes QEMU's voice (`audio_pcm_init_info`) directly with the Windows shared-mode default mix format (`GetMixFormat`) and default period (`GetDevicePeriod`), adapting channel counts between QEMU's ring buffer and the hardware buffer and using WASAPI's built-in `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` converter if the default endpoint changes at runtime to a different sample rate or format.
* **RAII Resource Management:** Uses `goldfish::base::IntrusivePtr` for COM interfaces and `goldfish::base::UniqueHandle` / `android::base::ScopedEventHandle` for Win32 event, `CoTaskMem`, and MMCSS `Avrt` handles.

## Dependencies
* **Base & Windows Helpers:** `//emulator/libs/base`, `//emulator/libs/windows:scoped_file_handle`, `//emulator/libs/windows:win32_utils`.
* **QEMU Headers & Imports:** `@qemu//:qemu-headers-exported`.

## Threading Model
* **Dedicated Worker per Voice:** Each `HWVoiceOut` / `HWVoiceIn` owns a dedicated `COINIT_MULTITHREADED` worker thread (boosted via MMCSS `"Pro Audio"`) that exclusively owns all COM/WASAPI interfaces and synchronizes QEMU `buf_emul` ring-buffer accesses with `buf_mutex_`.
