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

#include "goldfish/legacy/goldfish_gpu/goldfish_gpu.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/time.h"

#include "android/base/testing/test_system.h"
#include "android/base/testing/test_temp_dir.h"
#include "android/status/status_matcher_macros.h"
#include "gfxstream/host/features.h"
#include "goldfish/legacy/goldfish_gpu/goldfish_gpu_c_api.h"
#include "goldfish/legacy/goldfish_gpu/testing/mock_gfxstream.h"
#include "render-utils/RenderLib.h"
#include "render-utils/Renderer.h"

namespace goldfish::legacy {
namespace {

using ::absl_testing::StatusIs;
using ::testing::HasSubstr;

using ::goldfish::legacy::testing::MockRenderLib;
using ::goldfish::legacy::testing::MockRenderer;

class GoldfishGpuTest : public ::testing::Test {
  protected:
    void SetUp() override {
        renderer_ = std::make_shared<MockRenderer>();
        MockRenderer::last_flushed_display_id_ = -1;
        MockRenderLib::last_glsync_ = 0;
        MockRenderLib::last_thread_ = 0;
        MockRenderLib::last_timeline_ = 0;
        SetRenderLibFactoryForTesting([this]() {
            auto lib = std::make_unique<MockRenderLib>(renderer_);
            last_lib_ = lib.get();
            return lib;
        });
        // Every flow exercised here must be violation-free: fail (not just
        // log) on any invariant violation unless a test opts out.
        test_sys_.EnvSet("ANDROID_EMU_GOLDFISH_GPU_VERIFY", "enforce");
    }

    void TearDown() override {
        GetGoldfishGpu().Stop();
        SetRenderLibFactoryForTesting(nullptr);
    }

    android::base::TestSystem test_sys_{""};
    // Snapshot directory for tests that round-trip a meta blob: Load() refuses
    // a blob in in-memory mode (nothing to restore the renderer from).
    android::base::TestTempDir snap_dir_{"goldfish_gpu_fixture_snap"};
    std::shared_ptr<MockRenderer> renderer_;
    MockRenderLib* last_lib_ = nullptr;
};

TEST_F(GoldfishGpuTest, StartConfiguresRenderLibAndFeatures) {
    test_sys_.EnvSet("ANDROID_EMU_RENDERER", "lavapipe");

    uint32_t posted_w = 0;
    uint32_t posted_h = 0;
    std::vector<uint8_t> posted_pixels;

    GoldfishGpuConfig cfg;
    cfg.width = 4;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.renderer_features = "Vulkan:enabled;GlAsyncSwap:disabled";
    cfg.on_post = [&](uint32_t w, uint32_t h, const uint8_t* bgra) {
        posted_w = w;
        posted_h = h;
        posted_pixels.assign(bgra, bgra + static_cast<size_t>(w) * h * 4);
    };

    ASSERT_TRUE(GetGoldfishGpu().Start(cfg).ok());
    ASSERT_NE(last_lib_, nullptr);
    EXPECT_EQ(last_lib_->api_level, 36);
    EXPECT_EQ(last_lib_->gralloc, GOLDFISH_GRALLOC);
    EXPECT_EQ(last_lib_->selected_renderer, SELECTED_RENDERER_LAVAPIPE);
    EXPECT_EQ(last_lib_->width, 4);
    EXPECT_EQ(last_lib_->height, 2);
    EXPECT_FALSE(last_lib_->use_sub_window);
    EXPECT_TRUE(last_lib_->features.RefCountPipe.enabled());
    EXPECT_TRUE(last_lib_->features.VulkanAstcEmulation.enabled());
    EXPECT_TRUE(last_lib_->features.VulkanEtc2Emulation.enabled());
    EXPECT_FALSE(last_lib_->features.HostComposition.enabled());
    EXPECT_FALSE(last_lib_->features.GlAsyncSwap.enabled());
    // "Vulkan:enabled" from renderer_features is pinned back off: the
    // goldfish_pipe transport cannot snapshot with VulkanSnapshots on.
    EXPECT_FALSE(last_lib_->features.Vulkan.enabled());
    EXPECT_FALSE(last_lib_->features.VulkanSnapshots.enabled());
    EXPECT_EQ(test_sys_.EnvGet("ANDROID_EMU_HEADLESS"), "1");

    std::vector<uint8_t> frame(4ULL * 2ULL * 4ULL, 0x12);
    renderer_->SimulatePostFrame(4, 2, frame.data());
    EXPECT_EQ(posted_w, 4U);
    EXPECT_EQ(posted_h, 2U);
    ASSERT_EQ(posted_pixels.size(), 32U);
    // Alpha byte forced to 0xff.
    EXPECT_EQ(posted_pixels[3], 0xff);
    EXPECT_EQ(posted_pixels[0], 0x12);

    GetGoldfishGpu().FlushReadPixelPipeline();
    EXPECT_EQ(MockRenderer::last_flushed_display_id_, 0);
}

TEST_F(GoldfishGpuTest, InvalidRendererFeaturesReturnsError) {
    GoldfishGpuConfig cfg;
    cfg.width = 320;
    cfg.height = 240;
    cfg.guest_api_level = 36;
    cfg.renderer_features = "NotAFeature:enabled";
    absl::Status st = GetGoldfishGpu().Start(cfg);
    EXPECT_EQ(st.code(), absl::StatusCode::kInvalidArgument);
}

TEST_F(GoldfishGpuTest, GuestApiLevelIsRequiredAndForwardedToRenderLib) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 0;
    EXPECT_THAT(GetGoldfishGpu().Start(cfg),
                StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("guest_api_level")));
    EXPECT_EQ(last_lib_, nullptr);

