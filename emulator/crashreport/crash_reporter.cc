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
#include "android/crashreport/crash_reporter.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/synchronization/mutex.h"

#include "annotation_streambuf.h"
#include "crash_reporter_testing.h"
#include "simple_string_annotation.h"

#ifdef _WIN32
#include <io.h>
#else
#include <csignal>
#endif

namespace android::crashreport {

namespace {

constexpr size_t kMinAnnotationBucket = 64;
constexpr size_t kMaxAnnotationBucket = 16384;
// Crashpad's ProcessSnapshot stops reading AnnotationList after 400 total nodes
// (including retired zero-size buckets), and prepends new nodes at the list head.
// Cap owned nodes well below 400 so startup keys near the tail (`session_id`,
// `process_type`) are never pushed past Crashpad's reader limit.
constexpr size_t kMaxOwnedAnnotationNodes = 350;

constexpr size_t BucketCapacityForSize(size_t size) {
    return std::bit_ceil(std::clamp(size, kMinAnnotationBucket, kMaxAnnotationBucket));
}

void EnableSignalTermination() {
#if defined(__APPLE__) || defined(__linux__)
    // We will not get crash reports without the signals below enabled.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGFPE);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGSEGV);
    sigaddset(&set, SIGABRT);
    sigaddset(&set, SIGILL);
    const int result = pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    if (result != 0) {
        LOG(WARNING) << "Could not set thread sigmask: " << result;
    }
#endif
}

}  // namespace

class CrashReporterImpl : public CrashReporter {
  public:
    explicit CrashReporterImpl(size_t max_annotation_nodes = kMaxOwnedAnnotationNodes)
            : max_annotation_nodes_(max_annotation_nodes) {}

    [[noreturn]] void Die(std::string_view message) override {
        // Make sure we can actually register a crash on this thread.
        EnableSignalTermination();

        AddMessage(message);
        // this is the most cross-platform way of crashing
        // any other I know about has its flaws:
        //  - abort() isn't caught by Breakpad on Windows
        //  - null() may screw the callstack
        //  - explicit *null = 1 can be optimized out
        //  - requesting dump and exiting later has a very noticeable delay in
        //    between, so some real crash could stick in the middle
        volatile int* volatile ptr = nullptr;
        *ptr = 1313;  // die

#ifndef _WIN32
        raise(SIGABRT);
#endif
        abort();  // make compiler believe it doesn't return
    }

    void AddMessage(std::string_view message) override {
        absl::MutexLock lock(mutex_);
        annotation_log_ << message;
    }

    void SetAnnotation(std::string_view name, std::string_view value) override {
        absl::MutexLock lock(mutex_);

        const std::string_view key = name.substr(0, crashpad::Annotation::kNameMaxLength - 1);
        std::string_view next_value = value;
        if (next_value.size() > kMaxAnnotationBucket) {
            LOG(WARNING) << "Crash annotation '" << key << "' is very large (" << next_value.size()
                         << "), only " << kMaxAnnotationBucket << " bytes will be recorded, "
                         << (next_value.size() - kMaxAnnotationBucket) << " bytes are lost.";
            next_value = next_value.substr(0, kMaxAnnotationBucket);
        }

        auto it = active_annotations_.find(key);
        UpdatableStringAnnotation* previous = nullptr;
        if (it != active_annotations_.end()) {
            if (next_value.size() <= it->second->Capacity()) {
                it->second->Update(next_value);
                return;
            }
            if (owned_annotations_.size() >= max_annotation_nodes_) {
                LOG_FIRST_N(WARNING, 1)
                        << "Crash annotation node cap (" << max_annotation_nodes_
                        << ") reached; truncating '" << key << "' to existing bucket capacity ("
                        << it->second->Capacity() << " bytes).";
                it->second->Update(next_value.substr(0, it->second->Capacity()));
                return;
            }
            previous = it->second;
        } else if (owned_annotations_.size() >= max_annotation_nodes_) {
            LOG_FIRST_N(WARNING, 1) << "Crash annotation node cap (" << max_annotation_nodes_
                                    << ") reached; dropping new annotation '" << key << "'.";
            return;
        }

        // Register the new bucket in Crashpad's global AnnotationList (prepended at
        // the head) BEFORE retiring the outgrown bucket so concurrent crashes never
        // observe a gap where the key is completely absent.
        auto annotation = std::make_unique<UpdatableStringAnnotation>(
                key, BucketCapacityForSize(next_value.size()), next_value);
        if (previous != nullptr) {
            previous->Retire();
            it->second = annotation.get();
        } else {
            active_annotations_.emplace(key, annotation.get());
        }
        owned_annotations_.push_back(std::move(annotation));
    }

    std::optional<std::string> GetAttachedData(std::string_view name) const override {
        absl::MutexLock lock(mutex_);
        const std::string_view key = name.substr(0, crashpad::Annotation::kNameMaxLength - 1);
        auto it = active_annotations_.find(key);
        if (it == active_annotations_.end()) {
            return std::nullopt;
        }
        return std::string(it->second->ValueView());
    }

  private:
    const size_t max_annotation_nodes_;
    mutable absl::Mutex mutex_;
    absl::flat_hash_map<std::string, UpdatableStringAnnotation*> active_annotations_
            ABSL_GUARDED_BY(mutex_);
    // Crashpad permanently links registered Annotation pointers into its global
    // lock-free AnnotationList, so retired buckets must stay alive for the process lifetime.
    std::vector<std::unique_ptr<UpdatableStringAnnotation>> owned_annotations_
            ABSL_GUARDED_BY(mutex_);
    DefaultAnnotationStreambuf annotation_buf_ ABSL_GUARDED_BY(mutex_){"internal-msg"};
    std::ostream annotation_log_ ABSL_GUARDED_BY(mutex_){&annotation_buf_};
};

CrashReporter& CrashReporter::Get() {
    static CrashReporterImpl reporter;
    return reporter;
}

CrashReporter& CreateLeakedCrashReporterForTesting(size_t max_annotation_nodes) {
    static absl::NoDestructor<absl::Mutex> leaked_mutex;
    static absl::NoDestructor<std::vector<std::unique_ptr<CrashReporterImpl>>> leaked_instances;
    absl::MutexLock lock(*leaked_mutex);
    leaked_instances->push_back(std::make_unique<CrashReporterImpl>(max_annotation_nodes));
    return *leaked_instances->back();
}

HangDetector& CrashReporter::GetCrashingHangDetector() {
    static std::unique_ptr<HangDetector> hang_detector = HangDetector::Create(
            [](std::string_view message) { CrashReporter::Get().Die(message); },
            HangDetector::DefaultTiming());
    return *hang_detector;
}

}  // namespace android::crashreport
