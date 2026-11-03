/* Copyright 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "virtualscene_image_provider.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include "android/base/bazel_info.h"
#include "android/goldfish/fake_hardware_config.h"
#include "goldfish/camera_image_providers/virtualscene/virtualscene.h"
#include "goldfish/sensors/physical_model.h"
#include "goldfish/system/testing/fake_clocks.h"

namespace goldfish::camera_image_providers::virtualscene {

namespace {
// Helper to compare CropRegions for more detailed test failures.
::testing::AssertionResult IsCropRegionEq(const VirtualsceneImageProvider::CropRegion& expected,
                                          const VirtualsceneImageProvider::CropRegion& actual) {
    if (expected.offset.width == actual.offset.width &&
        expected.offset.height == actual.offset.height &&
        expected.size.width == actual.size.width && expected.size.height == actual.size.height) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "Expected crop region: offset=(" << expected.offset.width << ","
           << expected.offset.height << "), size=(" << expected.size.width << ","
           << expected.size.height << "). Actual: offset=(" << actual.offset.width << ","
           << actual.offset.height << "), size=(" << actual.size.width << "," << actual.size.height
           << ")";
}
}  // namespace

TEST(VirtualsceneImageProvider, GetCropRegion_SameAspectRatio) {
    const CameraImageProviderRect src = {1920, 1080};  // 16:9
    const CameraImageProviderRect dst = {1280, 720};   // 16:9
    const auto region = VirtualsceneImageProvider::getCropRegion(src, dst);
    const VirtualsceneImageProvider::CropRegion expected = {{0, 0}, {1920, 1080}};
    EXPECT_TRUE(IsCropRegionEq(expected, region));
}

TEST(VirtualsceneImageProvider, GetCropRegion_Pillarbox) {
    const CameraImageProviderRect src = {1920, 1080};  // 16:9
    const CameraImageProviderRect dst = {1024, 768};   // 4:3
    // src aspect (1.777) > dst aspect (1.333) -> pillarbox
    const auto region = VirtualsceneImageProvider::getCropRegion(src, dst);
    // new width = 1080 * (1024/768) = 1440
    // offset x = (1920 - 1440) / 2 = 240
    const VirtualsceneImageProvider::CropRegion expected = {{240, 0}, {1440, 1080}};
    EXPECT_TRUE(IsCropRegionEq(expected, region));
}

TEST(VirtualsceneImageProvider, GetCropRegion_Letterbox) {
    const CameraImageProviderRect src = {1024, 768};   // 4:3
    const CameraImageProviderRect dst = {1920, 1080};  // 16:9
    // src aspect (1.333) < dst aspect (1.777) -> letterbox
    const auto region = VirtualsceneImageProvider::getCropRegion(src, dst);
    // new height = 1024 / (1920/1080) = 576
    // offset y = (768 - 576) / 2 = 96
    const VirtualsceneImageProvider::CropRegion expected = {{0, 96}, {1024, 576}};
    EXPECT_TRUE(IsCropRegionEq(expected, region));
}

TEST(VirtualsceneImageProvider, GetCropRegion_SrcSmallerThanDst) {
    const CameraImageProviderRect src = {640, 480};    // 4:3
    const CameraImageProviderRect dst = {1920, 1080};  // 16:9
    // src aspect (1.333) < dst aspect (1.777) -> letterbox
    const auto region = VirtualsceneImageProvider::getCropRegion(src, dst);
    // new height = 640 / (1920/1080) = 360
    // offset y = (480 - 360) / 2 = 60
    const VirtualsceneImageProvider::CropRegion expected = {{0, 60}, {640, 360}};
    EXPECT_TRUE(IsCropRegionEq(expected, region));
}

TEST(VirtualsceneImageProvider, FindCaptureStreamBuffers) {
    // Setup: create a vector of buffers to search through.
    std::vector<VirtualsceneImageProvider::CaptureStreamBuffers> bufs(3);
    bufs[0].id = 10;
    bufs[1].id = 20;
    bufs[2].id = 30;

    // Test case: Find an existing buffer in the middle.
    const auto* found =
            VirtualsceneImageProvider::findCaptureStreamBuffers(20, bufs.data(), bufs.size());
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, 20);
    EXPECT_EQ(found, &bufs[1]);

    // Test case: Search for a non-existent buffer.
    EXPECT_EQ(VirtualsceneImageProvider::findCaptureStreamBuffers(99, bufs.data(), bufs.size()),
              nullptr);

    // Test case: Find the first and last buffers.
    EXPECT_EQ(VirtualsceneImageProvider::findCaptureStreamBuffers(10, bufs.data(), bufs.size()),
              &bufs[0]);
    EXPECT_EQ(VirtualsceneImageProvider::findCaptureStreamBuffers(30, bufs.data(), bufs.size()),
              &bufs[2]);

    // Test case: Search in an empty array.
    EXPECT_EQ(VirtualsceneImageProvider::findCaptureStreamBuffers(10, nullptr, 0), nullptr);
}

TEST(VirtualsceneImageProvider, CreateStartCaptureStopMultiStream) {
    namespace fs = std::filesystem;

    const fs::path objRunfile = ::android::base::Bazel::RunfilesPath(
            "goldfish_prebuilts_common+/virtualscene/Toren1BD/Toren1BD.obj");
    ASSERT_TRUE(fs::exists(objRunfile));

    const fs::path tempRoot = fs::path(testing::TempDir()) / "virtualscene_ver_test";
    const fs::path targetDir = tempRoot / "data" / "virtualscene" / "Toren1BD";
    fs::remove_all(tempRoot);
    fs::create_directories(targetDir.parent_path());
    fs::create_directory_symlink(objRunfile.parent_path(), targetDir);

    const fs::path vulkanDir =
            ::android::base::Bazel::RunfilesPath("goldfish+/emulator/libs/ver/lib64/vulkan");
    ASSERT_TRUE(ver_initialize({tempRoot}, vulkanDir));

    system::testing::FakeVirtualClock virtualClock;
    sensors::PhysicalModel physicalModel(android::goldfish::FakeHardwareConfig::GetHwConfig(),
                                         virtualClock);
    VirtualsceneImageProvider::CreateArgs createArgs{
        .sceneConfig = VerSceneConfig(VerSceneConfig::Mode::Mesh3D, ""),
        .physicalModel = &physicalModel,
    };
    CameraImageProviderInfo info = {
        .createArg = &createArgs,
        .isBackFacing = 1,
    };
    std::unique_ptr<VirtualsceneImageProvider> provider(
            static_cast<VirtualsceneImageProvider*>(VirtualsceneImageProvider::create(info)));
    if (!provider) {
        ver_cleanup();
        fs::remove_all(tempRoot);
        GTEST_SKIP() << "Vulkan renderer unavailable in test environment";
    }

    EXPECT_FALSE(physicalModel.GetVirtualSceneCamera().IsConnected());

    const CameraImageProviderStreamConfig streams[] = {
        {
            .size = {640, 480},
            .id = 1,
            .format = imaging::ImageFormat::kRgba8888,
        },
        {
            .size = {320, 240},
            .id = 2,
            .format = imaging::ImageFormat::kYuV4203P,
        },
        {
            .size = {320, 180},
            .id = 3,
            .format = imaging::ImageFormat::kYuV420NV12,
        },
    };

    ASSERT_EQ(provider->start(streams, std::size(streams)), 0);
    EXPECT_TRUE(physicalModel.GetVirtualSceneCamera().IsConnected());
    EXPECT_EQ(provider->getScaledFramebufferSizeBytes(), 320u * 240u * 4u);

    // Reconfiguring streams while active should keep scene controls active without dropping to 0.
    ASSERT_EQ(provider->start(streams, std::size(streams)), 0);
    EXPECT_TRUE(physicalModel.GetVirtualSceneCamera().IsConnected());
    EXPECT_EQ(provider->getScaledFramebufferSizeBytes(), 320u * 240u * 4u);

    const CameraImageProviderStreamCaptureInfo captureInfos[] = {
        {.cfg = &streams[0]},
        {.cfg = &streams[1]},
        {.cfg = &streams[2]},
    };

    std::unordered_map<int32_t, std::vector<uint8_t>> capturedFrames;
    auto sink = [](void* opaque, const CameraImageProviderStreamCaptureInfo* sci,
                   const void* frameBits, size_t frameByteSize) -> int {
        auto* map = static_cast<std::unordered_map<int32_t, std::vector<uint8_t>>*>(opaque);
        const auto* bytes = static_cast<const uint8_t*>(frameBits);
        (*map)[sci->cfg->id].assign(bytes, bytes + frameByteSize);
        return 0;
    };

    CameraImageProviderCaptureOpts opts = {};
    ASSERT_EQ(provider->capture(opts, sink, &capturedFrames, captureInfos, std::size(captureInfos)),
              0);

    ASSERT_EQ(capturedFrames.size(), 3u);
    EXPECT_EQ(capturedFrames[1].size(), 640u * 480u * 4u);
    EXPECT_EQ(capturedFrames[2].size(), 320u * 240u * 3u / 2u);
    EXPECT_EQ(capturedFrames[3].size(), 320u * 180u * 3u / 2u);

    // Ensure rendered output is not all zeros.
    EXPECT_TRUE(std::any_of(capturedFrames[1].begin(), capturedFrames[1].end(),
                            [](uint8_t b) { return b != 0; }));
    EXPECT_TRUE(std::any_of(capturedFrames[2].begin(), capturedFrames[2].end(),
                            [](uint8_t b) { return b != 0; }));
    EXPECT_TRUE(std::any_of(capturedFrames[3].begin(), capturedFrames[3].end(),
                            [](uint8_t b) { return b != 0; }));

    provider->stop();
    EXPECT_EQ(provider->getScaledFramebufferSizeBytes(), 0u);
    EXPECT_FALSE(physicalModel.GetVirtualSceneCamera().IsConnected());
    provider.reset();
    ver_cleanup();
    fs::remove_all(tempRoot);
}

TEST(VirtualsceneImageProvider, ComputeViewProjectionMatrix_InitialPoseMatchesSceneCamera) {
    const glm::mat4 proj = VirtualsceneImageProvider::computeProjectionMatrix({640, 480});
    const glm::mat4 viewProj = VirtualsceneImageProvider::computeViewProjectionMatrix(
            proj, glm::vec3(0.0f), glm::vec3(-4.75f, 0.0f, 0.0f),
            /*isBackFacing=*/true, /*supportsPosition=*/true);

    constexpr float kExpected[16] = {
        0.000f,  -2.000f, 0.000f,  0.000f,   //
        -1.495f, 0.000f,  -0.083f, -0.083f,  //
        0.124f,  0.000f,  -0.999f, -0.997f,  //
        -0.045f, 0.120f,  -0.202f, -0.002f,
    };

    const float* actual = &viewProj[0][0];
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(actual[i], kExpected[i], 0.001f) << "index " << i;
    }
}

