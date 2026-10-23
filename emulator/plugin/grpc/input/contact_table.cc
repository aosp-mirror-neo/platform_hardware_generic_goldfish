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
#include "contact_table.h"

#include <algorithm>
#include <optional>
#include <tuple>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace goldfish::grpc::v2 {

namespace {

constexpr int kNonHumanSlotLimit = kTouchDeviceSlots - kReservedHumanSlots;

}  // namespace

absl::StatusOr<int> ContactTable::Acquire(ContactKey key, TouchDeviceId device, int32_t display_id,
                                          bool human) {
    if (auto it = contacts_.find(key); it != contacts_.end()) {
        return it->second.contact.slot;
    }

    DeviceSlots& slots = devices_[device];
    if (!human && slots.non_human >= kNonHumanSlotLimit) {
        return absl::ResourceExhaustedError(absl::StrCat(
                "Touch device ", device, " has no slot for session ", key.session, " pointer ",
                key.pointer_id, ": automated sessions already hold ", slots.non_human, " of ",
                kTouchDeviceSlots, " slots and ", kReservedHumanSlots,
                " are reserved for human input. Lift a pointer before touching "
                "down another."));
    }
    if (slots.used.all()) {
        return absl::ResourceExhaustedError(
                absl::StrCat("Touch device ", device, " has no slot for session ", key.session,
                             " pointer ", key.pointer_id, ": all ", kTouchDeviceSlots,
                             " slots are held. Lift a pointer before touching down another."));
    }

    int slot = 0;
    while (slots.used.test(slot)) {
        ++slot;
    }
    slots.used.set(slot);
    if (!human) {
        ++slots.non_human;
    }
    contacts_.emplace(key, HeldContact{{device, slot, display_id}, human});
    return slot;
}

std::optional<Contact> ContactTable::Find(const ContactKey& key) const {
    auto it = contacts_.find(key);
    return it != contacts_.end() ? std::optional<Contact>(it->second.contact) : std::nullopt;
}

std::optional<Contact> ContactTable::Release(const ContactKey& key) {
    auto node = contacts_.extract(key);
    if (node.empty()) {
        return std::nullopt;
    }
    FreeSlot(node.mapped());
    return node.mapped().contact;
}

ReleasedContacts ContactTable::ReleaseSession(InputSessionId session) {
    return ReleaseIf(
            [session](const ContactKey& key, const Contact&) { return key.session == session; });
}

ReleasedContacts ContactTable::ReleaseSessionOn(InputSessionId session, TouchDeviceId device) {
    return ReleaseIf([session, device](const ContactKey& key, const Contact& contact) {
        return key.session == session && contact.device == device;
    });
}

ReleasedContacts ContactTable::ClearDevice(TouchDeviceId device) {
    return ReleaseIf([device](const ContactKey&, const Contact& contact) {
        return contact.device == device;
    });
}

ReleasedContacts ContactTable::Clear() {
    return ReleaseIf([](const ContactKey&, const Contact&) { return true; });
}

template <typename Predicate>
ReleasedContacts ContactTable::ReleaseIf(Predicate matches) {
    ReleasedContacts released;
    for (auto it = contacts_.begin(); it != contacts_.end();) {
        if (matches(it->first, it->second.contact)) {
            FreeSlot(it->second);
            released.emplace_back(it->first, it->second.contact);
            contacts_.erase(it++);
        } else {
            ++it;
        }
    }
    std::sort(released.begin(), released.end(), [](const auto& a, const auto& b) {
        return std::tie(a.second.device, a.second.slot) < std::tie(b.second.device, b.second.slot);
    });
    return released;
}

void ContactTable::FreeSlot(const HeldContact& held) {
    DeviceSlots& slots = devices_[held.contact.device];
    slots.used.reset(held.contact.slot);
    if (!held.human) {
        --slots.non_human;
    }
}

}  // namespace goldfish::grpc::v2
