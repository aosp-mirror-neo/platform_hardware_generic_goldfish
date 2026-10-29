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

// Includes WebRTC headers: only include from targets built with WEBRTC_COPTS (-fno-rtti).

#include <cstdint>
#include <memory>

#include "goldfish/videobridge/goldfish_audio_device_module.h"
#include "goldfish/videobridge/media_provider.h"

extern "C" {
typedef struct AudioBackend AudioBackend;
}

namespace goldfish::avd {
struct AvdUniverse;
}

namespace android::emulation::control {

/**
 * Creates the media of the in-process WebRTC sessions: the guest display and speaker are sent to
 * every participant, and audio participants send, such as a browser microphone, is mixed into the
 * guest microphone as a `kWebRtc` input.
 *
 * @param audio_device The audio device module the received audio is pulled from. Pass the same
 * module to the `Switchboard`, which plays received audio into it.
 */
std::shared_ptr<::goldfish::videobridge::MediaProvider> CreateInProcessMediaProvider(
        ::goldfish::avd::AvdUniverse& avd_universe, uint32_t display_id,
        AudioBackend* audio_backend,
        ::webrtc::scoped_refptr<::goldfish::videobridge::GoldfishAudioDeviceModule> audio_device);

}  // namespace android::emulation::control