TEST(VirtualsceneImageProvider, ComputeViewProjectionMatrix_LandscapeRotationRotatesAxes) {
    const glm::mat4 proj = VirtualsceneImageProvider::computeProjectionMatrix({640, 480});
    // Rotating the device +90 degrees around Z (landscape) should cancel the +90 degree
    // sensor mount roll for the back-facing camera, aligning world +X with clip +X (1.5)
    // and world +Y with clip -Y (-2.0).
    const glm::mat4 viewProj = VirtualsceneImageProvider::computeViewProjectionMatrix(
            proj, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 90.0f),
            /*isBackFacing=*/true, /*supportsPosition=*/true);

    EXPECT_NEAR(viewProj[0][0], 1.5f, 0.001f);
    EXPECT_NEAR(viewProj[0][1], 0.0f, 0.001f);
    EXPECT_NEAR(viewProj[1][0], 0.0f, 0.001f);
    EXPECT_NEAR(viewProj[1][1], -2.0f, 0.001f);
}

TEST(VirtualsceneImageProvider, ComputeViewProjectionMatrix_FrontFacingFlipsViewDirection) {
    const glm::mat4 proj = VirtualsceneImageProvider::computeProjectionMatrix({640, 480});
    const glm::mat4 backViewProj = VirtualsceneImageProvider::computeViewProjectionMatrix(
            proj, glm::vec3(0.0f), glm::vec3(0.0f),
            /*isBackFacing=*/true, /*supportsPosition=*/true);
    const glm::mat4 frontViewProj = VirtualsceneImageProvider::computeViewProjectionMatrix(
            proj, glm::vec3(0.0f), glm::vec3(0.0f),
            /*isBackFacing=*/false, /*supportsPosition=*/true);

    // Front camera adds a 180-degree Y rotation, negating the X and Z columns of viewFromWorld.
    EXPECT_NEAR(frontViewProj[0][1], -backViewProj[0][1], 0.001f);
    EXPECT_NEAR(frontViewProj[1][0], backViewProj[1][0], 0.001f);
    EXPECT_NEAR(frontViewProj[2][2], -backViewProj[2][2], 0.001f);
    EXPECT_NEAR(frontViewProj[2][3], -backViewProj[2][3], 0.001f);
}