    cfg.guest_api_level = 34;
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_NE(last_lib_, nullptr);
    EXPECT_EQ(last_lib_->api_level, 34);
    EXPECT_FALSE(last_lib_->features.Vulkan.enabled());
    EXPECT_FALSE(last_lib_->features.VulkanSnapshots.enabled());
    EXPECT_FALSE(last_lib_->features.GlDma.enabled());
    EXPECT_FALSE(last_lib_->features.GlDma2.enabled());
    EXPECT_TRUE(last_lib_->features.GlDirectMem.enabled());
}

TEST_F(GoldfishGpuTest, SyncAndMemoryCallbacks) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    ASSERT_TRUE(GetGoldfishGpu().Start(cfg).ok());

    uint64_t mapped_gpa = 0;
    uint64_t unmapped_gpa = 0;
    uint8_t dummy_host = 42;
    GetGoldfishGpu().SetMemoryMappingCallbacks(
            [&](uint64_t gpa, void* /*hva*/, uint64_t /*size*/) {
                mapped_gpa = gpa;
                return true;
            },
            [&](uint64_t gpa, uint64_t /*size*/) {
                unmapped_gpa = gpa;
                return true;
            },
            [&](uint64_t gpa) -> void* { return gpa == 0x1000 ? &dummy_host : nullptr; });

    last_lib_->vm_ops.map_user_memory(0x2000, &dummy_host, 4096);
    EXPECT_EQ(mapped_gpa, 0x2000U);
    last_lib_->vm_ops.unmap_user_memory(0x2000, 4096);
    EXPECT_EQ(unmapped_gpa, 0x2000U);
    EXPECT_EQ(last_lib_->dma_ops.get_host_addr(0x1000), &dummy_host);

    uint64_t inc_timeline = 0;
    uint32_t inc_count = 0;
    GetGoldfishGpu().SetTimelineIncCallback([&](uint64_t tl, uint32_t count) {
        inc_timeline = tl;
        inc_count = count;
    });
    EXPECT_TRUE(last_lib_->sync_device_exists());
    last_lib_->sync_timeline_inc(0x99, 3);
    EXPECT_EQ(inc_timeline, 0x99U);
    EXPECT_EQ(inc_count, 3U);

    GetGoldfishGpu().TriggerHostWait(11, 22, 33);
    EXPECT_EQ(MockRenderLib::last_glsync_, 11U);
    EXPECT_EQ(MockRenderLib::last_thread_, 22U);
    EXPECT_EQ(MockRenderLib::last_timeline_, 33U);

    uint64_t puid = GetGoldfishGpu().CreateProcessContext();
    GetGoldfishGpu().DestroyProcessContext(puid);
    WaitForProcessCleanupsForTesting();
    ASSERT_EQ(renderer_->created_puids.size(), 1U);
    EXPECT_EQ(renderer_->created_puids[0], puid);
    ASSERT_EQ(renderer_->cleaned_puids.size(), 1U);
    EXPECT_EQ(renderer_->cleaned_puids[0], puid);

    GetGoldfishGpu().ColorBufferLastRef(77);
    EXPECT_EQ(last_lib_->last_color_buffer_ref, 77U);
}

