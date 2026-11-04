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

#include <cstdint>

// This file provides declarations for the fake QEMU main loop functions
// implemented in fake_qemu_callbacks.cpp. These functions are used for
// unit testing of the QemuEventLoop.

extern "C" {
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "migration/snapshot.h"

#ifdef shutdown
#undef shutdown
#endif
#ifdef close
#undef close
#endif
// IWYU pragma: end_keep
// clang-format on

// Advances the fake clock and processes any events that occurred during that
// time.
void fake_qemu_advance_ms(int64_t ms);

// Fires a snapshot progress event to all registered QEMU snapshot progress
// handlers.
void fake_qemu_fire_snapshot_progress(const QemuSnapshotProgress* progress);

// Resets all fake QEMU state to its initial values.
void fake_qemu_reset();

// Enables or disables tracking of per-thread BQL state and unlock counts in
// fake_qemu_callbacks.cc.
void fake_qemu_set_bql_tracking(bool enabled);

// Returns the number of times bql_unlock() has been called while BQL tracking
// is enabled.
int64_t fake_qemu_bql_unlock_count();
}