TEST(VirtualsceneImageProvider, GetImageProviderInfo_BundlesSceneConfigAndPhysicalModel) {
    auto* fakePhysicalModel = reinterpret_cast<sensors::PhysicalModel*>(0x1234);

    const VerSceneConfig configs[] = {
        VerSceneConfig(VerSceneConfig::Mode::Mesh3D, "scene.obj"),
        VerSceneConfig(VerSceneConfig::Mode::VideoFile, "clip.mp4"),
        VerSceneConfig(VerSceneConfig::Mode::ImageFile, "photo.png"),
        VerSceneConfig(VerSceneConfig::Mode::Webcam, "0"),
    };

    for (const auto& expectedConfig : configs) {
        CameraImageProviderInfo info = {};
        ASSERT_TRUE(getImageProviderInfo(&info, /*isBackFacing=*/true, fakePhysicalModel,
                                         expectedConfig));
        ASSERT_NE(info.vtbl, nullptr);
        ASSERT_NE(info.vtbl->createArgDctor, nullptr);
        ASSERT_NE(info.createArg, nullptr);
        EXPECT_EQ(info.isBackFacing, 1u);

        const auto* args =
                static_cast<const VirtualsceneImageProvider::CreateArgs*>(info.createArg);
        EXPECT_EQ(args->sceneConfig, expectedConfig);
        EXPECT_EQ(args->physicalModel, fakePhysicalModel);

        (info.vtbl->createArgDctor)(const_cast<void*>(info.createArg));
    }
}