TEST_F(GoldfishGpuTest, AsgLifecycleAndSnapshot) {
    android::base::TestTempDir temp_dir("goldfish_gpu_snap");
    // Include a non-ASCII directory component per AGENTS.md.
    const std::filesystem::path snap_root = temp_dir.path / "测试_🚀_é";

    uint32_t posted_w = 0;
    uint32_t posted_h = 0;
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_root.string();
    cfg.on_post = [&](uint32_t w, uint32_t h, const uint8_t* /*bgra*/) {
        posted_w = w;
        posted_h = h;
    };
    ASSERT_TRUE(GetGoldfishGpu().Start(cfg).ok());

    ASSERT_OK_AND_ASSIGN(AsgBlob blob, GetGoldfishGpu().CreateAsgBlob(
                                               gfxstream::kAsgConsumerRingStorageSize + 65536));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(blob.external_addr) % 16384U, 0U);
    ASSERT_TRUE(GetGoldfishGpu().StartAsg(blob).ok());
    ASSERT_EQ(renderer_->asg_consumers.size(), 1U);
    EXPECT_EQ(renderer_->asg_consumers[0]->info.buffer, reinterpret_cast<char*>(blob.external_addr));
    EXPECT_EQ(renderer_->asg_consumers[0]->info.ring_storage,
              reinterpret_cast<char*>(blob.external_addr + 65536));
    EXPECT_EQ(renderer_->asg_consumers[0]->info.buffer_size, 65536U);

    // Verify PingAsg wakes OnUnavailableRead with kContinue.
    ASSERT_TRUE(GetGoldfishGpu().PingAsg(blob).ok());
    EXPECT_EQ(renderer_->asg_consumers[0]->info.callbacks.onUnavailableRead(),
              gfxstream::AsgOnUnavailableReadStatus::kContinue);

    // Open an OpenGLES pipe channel.
    absl::Notification read_done;
    std::vector<uint8_t> received;
    ASSERT_OK_AND_ASSIGN(const uint32_t pipe_id, GetGoldfishGpu().CreateOpenglesPipe(
                                                         [&](const uint8_t* data, size_t size) {
                                                             received.assign(data, data + size);
                                                             if (!read_done.HasBeenNotified()) {
                                                                 read_done.Notify();
                                                             }
                                                         },
                                                         /*on_writable=*/nullptr));
    const uint8_t payload[] = {1, 2, 3, 4};
    ASSERT_OK(GetGoldfishGpu().WriteOpenglesPipe(pipe_id, payload, sizeof(payload)));
    ASSERT_TRUE(read_done.WaitForNotificationWithTimeout(absl::Seconds(5)));
    EXPECT_EQ(received, std::vector<uint8_t>({1, 2, 3, 4}));

    // Post a frame and allocate a process context before saving snapshot.
    const uint64_t pre_save_puid = GetGoldfishGpu().CreateProcessContext();
    EXPECT_EQ(pre_save_puid, 1U);
    std::vector<uint8_t> frame(2ULL * 2ULL * 4ULL, 0x44);
    renderer_->SimulatePostFrame(2, 2, frame.data());

    // Snapshot Save sequence (including idempotent repeat calls across devices).
    ASSERT_TRUE(GetGoldfishGpu().Quiesce("snap_1").ok());
    ASSERT_TRUE(GetGoldfishGpu().Quiesce("snap_1").ok());
    EXPECT_EQ(renderer_->asg_consumers[0]->info.callbacks.onUnavailableRead(),
              gfxstream::AsgOnUnavailableReadStatus::kPauseForSnapshot);
    ASSERT_OK(GetGoldfishGpu().PreSave("snap_1"));
    ASSERT_OK(GetGoldfishGpu().PreSave("snap_1"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> meta, GetGoldfishGpu().Save("snap_1"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> repeated_meta, GetGoldfishGpu().Save("snap_1"));
    EXPECT_EQ(meta, repeated_meta);
    ASSERT_TRUE(GetGoldfishGpu().PostSave("snap_1").ok());
    EXPECT_EQ(renderer_->asg_consumers[0]->info.callbacks.onUnavailableRead(),
              gfxstream::AsgOnUnavailableReadStatus::kResumeAfterSnapshot);

    // Snapshot Load sequence (with RCU-deferred CloseAsg of old blob after RestoreAsgBlob).
    posted_w = 0;
    posted_h = 0;
    ASSERT_TRUE(GetGoldfishGpu().PreLoad("snap_1").ok());
    ASSERT_TRUE(GetGoldfishGpu().PreLoad("snap_1").ok());
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob restored_blob,
            GetGoldfishGpu().RestoreAsgBlob(blob.context_handle,
                                            gfxstream::kAsgConsumerRingStorageSize + 65536));
    // Deferred RCU retirement of the old blob must not evict the newly restored blob.
    GetGoldfishGpu().CloseAsg(blob);
    blob = restored_blob;
    ASSERT_OK(GetGoldfishGpu().Load("snap_1", meta.data(), meta.size()));
    EXPECT_EQ(renderer_->loaded_texture_magic, 0x54455837U);

    absl::Notification post_load_read;
    ASSERT_OK(GetGoldfishGpu().RestoreOpenglesPipe(
            pipe_id,
            [&](const uint8_t* /*data*/, size_t /*size*/) {
                if (!post_load_read.HasBeenNotified()) {
                    post_load_read.Notify();
                }
            },
            /*on_writable=*/nullptr));
    ASSERT_TRUE(GetGoldfishGpu().PostLoad("snap_1").ok());
    EXPECT_EQ(posted_w, 2U);
    EXPECT_EQ(posted_h, 2U);

    // Verify the texture loader stays alive after Load/PostLoad for lazy texture restore.
    {
        auto lazy_loader = renderer_->captured_texture_loader_wptr.lock();
        ASSERT_NE(lazy_loader, nullptr);
        uint32_t lazy_tex = 0;
        lazy_loader->loadTexture(7, [&lazy_tex](gfxstream::Stream* s) { lazy_tex = s->getBe32(); });
        EXPECT_EQ(lazy_tex, 0x54455837U);
    }

    ASSERT_OK(GetGoldfishGpu().WriteOpenglesPipe(pipe_id, payload, sizeof(payload)));
    ASSERT_TRUE(post_load_read.WaitForNotificationWithTimeout(absl::Seconds(5)));

    // Verify next_puid_ was restored from the snapshot and does not collide with pre_save_puid.
    const uint64_t post_load_puid = GetGoldfishGpu().CreateProcessContext();
    EXPECT_GT(post_load_puid, pre_save_puid);

    GetGoldfishGpu().CloseOpenglesPipe(pipe_id);
    GetGoldfishGpu().CloseAsg(blob);
}

