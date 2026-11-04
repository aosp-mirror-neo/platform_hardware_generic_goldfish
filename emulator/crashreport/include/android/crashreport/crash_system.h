// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS);
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <filesystem>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace android::crashreport {

namespace fs = std::filesystem;

enum class Consent {
    ASK,     ///< Ask for consent.
    ALWAYS,  ///< Always grant consent.
    NEVER    ///< Never grant consent.
};

enum class ReportAction {
    UNDECIDED_KEEP,  ///< Consent is undecided, keep the report.
    UPLOAD_REMOVE,   ///< Upload the report and remove it from the database.
    REMOVE           ///< Remove the report without uploading.
};

class CrashSystem {
  public:
    virtual ~CrashSystem() = default;

    virtual bool active() const = 0;

    /**
     * @brief Initializes the crash reporting system.
     *
     * This function sets up crash reporting for the emulator. It should be called
     * only once at program startup. This function attempts to start the
     * out-of-process Crashpad handler. If the handler is successfully started,
     * crashes will be recorded in a local database and can be uploaded later. If
     * the Crashpad handler cannot be started, crash reporting will be disabled, and
     * the returned status says why.
     *
     * Internally, this function utilizes the `CrashpadClient::StartHandler()`
     * method to establish communication with the Crashpad handler and configure
     * exception handling. Refer to the documentation of `StartHandler()` for
     * platform-specific details on how the handler is started and how exception
     * ports or signal handlers are configured.
     *
     * Crash reporting is controlled by the `ANDROID_EMU_ENABLE_CRASH_REPORTING`
     * environment variable. If this variable is not set or is set to "0", crash
     * reporting will be disabled.
     *
     * @param argc The number of command line arguments. These are used to populate
     *     the crash report if a crash occurs.
     * @param argv An array of command line arguments. These are used to populate
     *     the crash report if a crash occurs.
     * @return OK if crash reporting is active or intentionally disabled via
     *     `ANDROID_EMU_ENABLE_CRASH_REPORTING`, otherwise an error saying why
     *     initialization failed, for the caller to log (Crashpad's own logging
     *     is not shown on Windows).
     *
     * @note If the Crashpad handler cannot be found, crash reporting will be
     *     disabled. The handler location can be influenced by setting the
     *     `AEMU_CRASHPAD_HANDLER` environment variable.
     *
     * @note The crash report database is stored in a platform-specific temporary
     *     directory by default, and its name is based on the emulator version.
     *     The location can be overridden by setting the
     *     `ANDROID_EMU_CRASH_REPORTING_DATABASE` environment variable. Missing
     *     parent directories are created (see `ensureDatabaseDirectory()`).
     *
     * @note On macOS every child process will be handled by this reporter, on every
     *     other platform a reporter will need to be registered by the client.
     */
    // Catch crashes in everything.
    // This promises to not launch any threads...
    virtual absl::Status initialize() = 0;

    virtual void uploadEntries(const Consent consent) = 0;

    /**
     * @brief Returns the path to the crash report database directory.
     *
     * @return The database directory as a FilePath.
     */
    static fs::path databaseDirectory();

    /**
     * @brief Creates the crash report database directory if it is missing.
     *
     * Crashpad only creates the last component of the database path, so a
     * missing parent directory (for example `C:\tmp` for `/tmp/crash-report.db`
     * on Windows) disables crash reporting. This creates the directory and any
     * missing parents, owner-only on POSIX.
     *
     * @param database_path The database directory, absolute or relative to the
     *     current working directory.
     * @return The absolute database directory, or an error naming the path
     *     that could not be created and why.
     */
    static absl::StatusOr<fs::path> ensureDatabaseDirectory(const fs::path& database_path);

    /**
     * @brief Returns the path to the crashpad handler executable.
     *
     * @return The handler executable path as a FilePath.
     */
    static fs::path handlerExe();

    // Gets a handle to single instance of crash reporter
    static CrashSystem& get();
};

}  // namespace android::crashreport