TEST(VirtualsceneImageProvider, Create_2DModeLoadsWithoutRenderer) {
    CameraImageProviderInfo info = {};
    ASSERT_TRUE(getImageProviderInfo(&info, /*isBackFacing=*/false, /*physicalModel=*/nullptr,
                                     VerSceneConfig(VerSceneConfig::Mode::Color, "")));

    const CameraImageProviderVtbl* vtbl = nullptr;
    void* provider = (info.vtbl->create)(&info, &vtbl);
    (info.vtbl->createArgDctor)(const_cast<void*>(info.createArg));

    ASSERT_NE(provider, nullptr);
    ASSERT_NE(vtbl, nullptr);
    EXPECT_STREQ((vtbl->getId)(provider), "color:#000000");

    (vtbl->dctor)(provider);
}

TEST(VirtualsceneImageProvider, ComputePostRenderRotation_3DModesReturnZero) {
    const VerSceneConfig::Mode modes3d[] = {
        VerSceneConfig::Mode::Mesh3D,
        VerSceneConfig::Mode::Image360,
        VerSceneConfig::Mode::StreetView,
        VerSceneConfig::Mode::Video360,
    };
    const glm::vec3 portraitAccel(0.0f, 9.81f, 0.0f);
    for (const auto mode : modes3d) {
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(mode, 90, portraitAccel,
                                                                       /*isBackFacing=*/true),
                  0);
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(mode, 90, portraitAccel,
                                                                       /*isBackFacing=*/false),
                  0);
    }
}