// Exercises the C ABI used by goldfish_gpu_low_level.c: the quiesce section's pre_save
// (Quiesce + Save), the blob section's put (Save again, cached), goldfish_pipe's post_save
// (PostSave), then the load side in the same order.
TEST_F(GoldfishGpuTest, CApiSnapshotRoundTrip) {
    android::base::TestTempDir temp_dir("goldfish_gpu_c_api_snap");
    const std::filesystem::path snap_root = temp_dir.path / "c_api";

    uint32_t posted_w = 0;
    uint32_t posted_h = 0;
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_root.string();
    cfg.on_post = [&](uint32_t w, uint32_t h, const uint8_t* /*bgra*/) {
        posted_w = w;
        posted_h = h;
    };
    ASSERT_OK(GetGoldfishGpu().Start(cfg));

    ASSERT_OK_AND_ASSIGN(AsgBlob blob, GetGoldfishGpu().CreateAsgBlob(
                                               gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob));
    ASSERT_EQ(renderer_->asg_consumers.size(), 1U);
    ASSERT_OK_AND_ASSIGN(const uint32_t pipe_id,
                         GetGoldfishGpu().CreateOpenglesPipe([](const uint8_t*, size_t) {},
                                                             /*on_writable=*/nullptr));
    std::vector<uint8_t> frame(2ULL * 2ULL * 4ULL, 0x44);
    renderer_->SimulatePostFrame(2, 2, frame.data());

    // vm_stop quiesces; the "goldfish_gpu_begin" section's pre_save computes and caches the blob.
    ASSERT_TRUE(GoldfishGpuCoreQuiesce("c_api_snap"));
    EXPECT_EQ(renderer_->asg_consumers[0]->info.callbacks.onUnavailableRead(),
              gfxstream::AsgOnUnavailableReadStatus::kPauseForSnapshot);
    ASSERT_TRUE(GoldfishGpuCorePreSave("c_api_snap"));
    uint8_t* first = nullptr;
    size_t first_size = 0;
    ASSERT_TRUE(GoldfishGpuCoreSave("c_api_snap", &first, &first_size));
    ASSERT_NE(first, nullptr);
    ASSERT_GT(first_size, 0U);
    EXPECT_GE(GoldfishGpuCoreMaxSnapshotSize(), first_size);
    EXPECT_EQ(GoldfishGpuCoreMaxSnapshotSize(), static_cast<size_t>(256U << 20));

    // Blob section put: the cached blob is returned byte-for-byte.
    uint8_t* second = nullptr;
    size_t second_size = 0;
    ASSERT_TRUE(GoldfishGpuCoreSave("c_api_snap", &second, &second_size));
    ASSERT_EQ(second_size, first_size);
    EXPECT_EQ(std::memcmp(first, second, first_size), 0);
    const std::vector<uint8_t> meta(first, first + first_size);
    GoldfishGpuCoreFreeBuffer(second);
    GoldfishGpuCoreFreeBuffer(first);
    GoldfishGpuCoreFreeBuffer(nullptr);

    // "goldfish_gpu_end" section post_save resumes the renderer.
    ASSERT_TRUE(GoldfishGpuCorePostSave("c_api_snap"));
    EXPECT_EQ(renderer_->asg_consumers[0]->info.callbacks.onUnavailableRead(),
              gfxstream::AsgOnUnavailableReadStatus::kResumeAfterSnapshot);

    // Load side: "goldfish_gpu_begin" pre_load, goldfish_address_space restores the blob,
    // goldfish_pipe stages the opengles pipe callback (at MIG_PRI_DEFAULT), then the
    // "goldfish_gpu_end" section (at MIG_PRI_LOW) loads and its post_load resumes the renderer.
    posted_w = 0;
    posted_h = 0;
    ASSERT_TRUE(GoldfishGpuCorePreLoad("c_api_snap"));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob restored_blob,
            GetGoldfishGpu().RestoreAsgBlob(blob.context_handle,
                                            gfxstream::kAsgConsumerRingStorageSize + 65536));
    GetGoldfishGpu().CloseAsg(blob);
    blob = restored_blob;
    ASSERT_OK(GetGoldfishGpu().RestoreOpenglesPipe(
            pipe_id, [](const uint8_t*, size_t) {},
            /*on_writable=*/nullptr));
    ASSERT_TRUE(GoldfishGpuCoreLoad("c_api_snap", meta.data(), meta.size()));
    ASSERT_TRUE(GoldfishGpuCorePostLoad("c_api_snap"));
    EXPECT_EQ(posted_w, 2U);
    EXPECT_EQ(posted_h, 2U);

    // An unmatched pending restore (a pipe id not present in the GPU snapshot) is rejected by
    // Load().
    ASSERT_TRUE(GoldfishGpuCorePreLoad("c_api_snap"));
    ASSERT_OK(GetGoldfishGpu().RestoreOpenglesPipe(
            pipe_id + 999, [](const uint8_t*, size_t) {},
            /*on_writable=*/nullptr));
    EXPECT_FALSE(GoldfishGpuCoreLoad("c_api_snap", meta.data(), meta.size()));

    // A corrupt blob is rejected through the C ABI (and the renderer is resumed again).
    ASSERT_TRUE(GoldfishGpuCorePreLoad("c_api_snap"));
    std::vector<uint8_t> bad = meta;
    bad[0] ^= 0xFF;
    EXPECT_FALSE(GoldfishGpuCoreLoad("c_api_snap", bad.data(), bad.size()));
    EXPECT_FALSE(GoldfishGpuCoreLoad("c_api_snap", nullptr, 4));

    GetGoldfishGpu().CloseOpenglesPipe(pipe_id);
    GetGoldfishGpu().CloseAsg(blob);
}

