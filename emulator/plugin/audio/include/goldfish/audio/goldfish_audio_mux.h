// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

namespace goldfish::audio {

/**
 * Default QOM ID under `/audiodevs` for the wrapped host OS `AudioBackend`.
 */
inline constexpr char kDefaultHostAudiodevId[] = "hostaudiodev";

/**
 * Default QOM ID under `/audiodevs` exposed by `goldfish-audio-mux` to guest audio devices,
 * `QemuAudioCapture`, and `QemuAudioInject`.
 */
inline constexpr char kDefaultMainAudiodevId[] = "mainaudiodev";

/**
 * QOM type name for the `AudioBackend` decorator that multiplexes guest microphone input between
 * a wrapped host `AudioBackend` and `QemuAudioInject`.
 */
inline constexpr char kGoldfishAudioBackendType[] = "goldfish-audio-backend";

/**
 * QOM device type name (`-device goldfish-audio-mux,host-audiodev=...,audiodev-id=...`) that
 * realizes a `goldfish-audio-backend` under `/audiodevs/<audiodev-id>`.
 */
inline constexpr char kGoldfishAudioMuxDeviceType[] = "goldfish-audio-mux";

/**
 * Registers the `goldfish-audio-backend` and `goldfish-audio-mux` QOM types with QEMU.
 */
void audio_mux_register_types();

}  // namespace goldfish::audio
