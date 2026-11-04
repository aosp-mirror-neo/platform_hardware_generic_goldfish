// Copyright 2022 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "absl/log/check.h"

#define MINI_CHROMIUM_BASE_LOGGING_H_
#define MINI_CHROMIUM_BASE_CHECK_H_

#include "client/annotation.h"

namespace android::crashreport {

// Non-polymorphic updatable Crashpad string annotation backed by a fixed-capacity
// buffer. Once registered with SetSize(> 0), instances must never be destroyed
// because Crashpad's global AnnotationList retains raw Annotation pointers; call
// Retire() instead when retiring an outgrown bucket.
class UpdatableStringAnnotation final : public crashpad::Annotation {
  public:
    UpdatableStringAnnotation(std::string_view name, size_t capacity,
                              std::string_view initial_value)
            : UpdatableStringAnnotation(name, capacity,
                                        std::make_unique_for_overwrite<char[]>(capacity),
                                        initial_value) {}

    UpdatableStringAnnotation(const UpdatableStringAnnotation&) = delete;
    UpdatableStringAnnotation& operator=(const UpdatableStringAnnotation&) = delete;
    UpdatableStringAnnotation(UpdatableStringAnnotation&&) = delete;
    UpdatableStringAnnotation& operator=(UpdatableStringAnnotation&&) = delete;

    void Update(std::string_view value) {
        DCHECK_LE(value.size(), capacity_);
        const size_t len = std::min(value.size(), capacity_);
        auto guard = TryCreateScopedSpinGuard(kSpinGuardTimeoutNs);
        if (!guard.has_value()) {
            return;
        }
        // Clear size_ before mutating buffer_ so an out-of-process crashpad_handler
        // (which does not inspect spin_guard_state_ on Linux/macOS/Windows) never
        // reads a hybrid of new prefix + old suffix bytes mid-memcpy.
        Clear();
        std::atomic_thread_fence(std::memory_order_release);
        if (len > 0) {
            std::memcpy(buffer_.get(), value.data(), len);
        }
        std::atomic_thread_fence(std::memory_order_release);
        SetSize(static_cast<ValueSizeType>(len));
    }

    void Retire() {
        auto guard = TryCreateScopedSpinGuard(kSpinGuardTimeoutNs);
        if (guard.has_value()) {
            Clear();
        }
    }

    [[nodiscard]] std::string_view ValueView() const {
        return std::string_view(buffer_.get(), size());
    }

    [[nodiscard]] size_t Capacity() const { return capacity_; }

  private:
    static constexpr uint64_t kSpinGuardTimeoutNs = 1'000'000'000ULL;

    UpdatableStringAnnotation(std::string_view name, size_t capacity,
                              std::unique_ptr<char[]> buffer, std::string_view initial_value)
            : Annotation(Type::kString, name_, buffer.get(),
                         ConcurrentAccessGuardMode::kScopedSpinGuard)
            , capacity_(capacity)
            , buffer_(std::move(buffer)) {
        DCHECK_LT(capacity_, kValueMaxSize);
        const size_t name_len = std::min(name.size(), kNameMaxLength - 1);
        if (name_len > 0) {
            std::memcpy(name_, name.data(), name_len);
        }
        name_[name_len] = '\0';
        Update(initial_value);
    }

    char name_[kNameMaxLength] = {0};
    size_t capacity_ = 0;
    std::unique_ptr<char[]> buffer_;
};

}  // namespace android::crashreport