TEST_F(GoldfishGpuTest, SnapshotOperationFinishedUndoesQuiesceWithoutPostSave) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(const uint32_t pipe_id,
                         GetGoldfishGpu().CreateOpenglesPipe([](const uint8_t*, size_t) {},
                                                             /*on_writable=*/nullptr));

    // A save where a later VMState section failed: Quiesce + PreSave + Save ran, PostSave never did.
    ASSERT_OK(GetGoldfishGpu().Quiesce("snap_a"));
    ASSERT_OK(GetGoldfishGpu().PreSave("snap_a"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> first_meta, GetGoldfishGpu().Save("snap_a"));
    EXPECT_TRUE(renderer_->paused_all);
    EXPECT_FALSE(renderer_->resumed_all);

    GetGoldfishGpu().OnSnapshotOperationFinished();
    EXPECT_TRUE(renderer_->resumed_all);
    // A second notification with nothing pending is a no-op.
    renderer_->resumed_all = false;
    GetGoldfishGpu().OnSnapshotOperationFinished();
    EXPECT_FALSE(renderer_->resumed_all);

    // The pipe is no longer paused: a write is delivered to the reader.
    absl::Notification read_after;
    const uint8_t payload[] = {9, 9};
    ASSERT_OK(GetGoldfishGpu().RestoreOpenglesPipe(
            pipe_id,
            [&](const uint8_t*, size_t) {
                if (!read_after.HasBeenNotified()) {
                    read_after.Notify();
                }
            },
            /*on_writable=*/nullptr));
    ASSERT_OK(GetGoldfishGpu().WriteOpenglesPipe(pipe_id, payload, sizeof(payload)));
    ASSERT_TRUE(read_after.WaitForNotificationWithTimeout(absl::Seconds(5)));

    // The next save must produce a fresh blob (one more pipe) instead of the
    // cached one from the failed save.
    ASSERT_OK_AND_ASSIGN(const uint32_t second_pipe,
                         GetGoldfishGpu().CreateOpenglesPipe([](const uint8_t*, size_t) {},
                                                             /*on_writable=*/nullptr));
    renderer_->paused_all = false;
    ASSERT_OK(GetGoldfishGpu().Quiesce("snap_b"));
    EXPECT_TRUE(renderer_->paused_all);
    ASSERT_OK(GetGoldfishGpu().PreSave("snap_b"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> second_meta, GetGoldfishGpu().Save("snap_b"));
    EXPECT_NE(first_meta, second_meta);
    EXPECT_EQ(second_meta.size(), first_meta.size() + 8);  // be32 id + be32 channel tag
    ASSERT_OK(GetGoldfishGpu().PostSave("snap_b"));

    // Same for a PreLoad whose Load/PostLoad never ran.
    renderer_->resumed_all = false;
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap_b"));
    EXPECT_TRUE(renderer_->pre_loaded);
    GetGoldfishGpu().OnSnapshotOperationFinished();
    EXPECT_TRUE(renderer_->resumed_all);
    GetGoldfishGpu().CloseOpenglesPipe(pipe_id);
    GetGoldfishGpu().CloseOpenglesPipe(second_pipe);
}

// Offsets of the count fields inside the meta blob written by Save() for the
// state built by SaveStateForBadInputTest(): one ASG consumer, one pipe.
// magic, version, asg_count, {handle, marker}: the pipe_count field.
constexpr size_t kMetaPipeCountOffset = 8 + 4 + 8;

void PutBe32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value) {
    (*bytes)[offset] = static_cast<uint8_t>(value >> 24);
    (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 16);
    (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 8);
    (*bytes)[offset + 3] = static_cast<uint8_t>(value);
}

// The meta blob format itself is exercised in gpu_snapshot_codec_test; this
// checks what the facade adds: a failed Load() resumes what PreLoad() paused,
// a rejected blob creates no render channel, and the intact blob still loads
// afterwards.
TEST_F(GoldfishGpuTest, FailedLoadResumesRendererAndIntactBlobStillLoads) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_dir_.path.string();
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob blob,
            GetGoldfishGpu().CreateAsgBlob(gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob));
    ASSERT_OK_AND_ASSIGN(const uint32_t pipe_id,
                         GetGoldfishGpu().CreateOpenglesPipe([](const uint8_t*, size_t) {},
                                                             /*on_writable=*/nullptr));
    ASSERT_OK(GetGoldfishGpu().Quiesce("snap"));
    ASSERT_OK(GetGoldfishGpu().PreSave("snap"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> meta, GetGoldfishGpu().Save("snap"));
    ASSERT_OK(GetGoldfishGpu().PostSave("snap"));

    const auto expect_rejected = [&](const std::vector<uint8_t>& bad, const std::string& what) {
        // PreLoad is idempotent per load attempt; a failed Load re-arms it.
        renderer_->resumed_all = false;
        ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
        const absl::Status st = GetGoldfishGpu().Load("snap", bad.data(), bad.size());
        EXPECT_THAT(st, StatusIs(absl::StatusCode::kDataLoss)) << what;
        // A failed Load resumes what PreLoad paused; PostLoad never runs and
        // OnSnapshotOperationFinished() must then have nothing left to do.
        EXPECT_TRUE(renderer_->resumed_all) << what;
        renderer_->resumed_all = false;
        GetGoldfishGpu().OnSnapshotOperationFinished();
        EXPECT_FALSE(renderer_->resumed_all) << what;
    };

    expect_rejected(std::vector<uint8_t>(meta.begin(), meta.begin() + 9), "truncated blob");

    std::vector<uint8_t> bad = meta;
    PutBe32(&bad, kMetaPipeCountOffset, 0xffffffff);
    const size_t channels_before = renderer_->channels.size();
    expect_rejected(bad, "pipe_count = 0xffffffff");
    // No render channel (and thus no render thread) was created for the bogus count.
    EXPECT_EQ(renderer_->channels.size(), channels_before);

    // The intact blob still loads after the rejected attempts.
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
    ASSERT_OK(GetGoldfishGpu().Load("snap", meta.data(), meta.size()));
    ASSERT_OK(GetGoldfishGpu().PostLoad("snap"));
    EXPECT_EQ(renderer_->asg_consumers.size(), 1U);
    GetGoldfishGpu().CloseOpenglesPipe(pipe_id);
    GetGoldfishGpu().CloseAsg(blob);
}

