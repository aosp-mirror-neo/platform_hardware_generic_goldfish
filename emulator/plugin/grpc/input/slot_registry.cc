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
#include "slot_registry.h"

#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"

#include "standard-headers/linux/input-event-codes.h"

namespace android::emulation::control {

int SlotRegistry::AcquireSlot(uint32_t identifier) {
    uint32_t slot = 0;
    if (id_map_.count(identifier) == 0) {
        // pick next available slot.
        const int next_slot = FindNextFreeSlot();
        if (next_slot < 0) {
            VLOG(1) << "No slot available for" << identifier;
            return next_slot;
        }
        slot = static_cast<uint32_t>(next_slot);
        id_map_[identifier] = slot;
        used_slots_.set(slot);
    } else {
        slot = id_map_[identifier];
    }

    auto now = clock_.TimeNow();
    id_last_used_epoch_[identifier] = now + slot_expiration_;

    return static_cast<int>(slot);
}

bool SlotRegistry::IsIdentifierRegistered(uint32_t identifier) {
    return id_map_.count(identifier) > 0;
}

bool SlotRegistry::IsSlotRegistered(uint32_t slot) {
    return used_slots_.test(slot);
}

void SlotRegistry::UpdateSlotExpiration(uint32_t identifier) {
    DCHECK(id_last_used_epoch_.count(identifier) > 0);
    auto now = clock_.TimeNow();
    id_last_used_epoch_[identifier] = now + slot_expiration_;
}

void SlotRegistry::ReleaseSlot(uint32_t identifier) {
    if (id_map_.count(identifier) == 0) {
        return;
    }

    const uint32_t slot = id_map_[identifier];
    id_map_.erase(identifier);
    used_slots_.reset(slot);
    id_last_used_epoch_.erase(identifier);
}

std::vector<EvDevEvent> SlotRegistry::ReleaseAllSlots() {
    std::vector<EvDevEvent> events;
    events.reserve(id_map_.size() * 2);
    for (const auto& [identifier, slot] : id_map_) {
        events.push_back({EV_ABS, ABS_MT_SLOT, slot});
        events.push_back({EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp});
    }
    id_map_.clear();
    id_last_used_epoch_.clear();
    used_slots_.reset();
    return events;
}

std::vector<EvDevEvent> SlotRegistry::ExpireOldSlots() {
    std::vector<EvDevEvent> events;

    const absl::Time now = clock_.TimeNow();
    for (auto it = id_last_used_epoch_.begin(); it != id_last_used_epoch_.end();) {
        if (it->second < now) {
            DCHECK(id_map_.count(it->first) > 0);
            const uint32_t remove_slot = id_map_[it->first];
            VLOG(1) << "Expiring outdated touch event identifier: " << it->first
                    << ", slot: " << remove_slot;

            // First create an up event, otherwise android kernel might get
            // confused
            events.push_back({EV_ABS, ABS_MT_SLOT, remove_slot});
            events.push_back({EV_ABS, ABS_MT_TRACKING_ID, kMtsPointerUp});

            // Next remove the mappings from existence.
            id_map_.erase(it->first);
            id_last_used_epoch_.erase(it++);
            used_slots_.reset(remove_slot);
        } else {
            ++it;
        }
    }

    return events;
}

int SlotRegistry::FindNextFreeSlot() {
    static_assert(kMtsPointersNum < 20, "Consider a better algorithm for finding an empty slot.");
    for (int i = 0; std::cmp_less(i, kMtsPointersNum); i++) {
        if (!used_slots_.test(i)) {
            return i;
        }
    }
    return -1;
}
}  // namespace android::emulation::control
