// Copyright (C) 2025 The Android Open Source Project
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
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"

#include "android/emulation/control/ev_dev_event.h"

namespace android::emulation::control {

/**
 * @brief Maximum number of pointers, supported by multi-touch emulation.
 */
constexpr uint32_t kMtsPointersNum = 10;

/**
 * @brief Signals that a pointer is not tracked (or is "up").
 */
constexpr uint32_t kMtsPointerUp = -1;

/**
 * @brief Maximum ABS_MT_PRESSURE value (full contact pressure).
 *
 * Must match MTS_PRESSURE_RANGE_MAX advertised by virtio-input-android.
 */
constexpr uint32_t kMtsPressureMax = 0x400;

/**
 * @brief Manages the allocation and deallocation of touch slots for multi-touch events.
 *
 * This class is responsible for mapping external touch identifiers to internal
 * touch slots used by the Android kernel. It also handles the expiration of
 * old touch events to prevent resource leaks.
 */
class SlotRegistry {
  public:
    /**
     * @brief Default expiration time for touch events (120 seconds).
     * This means that if a given id has not received any updates in 120 seconds it will
     * be closed out upon receipt of the next event.
     */
    static constexpr absl::Duration kTouchExpireAfter120S = absl::Seconds(120);

    /**
     * @brief Constructs a SlotRegistry with a specified slot expiration duration.
     *
     * @param slot_expiration The duration after which a slot is considered expired.
     *                       Defaults to kTouchExpireAfter120S (120 seconds).
     * @param clock The clock to use for timing. Defaults to absl::Clock::GetRealClock().
     */
    explicit SlotRegistry(absl::Duration slot_expiration = kTouchExpireAfter120S,
                          absl::Clock& clock = absl::Clock::GetRealClock())
            : slot_expiration_(slot_expiration), clock_(clock) {}

    /**
     * @brief Acquires a free slot for a given touch identifier.
     *
     * If the identifier is already registered, the existing slot is returned.
     * Otherwise, a new free slot is allocated and associated with the identifier.
     *
     * @param identifier The unique identifier for the touch event.
     * @return The allocated slot number, or -1 if no slots are available.
     */
    int AcquireSlot(uint32_t identifier);

    /**
     * @brief Checks if a slot is currently registered.
     *
     * @param slot The slot number to check.
     * @return True if the slot is registered, false otherwise.
     */
    bool IsSlotRegistered(uint32_t slot);

    /**
     * @brief Checks if a touch identifier is currently registered.
     *
     * @param identifier The touch identifier to check.
     * @return True if the identifier is registered, false otherwise.
     */
    bool IsIdentifierRegistered(uint32_t identifier);

    /**
     * @brief Updates the expiration time for a given touch identifier.
     *
     * This method should be called whenever a touch event is received for an
     * existing identifier to prevent it from expiring prematurely.
     *
     * @param identifier The touch identifier to update.
     */
    void UpdateSlotExpiration(uint32_t identifier);

    /**
     * @brief Releases the slot associated with a given touch identifier.
     *
     * This method should be called when a touch event is finished (e.g., finger lifted).
     *
     * @param identifier The touch identifier to release.
     */
    void ReleaseSlot(uint32_t identifier);

    /**
     * @brief Immediately releases all registered slots and generates corresponding release events.
     *
     * @return A vector of EvDevEvent representing the release events for all active slots.
     */
    std::vector<EvDevEvent> ReleaseAllSlots();

    /**
     * @brief Expires old, unused slots and generates corresponding release events.
     *
     * This method iterates through all registered identifiers and releases any
     * that have not been updated within the expiration duration. It also generates
     * EV_ABS events to signal the release of these slots to the Android kernel.
     *
     * @return A vector of EvDevEvent representing the release events for expired slots.
     */
    std::vector<EvDevEvent> ExpireOldSlots();

    /**
     * @brief Sets the slot expiration duration.
     *
     * @param slot_expiration The new slot expiration duration.
     */
    void SetSlotExpiration(absl::Duration slot_expiration) { slot_expiration_ = slot_expiration; }

  private:
    /**
     * @brief Finds the next free slot.
     *
     * @return The next free slot number, or -1 if no slots are available.
     */
    int FindNextFreeSlot();

    /**
     * @brief Set of active slots.
     */
    std::bitset<kMtsPointersNum> used_slots_;

    /**
     * @brief Maps external touch id to linux slot.
     */
    absl::flat_hash_map<uint32_t, uint32_t> id_map_;

    /**
     * @brief Last time external touch id was used.
     * We use this to expunge dangling events.
     */
    absl::flat_hash_map<uint32_t, absl::Time> id_last_used_epoch_;

    /**
     * @brief The duration after which a slot is considered expired.
     */
    absl::Duration slot_expiration_;

    /**
     * @brief The clock used for timing.
     */
    absl::Clock& clock_;
};
}  // namespace android::emulation::control