TEST(VirtualsceneImageProvider, ComputePostRenderRotation_2DModesMatchDeviceAndSceneRotation) {
    const glm::vec3 portraitAccel(0.0f, 9.81f, 0.0f);
    const glm::vec3 reverseLandscapeAccel(-9.81f, 0.0f, 0.0f);
    const glm::vec3 reversePortraitAccel(0.0f, -9.81f, 0.0f);
    const glm::vec3 landscapeAccel(9.81f, 0.0f, 0.0f);

    const VerSceneConfig::Mode modes2d[] = {
        VerSceneConfig::Mode::ImageFile,
        VerSceneConfig::Mode::VideoFile,
        VerSceneConfig::Mode::Webcam,
        VerSceneConfig::Mode::Color,
    };

    for (const auto mode : modes2d) {
        // Portrait (coarseOrientation = 0), baseRotation = 0 -> 270 deg CW for both back & front
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(mode, 0, portraitAccel,
                                                                       /*isBackFacing=*/true),
                  270);
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(mode, 0, portraitAccel,
                                                                       /*isBackFacing=*/false),
                  270);

        // Landscape (coarseOrientation = 3), baseRotation = 0 -> 0 deg back, 180 deg front
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(mode, 0, landscapeAccel,
                                                                       /*isBackFacing=*/true),
                  0);
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(mode, 0, landscapeAccel,
                                                                       /*isBackFacing=*/false),
                  180);

        // Reverse Portrait (coarseOrientation = 2), baseRotation = 0 -> 90 deg for both
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                          mode, 0, reversePortraitAccel, /*isBackFacing=*/true),
                  90);
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                          mode, 0, reversePortraitAccel, /*isBackFacing=*/false),
                  90);

        // Reverse Landscape (coarseOrientation = 1), baseRotation = 0 -> 180 deg back, 0 deg front
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                          mode, 0, reverseLandscapeAccel, /*isBackFacing=*/true),
                  180);
        EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                          mode, 0, reverseLandscapeAccel, /*isBackFacing=*/false),
                  0);
    }

    // Scene base rotation (e.g. rotated video stream metadata) in Portrait:
    EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                      VerSceneConfig::Mode::VideoFile, 90, portraitAccel, /*isBackFacing=*/true),
              180);
    EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                      VerSceneConfig::Mode::VideoFile, 90, portraitAccel, /*isBackFacing=*/false),
              180);
    EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                      VerSceneConfig::Mode::VideoFile, 180, portraitAccel, /*isBackFacing=*/true),
              90);
    EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                      VerSceneConfig::Mode::VideoFile, 270, portraitAccel, /*isBackFacing=*/true),
              0);
    EXPECT_EQ(VirtualsceneImageProvider::computePostRenderRotation(
                      VerSceneConfig::Mode::VideoFile, -90, portraitAccel, /*isBackFacing=*/true),
              0);
}

