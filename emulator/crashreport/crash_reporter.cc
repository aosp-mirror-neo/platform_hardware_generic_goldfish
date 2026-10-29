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

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "annotation_streambuf.h"
#include "client/annotation.h"
#include "simple_string_annotation.h"

#ifdef _WIN32
#include <io.h>
#else
#include <csignal>
#endif

namespace android::crashreport {

using DefaultStringAnnotation = crashpad::StringAnnotation<1024>;

namespace {
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
    void Die(std::string_view message) override {
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

    void AddMessage(std::string_view message) override { annotation_log_ << message; }

    void AttachData(std::string name, std::string data, bool /*replace*/) override {
        // Let's figure out how many bytes we need in our annotations.
        // We will bucketize by power of 2. We take the floor because
        // 2<<1 == 2^2, 2<<2 == 2^3, ..., etc..
        const int shift = floor(log2(data.size()));

        std::unique_ptr<crashpad::Annotation> annotation;
        // Sadly we have to do some pseudo switching to minimize the use of
        // space in our minidump.
        if (shift <= 5) {
            // 2<<5 == 2^6 == 64
            annotation = std::make_unique<SimpleStringAnnotation<2 << 5>>(name, data);
        }
        if (shift == 6) annotation = std::make_unique<SimpleStringAnnotation<2 << 6>>(name, data);
        if (shift == 7) annotation = std::make_unique<SimpleStringAnnotation<2 << 7>>(name, data);
        if (shift == 8) annotation = std::make_unique<SimpleStringAnnotation<2 << 8>>(name, data);
        if (shift == 9) annotation = std::make_unique<SimpleStringAnnotation<2 << 9>>(name, data);
        if (shift == 10) annotation = std::make_unique<SimpleStringAnnotation<2 << 10>>(name, data);
        if (shift == 11) annotation = std::make_unique<SimpleStringAnnotation<2 << 11>>(name, data);
        if (shift == 12) annotation = std::make_unique<SimpleStringAnnotation<2 << 12>>(name, data);
        if (shift >= 13) {
            annotation = std::make_unique<SimpleStringAnnotation<2 << 13>>(name, data);
            if (data.size() > 2 << 13) {
                LOG(WARNING) << "Crash annotation is very large (" << data.size()
                             << "), only 16384 bytes will be recorded, " << data.size() - (2 << 13)
                             << " bytes are lost.";
            }
        }

        annotations_.push_back(std::move(annotation));
    }

  private:
    // TODO nothing ever reads this.
    std::vector<std::unique_ptr<Annotation>> annotations_;
    DefaultAnnotationStreambuf annotation_buf_{"internal-msg"};
    std::ostream annotation_log_{&annotation_buf_};
};

CrashReporter& CrashReporter::Get() {
    static CrashReporterImpl reporter;
    return reporter;
}

HangDetector& CrashReporter::GetCrashingHangDetector() {
    static std::unique_ptr<HangDetector> hang_detector = HangDetector::Create(
            [](std::string_view message) {
                const std::string copy(message);
                CrashReporter::Get().Die(copy);
            },
            HangDetector::DefaultTiming());
    return *hang_detector;
}

}  // namespace android::crashreport
