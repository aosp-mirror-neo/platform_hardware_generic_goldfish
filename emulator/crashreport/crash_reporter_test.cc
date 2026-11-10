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

#include "android/crashreport/crash_reporter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"

#include "client/annotation.h"
#include "client/annotation_list.h"
#include "crash_reporter_testing.h"

namespace android::crashreport {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

std::vector<std::string> GetActiveStringAnnotations(std::string_view target_name) {
    std::vector<std::string> result;
    auto* list = crashpad::AnnotationList::Get();
    if (list == nullptr) {
        return result;
    }
    for (crashpad::Annotation* annotation : *list) {
        if (!annotation->is_set() || annotation->type() != crashpad::Annotation::Type::kString) {
            continue;
        }
        if (std::string_view(annotation->name()) == target_name) {
            result.emplace_back(static_cast<const char*>(annotation->value()), annotation->size());
        }
    }
    return result;
}

TEST(CrashReporterTest, ReplaceInPlaceUpdatesSingleActiveAnnotation) {
    constexpr std::string_view kKey = "test_replace_in_place";
    auto& reporter = CrashReporter::Get();

    reporter.SetAnnotation(kKey, "LAUNCHER");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre("LAUNCHER"));

    reporter.SetAnnotation(kKey, "INIT_GENERAL");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre("INIT_GENERAL"));

    reporter.SetAnnotation(kKey, "RUNNING_GENERAL");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre("RUNNING_GENERAL"));

    reporter.SetAnnotation(kKey, "EXIT_GENERAL");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre("EXIT_GENERAL"));
}

TEST(CrashReporterTest, ReplaceAcrossBucketGrowthClearsOldBucketWithoutUaf) {
    constexpr std::string_view kKey = "test_replace_grow";
    auto& reporter = CrashReporter::Get();

    const std::string small(16, 'a');
    const std::string medium(200, 'b');
    const std::string large(1500, 'c');
    const std::string shrunk("final");

    reporter.SetAnnotation(kKey, small);
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre(small));

    reporter.SetAnnotation(kKey, medium);
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre(medium));

    reporter.SetAnnotation(kKey, large);
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre(large));

    reporter.SetAnnotation(kKey, shrunk);
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre(shrunk));

    // Walk the entire global AnnotationList to ensure all previously cleared
    // nodes remain valid live allocations.
    size_t total_nodes = 0;
    for (crashpad::Annotation* annotation : *crashpad::AnnotationList::Get()) {
        EXPECT_NE(annotation->name(), nullptr);
        ++total_nodes;
    }
    EXPECT_GE(total_nodes, 3U);
}

TEST(CrashReporterTest, EmptyAndOversizedValuesHandledSafely) {
    constexpr std::string_view kKey = "test_empty_and_oversized";
    auto& reporter = CrashReporter::Get();

    reporter.SetAnnotation(kKey, "");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), IsEmpty());

    reporter.SetAnnotation(kKey, "populated");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre("populated"));

    reporter.SetAnnotation(kKey, "");
    EXPECT_THAT(GetActiveStringAnnotations(kKey), IsEmpty());

    const std::string oversized(17000, 'x');
    reporter.SetAnnotation(kKey, oversized);
    EXPECT_THAT(GetActiveStringAnnotations(kKey), ElementsAre(std::string(16384, 'x')));

    // Overlong annotation names (>= kNameMaxLength = 256) are safely truncated and NUL-terminated.
    const std::string long_name(crashpad::Annotation::kNameMaxLength + 20, 'k');
    const std::string expected_truncated_name(crashpad::Annotation::kNameMaxLength - 1, 'k');
    reporter.SetAnnotation(long_name, "long-name-val");
    EXPECT_THAT(GetActiveStringAnnotations(expected_truncated_name), ElementsAre("long-name-val"));
    EXPECT_EQ(reporter.GetAttachedData(expected_truncated_name), "long-name-val");
    EXPECT_EQ(reporter.GetAttachedData(long_name), "long-name-val");
    reporter.SetAnnotation(absl::StrCat(expected_truncated_name, "_different_suffix"),
                           "updated-long-name-val");
    EXPECT_THAT(GetActiveStringAnnotations(expected_truncated_name),
                ElementsAre("updated-long-name-val"));
}

