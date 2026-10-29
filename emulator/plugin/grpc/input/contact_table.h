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

#include <bitset>
#include <cstdint>
#include <optional>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/status/statusor.h"

#include "input_session_id.h"

namespace goldfish::grpc::v2 {

/** @brief The resolved physical touch device (display) a contact is on. */
using TouchDeviceId = uint32_t;

/**
 * @brief Number of multi-touch slots per touch device (slots 0..10).
 *
 * Must stay in sync with the `ABS_MT_SLOT` maximum of 10 (`MTS_POINTERS_NUM`)
 * advertised by `virtio-input-android`.
 */
inline constexpr int kTouchDeviceSlots = 11;

/**
 * @brief Slots per device that non-human sessions can never occupy, so
 * automated input cannot starve the human operator.
 */
inline constexpr int kReservedHumanSlots = 2;

/** @brief Identifies one pointer of one input session. */
struct ContactKey {
    InputSessionId session;
    int32_t pointer_id;

    friend bool operator==(const ContactKey&, const ContactKey&) = default;

    template <typename H>
    friend H AbslHashValue(H h, const ContactKey& key) {
        return H::combine(std::move(h), key.session, key.pointer_id);
    }
};

/** @brief The multi-touch slot a contact occupies on a touch device. */
struct Contact {
    TouchDeviceId device;
    int slot;
    /**
     * @brief The display the client requested when the contact was created,
     * which can differ from the device it resolved to (a foldable may route
     * display 0 to another device).
     */
    int32_t display_id;

    friend bool operator==(const Contact&, const Contact&) = default;
};

using ReleasedContacts = absl::InlinedVector<std::pair<ContactKey, Contact>, kTouchDeviceSlots>;

/**
 * @brief Owns multi-touch slot allocation for every touch device, so that
 * independent input sessions never collide on the same guest slot.
 *
 * A contact is bound to the device it was first acquired on until it is
 * released; callers use the slot number as the evdev tracking ID.
 *
 * ### Invariants:
 * - Each held `ContactKey` maps to exactly one `(device, slot)` and keeps the
 *   display it was acquired for.
 * - Each `(device, slot)` has at most one owner.
 * - Contacts of non-human sessions on a device never exceed
 *   `kTouchDeviceSlots - kReservedHumanSlots`.
 *
 * ### Thread Safety:
 * Not thread-safe. Owned and used only by the input dispatcher on the QEMU
 * event loop.
 */
class ContactTable {
  public:
    /**
     * @brief Returns the slot held by `key`, allocating the lowest free slot on
     * `device` if `key` holds none.
     *
     * @param key The session pointer that wants a slot.
     * @param device Device to allocate on; ignored if `key` already holds a
     *     contact, which stays on the device it was first acquired on.
     * @param display_id The display the client requested, which `device` was
     *     resolved from; ignored, like `device`, if `key` holds a contact.
     * @param human Whether the session is driven by a human; only human
     *     sessions may use the reserved slots.
     * @return The slot, or `ResourceExhaustedError` (with no state change) if
     *     no slot is available to the caller.
     */
    absl::StatusOr<int> Acquire(ContactKey key, TouchDeviceId device, int32_t display_id,
                                bool human);

    std::optional<Contact> Find(const ContactKey& key) const;
    bool IsHeld(const ContactKey& key) const { return contacts_.contains(key); }
    bool HasSession(InputSessionId session) const;

    /**
     * @brief Frees the slot held by `key`.
     * @return The freed contact, if any.
     */
    std::optional<Contact> Release(const ContactKey& key);

    /**
     * @brief Frees all contacts of `session`.
     * @return The released contacts, sorted by `(device, slot)`.
     */
    ReleasedContacts ReleaseSession(InputSessionId session);

    /**
     * @brief Frees the contacts of `session` on `device`.
     * @return The released contacts, sorted by `slot`.
     */
    ReleasedContacts ReleaseSessionOn(InputSessionId session, TouchDeviceId device);

    /**
     * @brief Frees all contacts on `device`.
     * @return The released contacts, sorted by `slot`.
     */
    ReleasedContacts ClearDevice(TouchDeviceId device);

    /**
     * @brief Frees all contacts across all devices.
     * @return The released contacts, sorted by `(device, slot)`.
     */
    ReleasedContacts Clear();

  private:
    struct HeldContact {
        Contact contact;
        bool human;
    };
    struct DeviceSlots {
        std::bitset<kTouchDeviceSlots> used;
        int non_human = 0;
    };

    template <typename Predicate>
    ReleasedContacts ReleaseIf(Predicate matches);
    void FreeSlot(const HeldContact& held);

    absl::flat_hash_map<ContactKey, HeldContact> contacts_;
    absl::flat_hash_map<TouchDeviceId, DeviceSlots> devices_;
};

}  // namespace goldfish::grpc::v2
