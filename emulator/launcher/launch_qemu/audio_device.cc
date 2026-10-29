// Copyright 2024 The Android Open Source Project
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

#include "audio_device.h"

#ifdef __linux__
#include <filesystem>
#endif
#include <initializer_list>
#include <string>
#include <string_view>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

#include "android/base/system.h"
#include "android/goldfish/avd.h"
#include "goldfish/file/file.h"

namespace android::goldfish {

absl::Status AudioDevice::initialize(const EmulatorConfig& emulator) {
    return absl::OkStatus();
}

std::vector<std::string> AudioDevice::getQemuParameters(const EmulatorConfig& emulator) const {
    using namespace std::literals;
    const std::string_view kHostId = "id=hostaudiodev"sv;

    std::string audioBackend = getAudioBackend(emulator.opts());
    std::string_view audioSettings;
    if (emulator.opts().noaudio) {
        audioBackend = "none"s;
        audioSettings = "out.mixing-engine=off,in.mixing-engine=off"sv;
    } else {
        if (audioBackend.empty()) {
            audioBackend = "none"s;
        }
        audioSettings =
                "out.mixing-engine=on,out.fixed-settings=on,"
                "out.frequency=48000,out.format=s16,out.channels=2,"
                "in.mixing-engine=on,in.fixed-settings=on,"
                "in.frequency=48000,in.format=s16,in.channels=1"sv;
    }

    // The host microphone stays muted unless the user opts in with -allow-host-audio.
    const std::string muxDevice =
            absl::StrCat("goldfish-audio-mux,host-audiodev=hostaudiodev,audiodev-id=mainaudiodev"sv,
                         emulator.opts().allow_host_audio ? ",allow-host-audio=on"sv : ""sv);

    switch (emulator.avd().Arch()) {
    case Avd::CpuArchitecture::kArm:
    case Avd::CpuArchitecture::kX86:
        return {
            "-audiodev"s, absl::StrFormat("%s,%s,%s", audioBackend, kHostId, audioSettings),
            "-device"s,   muxDevice,
            "-device"s,   absl::StrCat("virtio-sound-pci,audiodev=mainaudiodev,addr="sv, addr()),
        };

    case Avd::CpuArchitecture::kRiscV:
    case Avd::CpuArchitecture::kUnknown:
        break;
    }

    return {};
}

std::string AudioDevice::getAudioBackend(const AndroidOptions& opts) {
    if (opts.noaudio) {
        return {};
    }

    const char* const audioBackendOpt = opts.audio;
    if (audioBackendOpt && *audioBackendOpt) {
        return audioBackendOpt;
    } else {
        std::string backend = detectHostAudioBackend();
        if (backend.empty()) {
            LOG(WARNING) << "No host audio backend detected; host audio playback/capture is "
                            "disabled and QEMU is falling back to the 'none' driver with software "
                            "mixing enabled (pass '-audio <backend>' to select a host driver, or "
                            "'-no-audio' to disable audio and suppress this warning).";
        }
        return backend;
    }
}

#if defined(__APPLE__)
// coreaudio.m (coreaudio_audio_init)
std::string AudioDevice::detectHostAudioBackend() {
    using namespace std::literals;
    return "coreaudio"s;
}
#elif defined(__linux__)
// paaudio.c (qpa_audio_init)
std::string AudioDevice::detectHostAudioBackend() {
    using namespace std::literals;
    std::string runtime = android::base::System::Get()->EnvGet("XDG_RUNTIME_DIR");
    if (runtime.empty()) {
        return {};
    }

    if (!base::file::exists(absl::StrFormat("%s/pulse/pid", runtime))) {
        return {};
    }

    return "pa"s;
}
#elif defined(_WIN32)
// audio_wasapi.cc (WasapiRegisterTypes)
std::string AudioDevice::detectHostAudioBackend() {
    using namespace std::literals;
    return "wasapi"s;
}
#else
#error Unexpected platform
#endif

}  // namespace android::goldfish
