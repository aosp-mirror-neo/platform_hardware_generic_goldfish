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

#include "goldfish/legacy/goldfish_gpu/renderer/gfxstream_library_loader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "absl/status/status.h"

#include "android/base/testing/test_system.h"
#include "android/base/testing/test_temp_dir.h"

namespace goldfish::legacy {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

const std::filesystem::path kPlugin{"/opt/emu/lib64"};

TEST(GfxstreamLibraryLoaderTest, CandidatesPackagedDirFirstThenPluginDir) {
    EXPECT_THAT(GfxstreamLibraryCandidates("/sdk/emulator/lib64", kPlugin),
                ElementsAre(std::filesystem::path("/sdk/emulator/lib64") / GfxstreamLibraryName(),
                            kPlugin / GfxstreamLibraryName()));
}

TEST(GfxstreamLibraryLoaderTest, CandidatesSkipDuplicatePluginDir) {
    EXPECT_THAT(GfxstreamLibraryCandidates("/opt/emu/lib64", kPlugin),
                ElementsAre(kPlugin / GfxstreamLibraryName()));
}

TEST(GfxstreamLibraryLoaderTest, CandidatesFallBackToLoaderSearchPathOnlyWhenUnset) {
    EXPECT_THAT(GfxstreamLibraryCandidates("", kPlugin),
                ElementsAre(kPlugin / GfxstreamLibraryName(),
                            std::filesystem::path(GfxstreamLibraryName())));
    EXPECT_THAT(GfxstreamLibraryCandidates("", {}),
                ElementsAre(std::filesystem::path(GfxstreamLibraryName())));
}

TEST(GfxstreamLibraryLoaderTest, CandidatesKeepNonAsciiDirectory) {
    const std::filesystem::path dir = std::filesystem::path("/tmp/测试_🚀_é");
    const auto candidates = GfxstreamLibraryCandidates(dir.string(), {});
    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_EQ(candidates[0], dir / GfxstreamLibraryName());
}

TEST(GfxstreamLibraryLoaderTest, LoadReportsEveryCandidateWhenNothingLoads) {
    android::base::TestTempDir temp_dir("gfxstream_loader_missing");
    const std::filesystem::path dir = temp_dir.path / "测试_🚀_é";
    GfxstreamLibraryLoader loader;
    const absl::Status st = loader.Load(dir.string()).status();
    EXPECT_EQ(st.code(), absl::StatusCode::kNotFound) << st;
    EXPECT_THAT(std::string(st.message()), HasSubstr((dir / GfxstreamLibraryName()).string()));
    EXPECT_THAT(std::string(st.message()), HasSubstr("ANDROID_EMULATOR_LIBRARY_DIR="));
    EXPECT_TRUE(loader.LoadedPath().empty());
}

TEST(GfxstreamLibraryLoaderTest, LinuxEglDefaultsRespectExistingValues) {
    android::base::TestSystem sys("");
    sys.SetOsType(android::base::OsType::kLinux);
    ApplySoftwareEglDefaults(&sys, {});
#if defined(__linux__)
    EXPECT_EQ(sys.EnvGet("EGL_PLATFORM"), "surfaceless");
    EXPECT_EQ(sys.EnvGet("LIBGL_ALWAYS_SOFTWARE"), "1");
#else
    EXPECT_TRUE(sys.EnvGet("EGL_PLATFORM").empty());
#endif
    sys.EnvSet("EGL_PLATFORM", "x11");
    sys.EnvSet("LIBGL_ALWAYS_SOFTWARE", "0");
    ApplySoftwareEglDefaults(&sys, {});
    EXPECT_EQ(sys.EnvGet("EGL_PLATFORM"), "x11");
    EXPECT_EQ(sys.EnvGet("LIBGL_ALWAYS_SOFTWARE"), "0");
}

TEST(GfxstreamLibraryLoaderTest, LavapipeIcdDefaultsConfigureVulkanLoaderAndRespectOverrides) {
    android::base::TestTempDir temp_dir("gfxstream_loader_icd");
    const std::filesystem::path lib_dir = temp_dir.path / "测试_🚀_é" / "lib64";
    const std::filesystem::path vulkan_dir = lib_dir / "vulkan";
    std::filesystem::create_directories(vulkan_dir);
    const std::filesystem::path icd_path = vulkan_dir / "lvp_icd.json";
    {
        std::ofstream out(icd_path);
        out << "{}";
    }

    android::base::TestSystem sys("");
    sys.EnvSet("ANDROID_EMULATOR_LIBRARY_DIR", lib_dir.string());
    ApplySoftwareEglDefaults(&sys, {});
    EXPECT_EQ(sys.EnvGet("VK_DRIVER_FILES"), icd_path.string());
    EXPECT_EQ(sys.EnvGet("VK_ICD_FILENAMES"), icd_path.string());

    sys.EnvSet("VK_DRIVER_FILES", "/custom/icd.json");
    sys.EnvSet("VK_ICD_FILENAMES", "");
    ApplySoftwareEglDefaults(&sys, {});
    EXPECT_EQ(sys.EnvGet("VK_DRIVER_FILES"), "/custom/icd.json");
    EXPECT_TRUE(sys.EnvGet("VK_ICD_FILENAMES").empty());
}

}  // namespace
}  // namespace goldfish::legacy
