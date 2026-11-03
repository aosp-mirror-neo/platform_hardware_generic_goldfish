// Copyright 2015 The Android Open Source Project
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

#include <optional>
#include <string>
#include <string_view>

#include "android/crashreport/hang_detector.h"

namespace android::crashreport {

/**
 * @brief Singleton class tracking crashpad annotations
 *
 * Provides methods to attach and update Crashpad annotations.
 */
class CrashReporter {
  public:
    virtual ~CrashReporter() = default;

    /**
     * @brief Sets or replaces a key-value annotation in the crash report.
     *
     * Allocates or updates a power-of-2 Crashpad annotation bucket in place
     * (up to 16384 bytes; larger values are truncated).
     *
     * @param name Annotation key name.
     * @param value String value to attach.
     */
    virtual void SetAnnotation(std::string_view name, std::string_view value) = 0;

    /**
     * @brief Returns the current value of an attached annotation if set.
     */
    [[nodiscard]] virtual std::optional<std::string> GetAttachedData(
            std::string_view name) const = 0;

    /**
     * @brief Adds an additional message to the default `internal-msg` annotation.
     *
     * Note: this can hold at most 8kb of data.
     *
     * @param message The message.
     */
    virtual void AddMessage(std::string_view message) = 0;

    /**
     * @brief Generates a crash dump and terminates the process.
     *
     * The message will be added to the 'internal-msg' annotation.
     *
     * @param message The message to include in the dump.
     */
    [[noreturn]] virtual void Die(std::string_view message) = 0;

    /**
     * @brief Gets the singleton instance of the CrashReporter.
     *
     * @return A reference to the CrashReporter instance.
     */
    static CrashReporter& Get();

    static HangDetector& GetCrashingHangDetector();
};

}  // namespace android::crashreport