TEST(VirtualsceneImageProvider, Capture2DImageFile_RotatesAccordingToDeviceOrientation) {
    namespace fs = std::filesystem;

    const fs::path posterRunfile = ::android::base::Bazel::RunfilesPath(
            "goldfish_prebuilts_common+/virtualscene/Toren1BD/poster.png");
    ASSERT_TRUE(fs::exists(posterRunfile));
    ASSERT_TRUE(ver_initialize({posterRunfile.parent_path()}));

    system::testing::FakeVirtualClock virtualClock;
    sensors::PhysicalModel physicalModel(android::goldfish::FakeHardwareConfig::GetHwConfig(),
                                         virtualClock);
    VirtualsceneImageProvider::CreateArgs createArgs{
        .sceneConfig = VerSceneConfig(VerSceneConfig::Mode::ImageFile, posterRunfile.string()),
        .physicalModel = &physicalModel,
    };
    CameraImageProviderInfo info = {
        .createArg = &createArgs,
        .isBackFacing = 1,
    };
    std::unique_ptr<VirtualsceneImageProvider> provider(
            static_cast<VirtualsceneImageProvider*>(VirtualsceneImageProvider::create(info)));
    ASSERT_NE(provider, nullptr);

    constexpr uint16_t kSize = 64;
    const CameraImageProviderStreamConfig stream = {
        .size = {kSize, kSize},
        .id = 1,
        .format = imaging::ImageFormat::kRgba8888,
    };
    const CameraImageProviderStreamCaptureInfo captureInfo = {.cfg = &stream};
    const CameraImageProviderCaptureOpts opts = {};

    std::vector<uint8_t> capturedFrame;
    auto sink = [](void* opaque, const CameraImageProviderStreamCaptureInfo*, const void* frameBits,
                   size_t frameByteSize) -> int {
        auto* vec = static_cast<std::vector<uint8_t>*>(opaque);
        const auto* bytes = static_cast<const uint8_t*>(frameBits);
        vec->assign(bytes, bytes + frameByteSize);
        return 0;
    };

    // 1. Capture in Landscape (accelerometer +X -> 0 deg post-render rotation for back camera).
    physicalModel.OverrideAccelerometer(glm::vec3(9.81f, 0.0f, 0.0f));
    ASSERT_EQ(provider->start(&stream, 1), 0);
    ASSERT_EQ(provider->capture(opts, sink, &capturedFrame, &captureInfo, 1), 0);
    const std::vector<uint8_t> landscapeFrame = capturedFrame;
    ASSERT_EQ(landscapeFrame.size(), static_cast<size_t>(kSize) * kSize * 4);

    // 2. Capture in Portrait (accelerometer +Y -> 270 deg CW post-render rotation for back camera).
    physicalModel.OverrideAccelerometer(glm::vec3(0.0f, 9.81f, 0.0f));
    ASSERT_EQ(provider->capture(opts, sink, &capturedFrame, &captureInfo, 1), 0);
    const std::vector<uint8_t> portraitFrame = capturedFrame;
    ASSERT_EQ(portraitFrame.size(), landscapeFrame.size());

    // Verify that portraitFrame is landscapeFrame rotated 270 degrees clockwise:
    // source pixel (x, y) maps to destination pixel (y, kSize - 1 - x).
    bool hasDifferenceWithoutRotation = false;
    for (uint16_t y = 0; y < kSize; ++y) {
        for (uint16_t x = 0; x < kSize; ++x) {
            const size_t srcIdx = (static_cast<size_t>(y) * kSize + x) * 4;
            const uint16_t dstX = y;
            const uint16_t dstY = kSize - 1 - x;
            const size_t dstIdx = (static_cast<size_t>(dstY) * kSize + dstX) * 4;
            for (size_t c = 0; c < 4; ++c) {
                EXPECT_EQ(portraitFrame[dstIdx + c], landscapeFrame[srcIdx + c])
                        << "Mismatch at (" << x << ", " << y << ") channel " << c;
                if (portraitFrame[srcIdx + c] != landscapeFrame[srcIdx + c]) {
                    hasDifferenceWithoutRotation = true;
                }
            }
        }
    }
    EXPECT_TRUE(hasDifferenceWithoutRotation);

    provider->stop();
    provider.reset();
    ver_cleanup();
}