// Without a snapshot directory Save() only drives Renderer::save() into a
// discarded stream, so a meta blob cannot be loaded back: Load() must refuse
// (FAILED_PRECONDITION) before creating any ASG consumer or render channel
// against the unrestored renderer, and must resume what PreLoad() paused.
TEST_F(GoldfishGpuTest, InMemoryModeRefusesToLoadMetaBlob) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(const uint32_t pipe_id,
                         GetGoldfishGpu().CreateOpenglesPipe([](const uint8_t*, size_t) {},
                                                             /*on_writable=*/nullptr));
    ASSERT_OK(GetGoldfishGpu().Quiesce("snap"));
    ASSERT_OK(GetGoldfishGpu().PreSave("snap"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> meta, GetGoldfishGpu().Save("snap"));
    ASSERT_OK(GetGoldfishGpu().PostSave("snap"));

    renderer_->resumed_all = false;
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
    const size_t channels_before = renderer_->channels.size();
    EXPECT_THAT(GetGoldfishGpu().Load("snap", meta.data(), meta.size()),
                StatusIs(absl::StatusCode::kFailedPrecondition,
                         HasSubstr("no snapshot directory configured")));
    EXPECT_EQ(renderer_->channels.size(), channels_before);
    EXPECT_TRUE(renderer_->resumed_all);

    // An empty blob (no GPU state to restore) is still fine in-memory.
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
    ASSERT_OK(GetGoldfishGpu().Load("snap", nullptr, 0));
    ASSERT_OK(GetGoldfishGpu().PostLoad("snap"));
    GetGoldfishGpu().CloseOpenglesPipe(pipe_id);
}

TEST_F(GoldfishGpuTest, QuiesceBeforeSaveAndLoadAndPauseResume) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_dir_.path.string();
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob blob,
            GetGoldfishGpu().CreateAsgBlob(gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob));

    // 1. Plain VM pause/resume (vm_stop(RUN_STATE_PAUSED) -> vm_start()):
    //    GoldfishGpuCoreQuiesce(nullptr) pauses the renderer, and
    //    GoldfishGpuCoreOnSnapshotFinished() resumes it when no snapshot ran.
    renderer_->paused_all = false;
    renderer_->resumed_all = false;
    ASSERT_TRUE(GoldfishGpuCoreQuiesce(nullptr));
    EXPECT_TRUE(renderer_->paused_all);
    GoldfishGpuCoreOnSnapshotFinished();
    EXPECT_TRUE(renderer_->resumed_all);

    // 2. Save path: vm_stop(RUN_STATE_SAVE_VM) calls GoldfishGpuCoreQuiesce(nullptr)
    //    before the RAM pass, followed by GoldfishGpuCorePreSave("snap") from
    //    the "goldfish_gpu_begin" VMState section (idempotent quiesce + stream
    //    save) and GoldfishGpuCoreSave/PostSave from "goldfish_gpu_end".
    renderer_->paused_all = false;
    ASSERT_TRUE(GoldfishGpuCoreQuiesce(nullptr));
    EXPECT_TRUE(renderer_->paused_all);
    renderer_->paused_all = false;
    ASSERT_TRUE(GoldfishGpuCorePreSave("snap"));
    EXPECT_FALSE(renderer_->paused_all);
    uint8_t* meta = nullptr;
    size_t meta_size = 0;
    ASSERT_TRUE(GoldfishGpuCoreSave("snap", &meta, &meta_size));
    ASSERT_TRUE(GoldfishGpuCorePostSave("snap"));

    // 3. Load path: vm_stop(RUN_STATE_RESTORE_VM) calls GoldfishGpuCoreQuiesce(nullptr),
    //    and GoldfishGpuCorePreLoad("snap") unparks the paused threads before
    //    tearing them down in preLoad().
    renderer_->resumed_all = false;
    ASSERT_TRUE(GoldfishGpuCoreQuiesce(nullptr));
    ASSERT_TRUE(GoldfishGpuCorePreLoad("snap"));
    EXPECT_TRUE(renderer_->resumed_all);
    ASSERT_TRUE(GoldfishGpuCoreLoad("snap", meta, meta_size));
    ASSERT_TRUE(GoldfishGpuCorePostLoad("snap"));
    GoldfishGpuCoreFreeBuffer(meta);

    GetGoldfishGpu().CloseAsg(blob);
}

TEST_F(GoldfishGpuTest, CAbiLifecycleStartFlushAndStop) {
    char err_buf[128] = {0};
    // Guest API level 0 is rejected and populates err_buf.
    EXPECT_FALSE(GoldfishGpuCoreStart(/*width=*/2, /*height=*/2, /*guest_api_level=*/0, nullptr,
                                      nullptr, nullptr, nullptr, err_buf, sizeof(err_buf)));
    EXPECT_THAT(std::string_view(err_buf), HasSubstr("guest_api_level"));

    uint32_t posted_w = 0;
    uint32_t posted_h = 0;
    auto on_frame = +[](void* opaque, uint32_t w, uint32_t h, const uint8_t* /*bgra*/) {
        auto* dims = static_cast<uint32_t*>(opaque);
        dims[0] = w;
        dims[1] = h;
    };
    uint32_t dims[2] = {0, 0};
    ASSERT_TRUE(GoldfishGpuCoreStart(/*width=*/4, /*height=*/2, /*guest_api_level=*/36,
                                     "Vulkan:disabled", nullptr, on_frame, dims, err_buf,
                                     sizeof(err_buf)));
    std::vector<uint8_t> frame(4ULL * 2ULL * 4ULL, 0x11);
    renderer_->SimulatePostFrame(4, 2, frame.data());
    posted_w = dims[0];
    posted_h = dims[1];
    EXPECT_EQ(posted_w, 4U);
    EXPECT_EQ(posted_h, 2U);

    MockRenderer::last_flushed_display_id_ = -1;
    GoldfishGpuCoreFlush();
    EXPECT_EQ(MockRenderer::last_flushed_display_id_, 0);

    GoldfishGpuCoreStopForProcessExit();
    GoldfishGpuCoreStop();
}

