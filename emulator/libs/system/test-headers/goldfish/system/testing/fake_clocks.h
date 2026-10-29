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

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "goldfish/system/steady_clock.h"
#include "goldfish/system/virtual_clock.h"

namespace goldfish::system::testing {

// Thread-safe fake SteadyClock for deterministic unit tests.
class FakeSteadyClock : public SteadyClock {
  public:
    explicit FakeSteadyClock(absl::Duration initial = absl::ZeroDuration())
        : now_(initial) {}

    absl::Duration Now() const override {
        absl::MutexLock lock(mutex_);
        return now_;
    }

    void Set(absl::Duration now) {
        absl::MutexLock lock(mutex_);
        now_ = now;
    }

    void Advance(absl::Duration delta) {
        absl::MutexLock lock(mutex_);
        now_ += delta;
    }

  private:
    mutable absl::Mutex mutex_;
    absl::Duration now_ ABSL_GUARDED_BY(mutex_);
};

// Thread-safe fake VirtualClock for deterministic unit tests.
class FakeVirtualClock : public VirtualClock {
  public:
    explicit FakeVirtualClock(absl::Duration initial = absl::ZeroDuration())
        : now_(initial) {}

    absl::Duration Now() const override {
        absl::MutexLock lock(mutex_);
        return now_;
    }

    void Set(absl::Duration now) {
        absl::MutexLock lock(mutex_);
        now_ = now;
    }

    void Advance(absl::Duration delta) {
        absl::MutexLock lock(mutex_);
        now_ += delta;
    }

  private:
    mutable absl::Mutex mutex_;
    absl::Duration now_ ABSL_GUARDED_BY(mutex_);
};

}  // namespace goldfish::system::testing
