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

#include "goldfish/legacy/goldfish_gpu/snapshot/gpu_snapshot_files.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "absl/status/status.h"

#include "android/base/testing/test_temp_dir.h"
#include "android/status/status_matcher_macros.h"
#include "goldfish/legacy/goldfish_gpu/testing/mock_gfxstream.h"

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::goldfish::legacy::testing::MockRenderer;
using ::testing::HasSubstr;

TEST(GpuSnapshotFilesTest, InMemoryModeDrivesTheRendererWithoutWriting) {
    GpuSnapshotFiles files{std::filesystem::path{}};
    EXPECT_FALSE(files.IsOnDisk("snap"));
    MockRenderer renderer;
    ASSERT_OK(files.Save("snap", &renderer));
    EXPECT_EQ(renderer.save_calls, 1) << "Renderer::save() must still run for its bookkeeping";
    ASSERT_OK_AND_ASSIGN(gfxstream::ITextureLoaderPtr loader, files.Load("snap", &renderer));
    EXPECT_EQ(loader, nullptr);
    EXPECT_EQ(renderer.loaded_texture_magic, 0U) << "Renderer::load() is not run in-memory";
    EXPECT_OK(files.ArtifactsPresent("snap"));

    // A configured directory with an unnamed snapshot is in-memory too.
    android::base::TestTempDir temp_dir("gpu_snapshot_files_unnamed");
    GpuSnapshotFiles named{temp_dir.path};
    EXPECT_FALSE(named.IsOnDisk(""));
    ASSERT_OK(named.Save("", &renderer));
    EXPECT_EQ(renderer.save_calls, 2);
    EXPECT_TRUE(std::filesystem::is_empty(temp_dir.path));
}

TEST(GpuSnapshotFilesTest, SaveThenLoadRoundTripsThroughNonAsciiDirectory) {
    android::base::TestTempDir temp_dir("gpu_snapshot_files_rt");
    const std::filesystem::path root = temp_dir.path / "测试_🚀_é";
    GpuSnapshotFiles files{root};
    EXPECT_TRUE(files.IsOnDisk("snap"));

    MockRenderer renderer;
    EXPECT_THAT(files.ArtifactsPresent("snap"), StatusIs(absl::StatusCode::kDataLoss));
    ASSERT_OK(files.Save("snap", &renderer));
    EXPECT_OK(files.ArtifactsPresent("snap"));
    EXPECT_TRUE(std::filesystem::is_regular_file(files.Directory("snap") /
                                                 GpuSnapshotFiles::kRenderStreamFileName));
    EXPECT_TRUE(std::filesystem::is_regular_file(files.Directory("snap") /
                                                 GpuSnapshotFiles::kTextureFileName));

    ASSERT_OK_AND_ASSIGN(gfxstream::ITextureLoaderPtr loader, files.Load("snap", &renderer));
    ASSERT_NE(loader, nullptr);
    EXPECT_EQ(renderer.loaded_texture_magic, 0x54455837U);
    EXPECT_EQ(renderer.captured_texture_loader_wptr.lock(), loader);
}

TEST(GpuSnapshotFilesTest, LoadOfMissingSnapshotIsDataLoss) {
    android::base::TestTempDir temp_dir("gpu_snapshot_files_missing");
    GpuSnapshotFiles files{temp_dir.path};
    MockRenderer renderer;
    EXPECT_THAT(files.Load("nope", &renderer).status(),
                StatusIs(absl::StatusCode::kDataLoss, HasSubstr("gfxstream_render.bin")));
}

TEST(GpuSnapshotFilesTest, TruncatedTextureFileIsRejectedBeforeRendererLoad) {
    android::base::TestTempDir temp_dir("gpu_snapshot_files_trunc");
    GpuSnapshotFiles files{temp_dir.path};
    MockRenderer renderer;
    ASSERT_OK(files.Save("snap", &renderer));
    {
        std::ofstream trunc(files.Directory("snap") / GpuSnapshotFiles::kTextureFileName,
                            std::ios::binary | std::ios::trunc);
        trunc << "xx";
    }
    renderer.loaded_texture_magic = 0;
    EXPECT_THAT(files.Load("snap", &renderer).status(), StatusIs(absl::StatusCode::kDataLoss));
    // Renderer::load() never ran: gfxstream would have aborted on a bad texture file.
    EXPECT_EQ(renderer.loaded_texture_magic, 0U);
}

TEST(GpuSnapshotFilesTest, SaveFailsWhenTheDirectoryCannotBeCreated) {
    android::base::TestTempDir temp_dir("gpu_snapshot_files_nodir");
    const std::filesystem::path blocker = temp_dir.path / "file";
    { std::ofstream(blocker) << "not a directory"; }
    GpuSnapshotFiles files{blocker};
    MockRenderer renderer;
    EXPECT_THAT(files.Save("snap", &renderer), StatusIs(absl::StatusCode::kInternal,
                                                        HasSubstr("snapshot directory")));
}

TEST(GpuSnapshotFilesTest, FailedSaveLeavesNoArtifactsBehind) {
    android::base::TestTempDir temp_dir("gpu_snapshot_files_failed_save");
    GpuSnapshotFiles files{temp_dir.path};
    MockRenderer renderer;
    renderer.fail_save = true;
    EXPECT_THAT(files.Save("snap", &renderer),
                StatusIs(absl::StatusCode::kInternal, HasSubstr("Renderer::save() failed")));
    // Renderer::save() had already written both files; they must be gone so
    // ArtifactsPresent() does not report a half-written snapshot as complete.
    EXPECT_FALSE(std::filesystem::exists(files.Directory("snap") /
                                         GpuSnapshotFiles::kRenderStreamFileName));
    EXPECT_FALSE(std::filesystem::exists(files.Directory("snap") /
                                         GpuSnapshotFiles::kTextureFileName));
    EXPECT_THAT(files.ArtifactsPresent("snap"), StatusIs(absl::StatusCode::kDataLoss));

    // A later good save of the same name succeeds.
    renderer.fail_save = false;
    ASSERT_OK(files.Save("snap", &renderer));
    EXPECT_OK(files.ArtifactsPresent("snap"));
}

TEST(GpuSnapshotFilesTest, NamesThatEscapeTheDirectoryAreRejected) {
    android::base::TestTempDir temp_dir("gpu_snapshot_files_names");
    const std::filesystem::path root = temp_dir.path / "snapshots";
    GpuSnapshotFiles files{root};
    MockRenderer renderer;
    for (const char* bad : {"..", ".", "../escape", "a/b", "a\\b", "/abs", "C:x", "x:stream"}) {
        EXPECT_THAT(files.Save(bad, &renderer), StatusIs(absl::StatusCode::kInvalidArgument))
                << bad;
        EXPECT_THAT(files.Load(bad, &renderer).status(),
                    StatusIs(absl::StatusCode::kInvalidArgument))
                << bad;
        EXPECT_THAT(files.ArtifactsPresent(bad), StatusIs(absl::StatusCode::kInvalidArgument))
                << bad;
    }
    // Nothing was created outside (or inside) the root.
    EXPECT_FALSE(std::filesystem::exists(temp_dir.path / "escape"));
    EXPECT_FALSE(std::filesystem::exists(root));
    // Plain names, including non-ASCII ones, are fine.
    EXPECT_OK(GpuSnapshotFiles::ValidateName("snap_1"));
    EXPECT_OK(GpuSnapshotFiles::ValidateName("快照_🚀"));
}

}  // namespace
}  // namespace goldfish::legacy
