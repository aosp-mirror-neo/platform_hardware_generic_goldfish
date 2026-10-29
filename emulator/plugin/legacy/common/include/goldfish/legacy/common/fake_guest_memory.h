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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#include "goldfish/legacy/common/i_guest_memory.h"

namespace goldfish::legacy {

class FakeGuestMemory : public IGuestMemory {
  public:
    explicit FakeGuestMemory(size_t size) : ram_(size, 0) {}

    bool Read(uint64_t gpa, void* dst, size_t size) ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        if (gpa + size > ram_.size() || gpa + size < gpa) {
            return false;
        }
        std::memcpy(dst, ram_.data() + gpa, size);
        return true;
    }

    bool Write(uint64_t gpa, const void* src, size_t size) ABSL_LOCKS_EXCLUDED(mu_) override {
        absl::MutexLock lock(&mu_);
        if (gpa + size > ram_.size() || gpa + size < gpa) {
            return false;
        }
        std::memcpy(ram_.data() + gpa, src, size);
        return true;
    }

    template <typename T>
    void Put(uint64_t gpa, const T& val) ABSL_LOCKS_EXCLUDED(mu_) {
        Write(gpa, &val, sizeof(T));
    }

    template <typename T>
    T Get(uint64_t gpa) ABSL_LOCKS_EXCLUDED(mu_) {
        T val{};
        Read(gpa, &val, sizeof(T));
        return val;
    }

  private:
    std::vector<uint8_t> ram_ ABSL_GUARDED_BY(mu_);
    absl::Mutex mu_;
};

}  // namespace goldfish::legacy
