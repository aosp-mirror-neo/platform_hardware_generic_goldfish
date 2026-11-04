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

#include <atomic>
#include <thread>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "emulator/plugin/vminterface/vm_lock.h"

namespace goldfish::audio {

/**
 * A real, per-thread `VmLock` standing in for the QEMU BQL in unit tests. Installs itself as
 * `VmLock::Get()` for its lifetime, so `ScopedVmLock`, `RecursiveScopedVmLock`, and the
 * `AudioInputMux` lock check behave as they do in QEMU.
 */
class TestBql final : public android::goldfish::VmLock {
  public:
    TestBql() : previous_(VmLock::Set(this)) {}
    ~TestBql() override { VmLock::Set(previous_); }

    void Lock() override ABSL_NO_THREAD_SAFETY_ANALYSIS {
        mutex_.Lock();
        owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }

    void Unlock() override ABSL_NO_THREAD_SAFETY_ANALYSIS {
        owner_.store(std::thread::id(), std::memory_order_relaxed);
        mutex_.Unlock();
    }

    bool IsLockedBySelf() const override {
        return owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

  private:
    absl::Mutex mutex_;
    std::atomic<std::thread::id> owner_;
    android::goldfish::VmLock* const previous_;
};

}  // namespace goldfish::audio
