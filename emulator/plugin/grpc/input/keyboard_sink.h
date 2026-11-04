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

#include <cstdint>
#include <string_view>

namespace goldfish::grpc::v2 {

/**
 * @brief The guest keyboard, shared by every input session.
 *
 * The caller decides which key transitions reach the guest; the sink writes
 * each one as it is given, synchronously, so keys and touches written in order
 * on the loop are injected in that order.
 *
 * ### Thread Safety:
 * Called only on the QEMU event loop.
 *
 * ### Ownership:
 * Not owned by its callers; it must outlive them.
 */
class KeyboardSink {
  public:
    virtual ~KeyboardSink() = default;

    /** @brief Writes a press (`down`) or release of the Linux evdev key `evdev_code`. */
    virtual void WriteKey(uint32_t evdev_code, bool down) = 0;

    /** @brief Types `utf8` into the guest, leaving no key held. */
    virtual void TypeText(std::string_view utf8) = 0;

    /**
     * @brief Releases every key the guest may hold, whether pressed through
     * `WriteKey()` or restored by a snapshot, and resets the sink's state.
     *
     * Callers release nothing beforehand: the sink paces the releases so
     * that, together, they fit the guest's input queue.
     */
    virtual void Reset() = 0;
};

}  // namespace goldfish::grpc::v2