TEST(CrashReporterTest, ConcurrentSetAnnotationAndAddMessageIsRaceFree) {
    auto& reporter = CrashReporter::Get();
    std::atomic<bool> start{false};
    constexpr int kThreads = 4;
    constexpr int kIterations = 100;

    std::vector<std::thread> workers;
    workers.reserve(kThreads + 1);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&reporter, &start, t]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const std::string per_thread_key = absl::StrCat("test_concurrent_thread_", t);
            for (int i = 0; i < kIterations; ++i) {
                reporter.SetAnnotation(
                        "test_concurrent_shared",
                        absl::StrCat("thread=", t, ",iter=", i, ",",
                                     std::string(static_cast<size_t>(i % 8) * 40U, 'p')));
                reporter.SetAnnotation(per_thread_key, absl::StrCat("val_", i));
                reporter.AddMessage(absl::StrCat("[t", t, ":", i, "]"));
            }
        });
    }

    // Reader thread exercising AnnotationList + ScopedSpinGuard concurrently.
    workers.emplace_back([&start]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kIterations; ++i) {
            auto* list = crashpad::AnnotationList::Get();
            if (list == nullptr) {
                continue;
            }
            for (crashpad::Annotation* annotation : *list) {
                if (annotation->concurrent_access_guard_mode() ==
                    crashpad::Annotation::ConcurrentAccessGuardMode::kScopedSpinGuard) {
                    auto guard = annotation->TryCreateScopedSpinGuard(1'000'000);
                    if (guard.has_value() && annotation->is_set()) {
                        volatile char first = *static_cast<const char*>(annotation->value());
                        (void)first;
                    }
                }
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    EXPECT_EQ(GetActiveStringAnnotations("test_concurrent_shared").size(), 1U);
    for (int t = 0; t < kThreads; ++t) {
        const std::string per_thread_key = absl::StrCat("test_concurrent_thread_", t);
        EXPECT_THAT(GetActiveStringAnnotations(per_thread_key),
                    ElementsAre(absl::StrCat("val_", kIterations - 1)));
    }
}

TEST(CrashReporterTest, CapsAnnotationListNodesWithoutDroppingEarlierKeys) {
    static std::atomic<int> run_id{0};
    const std::string prefix = absl::StrCat("cap_", run_id.fetch_add(1), "_");
    auto& capped = CreateLeakedCrashReporterForTesting(/*max_annotation_nodes=*/4);

    capped.SetAnnotation(absl::StrCat(prefix, "session_id"), "sess-tail");
    capped.SetAnnotation(absl::StrCat(prefix, "process_type"), "qemu");
    capped.SetAnnotation(absl::StrCat(prefix, "growable"), "short");

    // Growing `growable` once allocates node #4 (retiring node #3).
    const std::string medium(100, 'm');
    capped.SetAnnotation(absl::StrCat(prefix, "growable"), medium);
    EXPECT_THAT(GetActiveStringAnnotations(absl::StrCat(prefix, "growable")), ElementsAre(medium));

    // At 4/4 owned nodes, growing `growable` beyond its 128-byte bucket truncates in-place
    // instead of allocating node #5, and brand-new keys are dropped cleanly.
    const std::string huge(300, 'h');
    capped.SetAnnotation(absl::StrCat(prefix, "growable"), huge);
    EXPECT_THAT(GetActiveStringAnnotations(absl::StrCat(prefix, "growable")),
                ElementsAre(std::string(128, 'h')));

    capped.SetAnnotation(absl::StrCat(prefix, "dropped_new_key"), "value");
    EXPECT_THAT(GetActiveStringAnnotations(absl::StrCat(prefix, "dropped_new_key")), IsEmpty());
    EXPECT_EQ(capped.GetAttachedData(absl::StrCat(prefix, "dropped_new_key")), std::nullopt);

    // Earlier startup keys remain intact and in-place updates still succeed.
    EXPECT_THAT(GetActiveStringAnnotations(absl::StrCat(prefix, "session_id")),
                ElementsAre("sess-tail"));
    capped.SetAnnotation(absl::StrCat(prefix, "process_type"), "launcher");
    EXPECT_THAT(GetActiveStringAnnotations(absl::StrCat(prefix, "process_type")),
                ElementsAre("launcher"));
}

}  // namespace
}  // namespace android::crashreport
