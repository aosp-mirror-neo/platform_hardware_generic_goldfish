// Copyright 2026 The Android Open Source Project
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

namespace android::goldfish {

constexpr int kMinSupportedApiLevel = 31;

// Minimum Android API level where virtio-gpu (minigbm/gfxstream) and modern
// virtio devices replace legacy goldfish_pipe, goldfish_sync, and
// goldfish_address_space.
constexpr int kVirtioReadyApiLevel = 37;

// Minimum Android API level whose goldfish radio HAL
// (android.hardware.radio-service.ranchu) works with netsimd's cellular modem.
// Earlier images abort on the empty operator strings netsimd returns while
// unregistered (unguarded `operatorNumeric.substr(3)` in getMcc()/getMnc()),
// so they use the in-process modem simulator, as the classic emulator did.
constexpr int kNetsimModemMinApiLevel = 37;

// Whether telephony goes through netsimd's cellular modem (otherwise the
// launcher starts the in-process modem simulator). `api_level` is
// `Avd::ApiLevel()`.
constexpr bool UseNetsimModem(bool no_netsim_modem_opt, int api_level) {
    return !no_netsim_modem_opt && api_level >= kNetsimModemMinApiLevel;
}

}  // namespace android::goldfish