TEST(VirtualsceneImageProvider, SharesScaledFramebufferAcrossMultiStreamSession) {
    namespace fs = std::filesystem;

    const fs::path posterRunfile = ::android::base::Bazel::RunfilesPath(
            "goldfish_prebuilts_common+/virtualscene/Toren1BD/poster.png");
    ASSERT_TRUE(fs::exists(posterRunfile));
    ASSERT_TRUE(ver_initialize({posterRunfile.parent_path()}));

    system::testing::FakeVirtualClock virtualClock;
    sensors::PhysicalModel physicalModel(android::goldfish::FakeHardwareConfig::GetHwConfig(),
                                         virtualClock);
    physicalModel.OverrideAccelerometer(glm::vec3(9.81f, 0.0f, 0.0f));
    VirtualsceneImageProvider::CreateArgs createArgs{
        .sceneConfig = VerSceneConfig(VerSceneConfig::Mode::ImageFile, posterRunfile.string()),
        .physicalModel = &physicalModel,
    };
    CameraImageProviderInfo info = {
        .createArg = &createArgs,
        .isBackFacing = 1,
    };
    std::unique_ptr<VirtualsceneImageProvider> provider(
            static_cast<VirtualsceneImageProvider*>(VirtualsceneImageProvider::create(info)));
    ASSERT_NE(provider, nullptr);

    const CameraImageProviderStreamConfig streams[] = {
        {
            .size = {1280, 960},
            .id = 1,
            .format = imaging::ImageFormat::kRgba8888,
        },
        {
            .size = {1280, 720},
            .id = 2,
            .format = imaging::ImageFormat::kRgba8888,
        },
        {
            .size = {1280, 720},
            .id = 3,
            .format = imaging::ImageFormat::kYuV420NV12,
        },
        {
            .size = {640, 480},
            .id = 4,
            .format = imaging::ImageFormat::kYuV4203P,
        },
    };

    // Single max-resolution stream matches RenderOutputConfig and needs no scaling buffer.
    ASSERT_EQ(provider->start(&streams[0], 1), 0);
    EXPECT_EQ(provider->getScaledFramebufferSizeBytes(), 0u);

    auto sink = [](void* opaque, const CameraImageProviderStreamCaptureInfo* sci,
                   const void* frameBits, size_t frameByteSize) -> int {
        auto* map = static_cast<std::unordered_map<int32_t, std::vector<uint8_t>>*>(opaque);
        const auto* bytes = static_cast<const uint8_t*>(frameBits);
        (*map)[sci->cfg->id].assign(bytes, bytes + frameByteSize);
        return 0;
    };
    const CameraImageProviderCaptureOpts opts = {};

    // Capture reference outputs when each non-max stream is the only scaled stream in the session.
    std::unordered_map<int32_t, std::vector<uint8_t>> referenceFrames;
    for (size_t i = 1; i < std::size(streams); ++i) {
        const CameraImageProviderStreamConfig pairConfigs[] = {streams[0], streams[i]};
        const CameraImageProviderStreamCaptureInfo singleCaptureInfo = {.cfg = &streams[i]};
        ASSERT_EQ(provider->start(pairConfigs, std::size(pairConfigs)), 0);
        ASSERT_EQ(provider->capture(opts, sink, &referenceFrames, &singleCaptureInfo, 1), 0);
    }

    // Multi-stream session with 3 scaled streams (1280x720 RGBA, 1280x720 NV12, 640x480 I420)
    // shares a single scaling buffer sized to the largest scaled stream (1280x720x4 = 3,686,400 B)
    // instead of allocating 3,686,400 + 3,686,400 + 1,228,800 = 8,601,600 B across streams.
    constexpr size_t kMaxScaledStreamBytes = 1280u * 720u * 4u;
    constexpr size_t kPerStreamScaledBytesSum = (1280u * 720u * 4u) * 2u + (640u * 480u * 4u);
    ASSERT_EQ(provider->start(streams, std::size(streams)), 0);
    EXPECT_EQ(provider->getScaledFramebufferSizeBytes(), kMaxScaledStreamBytes);
    EXPECT_LT(provider->getScaledFramebufferSizeBytes(), kPerStreamScaledBytesSum);

    const CameraImageProviderStreamCaptureInfo captureInfos[] = {
        {.cfg = &streams[0]},
        {.cfg = &streams[1]},
        {.cfg = &streams[2]},
        {.cfg = &streams[3]},
    };
    std::unordered_map<int32_t, std::vector<uint8_t>> multiStreamFrames;
    ASSERT_EQ(provider->capture(opts, sink, &multiStreamFrames, captureInfos,
                                std::size(captureInfos)),
              0);

    ASSERT_EQ(multiStreamFrames.size(), 4u);
    EXPECT_EQ(multiStreamFrames[1].size(), 1280u * 960u * 4u);
    EXPECT_EQ(multiStreamFrames[2], referenceFrames[2]);
    EXPECT_EQ(multiStreamFrames[3], referenceFrames[3]);
    EXPECT_EQ(multiStreamFrames[4], referenceFrames[4]);

    provider->stop();
    EXPECT_EQ(provider->getScaledFramebufferSizeBytes(), 0u);
    provider.reset();
    ver_cleanup();
}

}  // namespace goldfish::camera_image_providers::virtualscene
