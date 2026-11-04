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
#include <windows.h>

#if defined(UNICODE) || defined(_UNICODE)
#error "This file should be compiled without UNICODE defined, we rely on utf-8 manifest being present"
#endif

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "absl/base/thread_annotations.h"
#include "absl/cleanup/cleanup.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

#include "android/base/file_system_watcher.h"
#include "android/base/scoped_file_handle.h"
#include "android/base/win32_unicode_string.h"
#include "goldfish/file/file.h"

namespace android::base {

using FileSystemWatcherCallback = FileSystemWatcher::FileSystemWatcherCallback;
using WatcherChangeType = FileSystemWatcher::WatcherChangeType;

/**
 * @brief Windows implementation of the FileSystemWatcher.
 *
 * This implementation is based on the `ReadDirectoryChangesW` API, which
 * provides an efficient way to monitor a directory for changes.
 */
class ReadDirectoryChangesWin32 : public FileSystemWatcher {
  public:
    enum class State : std::uint8_t {
        kIdle,      ///< No thread is running. Initial and final state.
        kStarting,  ///< Start() was called, thread is spawning and setting up resources.
        kRunning,   ///< Background thread is actively monitoring.
        kStopping   ///< Stop() was called, thread is signaled to exit.
    };

    ReadDirectoryChangesWin32(std::filesystem::path path,
                              FileSystemWatcherCallback on_change_callback)
            : FileSystemWatcher(std::move(on_change_callback)), path_(std::move(path)) {}

    ~ReadDirectoryChangesWin32() override { Stop(); }

    bool Start() override {
        {
            const absl::MutexLock lock(&mu_);
            if (state_ != State::kIdle) {
                return false;
            }

            state_ = State::kStarting;
            worker_done_ = false;
            DCHECK(!watcher_thread_.joinable()) << "A watcher thread is active.";
            watcher_thread_ = std::thread([this] { WatchForChanges(); });

            // Wait for the thread to move out of the starting state.
            mu_.Await(absl::Condition(this, &ReadDirectoryChangesWin32::IsNotStarting));
            if (state_ == State::kRunning) {
                return true;
            }
        }

        Stop();
        return false;
    }

    void Stop() override {
        std::thread thread_to_join;
        {
            const absl::MutexLock lock(&mu_);
            if (state_ == State::kIdle) {
                DCHECK(!watcher_thread_.joinable());
                return;
            }
            if (state_ == State::kStopping) {
                mu_.Await(absl::Condition(this, &ReadDirectoryChangesWin32::IsIdle));
                return;
            }

            state_ = State::kStopping;
            if (dir_handle_) {
                CancelIoEx(dir_handle_.get(), NULL);
            }
            thread_to_join = std::move(watcher_thread_);
        }

        if (thread_to_join.joinable()) {
            thread_to_join.join();
        }

        {
            const absl::MutexLock lock(&mu_);
            dir_handle_.reset();
            state_ = State::kIdle;
        }
    }

  private:
    bool IsNotStarting() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
        return state_ != State::kStarting || worker_done_;
    }

    bool IsIdle() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) { return state_ == State::kIdle; }

    // Returns true if the watcher is currently running.
    bool IsRunning() ABSL_LOCKS_EXCLUDED(mu_) {
        const absl::MutexLock lock(&mu_);
        return state_ == State::kRunning;
    }

    void WatchForChanges() {
        absl::Cleanup mark_worker_done = [this] {
            const absl::MutexLock lock(&mu_);
            worker_done_ = true;
        };
        // Use A variant with UTF-8 path.
        ScopedFileHandle dir_handle(CreateFileA(
                path_.string().c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL));
        ScopedEventHandle overlap_event(CreateEvent(NULL, TRUE, FALSE, NULL));

        if (!dir_handle || !overlap_event) {
            return;
        }

        HANDLE raw_dir_handle = dir_handle.get();
        HANDLE raw_overlap_event = overlap_event.get();

        {
            const absl::MutexLock lock(&mu_);
            if (state_ == State::kStopping) {
                return;
            }
            dir_handle_ = std::move(dir_handle);
            state_ = State::kRunning;
        }

        BYTE buffer[4096];
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = raw_overlap_event;
        bool io_pending = false;
        absl::Cleanup drain_pending_io = [&] {
            if (io_pending) {
                CancelIo(raw_dir_handle);
                DWORD ignored = 0;
                GetOverlappedResult(raw_dir_handle, &overlapped, &ignored, TRUE);
            }
        };

        while (true) {
            if (!IsRunning()) {
                return;
            }
            ResetEvent(raw_overlap_event);

            // Note: There is NO ReadDirectoryChangesA API. We must use ReadDirectoryChangesW.
            // It populates the buffer with FILE_NOTIFY_INFORMATION which contains WCHAR FileName.
            if (!ReadDirectoryChangesW(raw_dir_handle, buffer, sizeof(buffer), TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                               FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                               FILE_NOTIFY_CHANGE_SIZE |
                                               FILE_NOTIFY_CHANGE_LAST_WRITE,
                                       NULL, &overlapped, NULL)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    break;
                }
            }
            io_pending = true;

            if (!IsRunning()) {
                return;
            }
            DWORD wait = WaitForSingleObject(raw_overlap_event, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                return;
            }

            // Overlap event signaled (either IO completed or CancelIoEx was called).
            if (!IsRunning()) {
                return;
            }

            DWORD bytes_returned = 0;
            BOOL result = GetOverlappedResult(raw_dir_handle, &overlapped, &bytes_returned, FALSE);
            io_pending = false;
            if (result && bytes_returned > 0) {
                ProcessBuffer(buffer, bytes_returned);
            }
        }
    }

    void ProcessBuffer(BYTE* buffer, DWORD bytes_returned) {
        DWORD offset = 0;
        while (true) {
            FILE_NOTIFY_INFORMATION* info =
                    reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer + offset);

            // FILE_NOTIFY_INFORMATION always uses WCHAR for FileName.
            const std::filesystem::path changed =
                    path_ / Win32UnicodeString::convertToUtf8(
                                    info->FileName, info->FileNameLength / sizeof(wchar_t));

            if (!IsRunning()) {
                return;
            }

            switch (info->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_RENAMED_NEW_NAME:
                change_callback(WatcherChangeType::kCreated, changed);
                break;
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_RENAMED_OLD_NAME:
                change_callback(WatcherChangeType::kDeleted, changed);
                break;
            case FILE_ACTION_MODIFIED:
                change_callback(WatcherChangeType::kChanged, changed);
                break;
            }

            if (info->NextEntryOffset == 0) {
                break;
            }
            offset += info->NextEntryOffset;
        }
    }

    const std::filesystem::path path_;
    absl::Mutex mu_;
    State state_ ABSL_GUARDED_BY(mu_) = State::kIdle;
    bool worker_done_ ABSL_GUARDED_BY(mu_) = false;
    ScopedFileHandle dir_handle_ ABSL_GUARDED_BY(mu_);
    std::thread watcher_thread_ ABSL_GUARDED_BY(mu_);
};

std::unique_ptr<FileSystemWatcher> FileSystemWatcher::GetFileSystemWatcher(
        const std::filesystem::path& path, const FileSystemWatcherCallback& on_change_callback) {
    if (!base::file::is_dir(path)) {
        return nullptr;
    }
    return std::make_unique<ReadDirectoryChangesWin32>(path, on_change_callback);
};

}  // namespace android::base
