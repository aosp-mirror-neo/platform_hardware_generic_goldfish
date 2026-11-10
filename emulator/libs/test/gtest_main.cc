// Copyright 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <vector>

#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/globals.h"
#include "absl/log/initialize.h"
#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

#include "android/base/bazel_info.h"
#include "android/base/system.h"

using android::base::Bazel;
using android::base::System;

ABSL_FLAG(bool, verbose_test, false,
          "Enable verbose test logging, equivalent to "
          "--vmodule=\"*=1\" --logtostderr=true --stderrthreshold=0.");

namespace {

void SetupSanitizers() {
#ifdef __linux__
    System::SetEnvironmentVariable("LLVM_SYMBOLIZER", Bazel::RunfilesPath("goldfish_build++toolchain+clang_linux_x64/bin/llvm-symbolizer"));
#elifdef __APPLE__
    System::SetEnvironmentVariable("LLVM_SYMBOLIZER", Bazel::RunfilesPath("goldfish_build++toolchain+clang_mac_all/bin/llvm-symbolizer"));
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    System::SetEnvironmentVariable("LSAN_OPTIONS", absl::StrCat("suppressions=", Bazel::RunfilesPath("goldfish+/emulator/libs/test/leak_suppressions.txt")));
    System::SetEnvironmentVariable("ASAN_OPTIONS", "detect_odr_violation=0");
#endif
#endif
}

} // namespace

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
// TSAN reads its options when the runtime starts, before main() runs, so
// setting TSAN_OPTIONS from main() has no effect. These hooks are read then.
extern "C" const char* __tsan_default_options() {
    return "second_deadlock_stack=1:die_after_fork=0";
}

extern "C" const char* __tsan_default_suppressions() {
    return
            // The prebuilt libc++ runtime is not TSAN-instrumented, so the
            // acquire/release on a shared_ptr control block's weak count
            // (__release_weak, lock) is invisible and freeing the block after
            // the last weak_ptr is misreported as a race.
            "race:__on_zero_shared_weak\n"
            // Lavapipe's (@mesa3d//:vulkan_lvp) lp_fence_signalled() polls
            // fence->count == fence->rank without locking fence->mutex before
            // lp_fence_destroy() destroys fence->mutex and fence->signalled,
            // and util_queue_thread_func() destroys a stack pthread_barrier_t
            // after pthread_barrier_wait() returns PTHREAD_BARRIER_SERIAL_THREAD
            // (b/573994192, b/579559759).
            "race:lp_fence_destroy\n"
            "race:util_queue_thread_func\n"
            "race:cuttlefish::NetworkService::isRadioOff\n";
}
#endif
#endif

int main(int argc, char* argv[]) {
    absl::InitializeSymbolizer(argv[0]);

    absl::InstallFailureSignalHandler({});

    if (Bazel::InBazel()) {
        Bazel::StoreCommandLineArgs(argc, argv);
    }

    SetupSanitizers();

    // Parse abseil logging configuration.
    std::vector<char*> positional_args;
    std::vector<absl::UnrecognizedFlag> unrecognized_flags;
    absl::ParseAbseilFlagsOnly(argc, argv, positional_args, unrecognized_flags);

    if (absl::GetFlag(FLAGS_verbose_test)) {
        absl::SetVLogLevel("*", 1);
        absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);
        absl::SetStderrThreshold(absl::LogSeverityAtLeast::kInfo);
    }

    absl::InitializeLog();

    // Note: Google Test will ignore all the abseil flags
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