TEST_F(GoldfishGpuTest, VerifyModeConfigAndEnvOverride) {
    test_sys_.EnvSet("ANDROID_EMU_GOLDFISH_GPU_VERIFY", "");
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    EXPECT_EQ(GetGoldfishGpu().GetVerifyMode(), GoldfishGpuVerifyMode::kLog);

    GetGoldfishGpu().SetVerifyMode(GoldfishGpuVerifyMode::kEnforce);
    EXPECT_EQ(GetGoldfishGpu().GetVerifyMode(), GoldfishGpuVerifyMode::kEnforce);

    // The environment overrides cfg.verify_mode (aliases: renderer_config_test).
    test_sys_.EnvSet("ANDROID_EMU_GOLDFISH_GPU_VERIFY", "off");
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    EXPECT_EQ(GetGoldfishGpu().GetVerifyMode(), GoldfishGpuVerifyMode::kOff);
}

// Production same-session reload: goldfish_address_space restores the blob
// for a handle while the entry from before the load is still registered, and
// the old blob is only retired (RCU-deferred CloseAsg) after the GPU Load.
TEST_F(GoldfishGpuTest, SameSessionReloadRetiresOldBlobAfterLoad) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_dir_.path.string();
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob old_blob,
            GetGoldfishGpu().CreateAsgBlob(gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(old_blob));

    ASSERT_OK(GetGoldfishGpu().Quiesce("snap"));
    ASSERT_OK(GetGoldfishGpu().PreSave("snap"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> meta, GetGoldfishGpu().Save("snap"));
    ASSERT_OK(GetGoldfishGpu().PostSave("snap"));

    // vm_stop quiesces, then the quiesce section's pre_load runs.
    ASSERT_TRUE(GoldfishGpuCoreQuiesce(nullptr));
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob new_blob,
            GetGoldfishGpu().RestoreAsgBlob(old_blob.context_handle,
                                            gfxstream::kAsgConsumerRingStorageSize + 65536));
    EXPECT_NE(new_blob.blob_id, old_blob.blob_id);
    ASSERT_OK(GetGoldfishGpu().Load("snap", meta.data(), meta.size()));
    ASSERT_EQ(renderer_->asg_consumers.size(), 1U);
    void* const restored_consumer = renderer_->asg_consumers[0].get();

    // The deferred retire of the old blob must not evict the restored one.
    GetGoldfishGpu().CloseAsg(old_blob);
    ASSERT_OK(GetGoldfishGpu().PostLoad("snap"));
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());
    ASSERT_EQ(renderer_->asg_consumers.size(), 1U);
    EXPECT_EQ(renderer_->asg_consumers[0].get(), restored_consumer);
    ASSERT_OK(GetGoldfishGpu().PingAsg(new_blob));

    GetGoldfishGpu().CloseAsg(new_blob);
    EXPECT_EQ(renderer_->DestroyCount(restored_consumer), 1);
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());
}

// An RCU-deferred CloseAsg() (BQL held) racing a PreLoad() that dropped the
// BQL must destroy the consumer exactly once. Interleaving forced: PreLoad()
// snapshots the entry and is inside addressSpaceGraphicsConsumerDestroy()
// (held open by `destroy_delay`) when CloseAsg() unlinks the same entry.
// Fails without the entry->mu guard on `consumer`: CloseAsg() also saw the
// live pointer and destroyed it a second time.
TEST_F(GoldfishGpuTest, ConcurrentDestroyAsgAndPreLoadDestroyConsumerOnce) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob blob,
            GetGoldfishGpu().CreateAsgBlob(gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob));
    ASSERT_EQ(renderer_->asg_consumers.size(), 1U);
    void* const consumer = renderer_->asg_consumers[0].get();

    absl::Notification destroy_started;
    {
        absl::MutexLock lock(&renderer_->destroy_mu);
        renderer_->on_destroy_started = &destroy_started;
    }
    renderer_->destroy_delay = absl::Milliseconds(200);

    std::thread pre_loader([&]() { EXPECT_OK(GetGoldfishGpu().PreLoad("snap")); });
    ASSERT_TRUE(destroy_started.WaitForNotificationWithTimeout(absl::Seconds(5)));
    // PreLoad() is now blocked inside the (first) destroy of `consumer`.
    GetGoldfishGpu().CloseAsg(blob);
    pre_loader.join();
    {
        absl::MutexLock lock(&renderer_->destroy_mu);
        renderer_->on_destroy_started = nullptr;
    }
    renderer_->destroy_delay = absl::ZeroDuration();
    EXPECT_EQ(renderer_->DestroyCount(consumer), 1);
    EXPECT_TRUE(renderer_->asg_consumers.empty());

    // The blob was unlinked while PreLoad() was in progress, so its deferred
    // destroy (a no-op: the consumer is gone) drains when the load is abandoned.
    GetGoldfishGpu().OnSnapshotOperationFinished();
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());
}

// While the GPU is quiesced the RCU-deferred CloseAsg() runs with the BQL
// held and must not wait for a parked render thread: the destroy is deferred
// to the end of the snapshot operation.
TEST_F(GoldfishGpuTest, DestroyAsgWhileQuiescedDefersConsumerDestroy) {
    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_dir_.path.string();
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob blob,
            GetGoldfishGpu().CreateAsgBlob(gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob));
    void* const consumer = renderer_->asg_consumers[0].get();

    // Save side: guest closes the ASG during the save; PostSave drains it.
    ASSERT_TRUE(GoldfishGpuCoreQuiesce(nullptr));
    GetGoldfishGpu().CloseAsg(blob);
    EXPECT_EQ(renderer_->DestroyCount(consumer), 0);
    ASSERT_OK(GetGoldfishGpu().PreSave("snap"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> meta, GetGoldfishGpu().Save("snap"));
    ASSERT_OK(GetGoldfishGpu().PostSave("snap"));
    EXPECT_EQ(renderer_->DestroyCount(consumer), 1);
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    // Load side: qemu_system_reset retires the blob after vm_stop quiesced the
    // GPU; the quiesce section's PreLoad drains it before preLoad().
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob blob2,
            GetGoldfishGpu().CreateAsgBlob(gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob2));
    void* const consumer2 = renderer_->asg_consumers[0].get();
    ASSERT_TRUE(GoldfishGpuCoreQuiesce(nullptr));
    GetGoldfishGpu().CloseAsg(blob2);
    EXPECT_EQ(renderer_->DestroyCount(consumer2), 0);
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
    EXPECT_EQ(renderer_->DestroyCount(consumer2), 1);
    EXPECT_TRUE(renderer_->asg_consumers.empty());
    // The snapshot had no ASG consumers left (saved after the close).
    ASSERT_OK(GetGoldfishGpu().Load("snap", meta.data(), meta.size()));
    ASSERT_OK(GetGoldfishGpu().PostLoad("snap"));
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());
}

TEST_F(GoldfishGpuTest, VerifySteadyStateAndSnapshotPhasesInEnforceMode) {
    android::base::TestTempDir temp_dir("goldfish_gpu_verify");
    const std::filesystem::path snap_root = temp_dir.path / "verify_snap";

    GoldfishGpuConfig cfg;
    cfg.width = 2;
    cfg.height = 2;
    cfg.guest_api_level = 36;
    cfg.snapshot_directory = snap_root.string();
    cfg.verify_mode = GoldfishGpuVerifyMode::kEnforce;
    ASSERT_OK(GetGoldfishGpu().Start(cfg));
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    ASSERT_OK_AND_ASSIGN(AsgBlob blob, GetGoldfishGpu().CreateAsgBlob(
                                               gfxstream::kAsgConsumerRingStorageSize + 65536));
    ASSERT_OK(GetGoldfishGpu().StartAsg(blob));
    ASSERT_OK_AND_ASSIGN(const uint32_t pipe_id,
                         GetGoldfishGpu().CreateOpenglesPipe([](const uint8_t*, size_t) {},
                                                             /*on_writable=*/nullptr));
    GetGoldfishGpu().ColorBufferRef(5);
    std::vector<uint8_t> frame(2ULL * 2ULL * 4ULL, 0x22);
    renderer_->SimulatePostFrame(2, 2, frame.data());
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    // Save() is a pure read of the blob cached by PreSave(): without PreSave it
    // is rejected in every verify mode (not just kEnforce), never writes the
    // streams, and leaves the GPU running.
    EXPECT_THAT(GetGoldfishGpu().Save("snap"),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("PreSave must run")));
    EXPECT_FALSE(renderer_->paused_all);
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    GetGoldfishGpu().SetVerifyMode(GoldfishGpuVerifyMode::kLog);
    EXPECT_THAT(GetGoldfishGpu().Save("snap"),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("PreSave must run")));
    EXPECT_FALSE(renderer_->paused_all);
    GetGoldfishGpu().SetVerifyMode(GoldfishGpuVerifyMode::kEnforce);
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    // Quiesce alone (kQuiesced) is not enough either: PreSave must have cached the blob.
    ASSERT_OK(GetGoldfishGpu().Quiesce("snap"));
    EXPECT_THAT(GetGoldfishGpu().VerifySteadyState(),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("LIFE-SNAPSHOT-FLAGS")));
    EXPECT_THAT(GetGoldfishGpu().Save("snap"),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("PreSave must run")));
    EXPECT_TRUE(renderer_->paused_all);

    // Proper Quiesce -> PreSave -> Save -> PostSave passes all invariant checks in kEnforce mode.
    ASSERT_OK(GetGoldfishGpu().PreSave("snap"));
    ASSERT_OK_AND_ASSIGN(const std::vector<uint8_t> meta, GetGoldfishGpu().Save("snap"));
    ASSERT_OK(GetGoldfishGpu().PostSave("snap"));
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    // Calling Load() without PreLoad() violates PRE_LOAD invariants in kEnforce mode.
    EXPECT_THAT(GetGoldfishGpu().Load("snap", meta.data(), meta.size()),
                StatusIs(absl::StatusCode::kFailedPrecondition, HasSubstr("PRE_LOAD")));
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    // Proper PreLoad -> RestoreAsgBlob + ColorBufferRef + RestoreOpenglesPipe (MIG_PRI_DEFAULT) ->
    // Load -> PostLoad (MIG_PRI_LOW) passes all checks in kEnforce mode.
    ASSERT_OK(GetGoldfishGpu().PreLoad("snap"));
    ASSERT_OK_AND_ASSIGN(
            const AsgBlob restored_blob,
            GetGoldfishGpu().RestoreAsgBlob(blob.context_handle,
                                            gfxstream::kAsgConsumerRingStorageSize + 65536));
    GetGoldfishGpu().CloseAsg(blob);
    blob = restored_blob;
    GetGoldfishGpu().ColorBufferRef(5);
    ASSERT_OK(GetGoldfishGpu().RestoreOpenglesPipe(
            pipe_id, [](const uint8_t*, size_t) {}, /*on_writable=*/nullptr));
    ASSERT_OK(GetGoldfishGpu().Load("snap", meta.data(), meta.size()));
    ASSERT_OK(GetGoldfishGpu().PostLoad("snap"));
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());

    GetGoldfishGpu().CloseOpenglesPipe(pipe_id);
    GetGoldfishGpu().CloseAsg(blob);
    ASSERT_OK(GetGoldfishGpu().VerifySteadyState());
}

}  // namespace
}  // namespace goldfish::legacy
