/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "Renderer.h"
#include "TestUtils.h"
#include "android/base/system.h"

using namespace android::base;
using namespace android::ver;

namespace {

class SceneRenderingTest : public ::testing::TestWithParam<std::tuple<std::string, std::string>> {
  protected:
    void SetUp() override {
        setupTestLibraryPaths();
        const auto& [backend, modeName] = GetParam();
        System::Get()->EnvSet("VER_RENDERER_BACKEND", backend);

        std::filesystem::path testProgDir(getTestProgramDirectory());
        std::vector<std::filesystem::path> resourcePaths;
        resourcePaths.push_back(testProgDir / "resources");
        resourcePaths.push_back(testProgDir);

        std::filesystem::path vulkanDir = testProgDir / "lib64" / "vulkan";

        // Enable flags for upcoming features
        System::Get()->EnvSet("ANDROID_EMU_ENABLE_STREETVIEW", "1");
        System::Get()->EnvSet("ANDROID_EMU_ENABLE_VIDEO360", "1");

        // Set test parameters for streetview mode to avoid dependency on
        // internet connection
        System::Get()->EnvSet("ANDROID_EMU_STREETVIEW_IMAGE_PATH", "testdata/streetview.jpg");

        bool success = ver_initialize(resourcePaths, vulkanDir);
        ASSERT_TRUE(success) << "Failed to initialize VER with backend: " << backend;
    }

    void TearDown() override {
        ver_cleanup();
        System::Get()->EnvSet("VER_RENDERER_BACKEND", "");
    }
};

TEST_P(SceneRenderingTest, RenderSceneMode) {
    const auto& [backend, modeName] = GetParam();
    VerSceneConfig::Mode mode = VerSceneConfig::modeFromString(modeName);

    // Fail on invalid modes
    ASSERT_NE(mode, VerSceneConfig::Mode::Unknown);

    VerSceneConfig config(mode, VerSceneConfig::defaultArgumentForMode(mode));
    VerSceneHandle scene = ver_create_scene(config);
    ASSERT_NE(scene, (VerSceneHandle)VER_INVALID_HANDLE)
            << "Failed to create scene for mode: " << modeName;

    ver_scene_load_user_resources(scene, []() {});

    // Enforce a static time for deterministic updates
    ver_scene_set_frame_time_us(scene, 0);
    ver_scene_update(scene, false);

    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int width = 640;
    int height = 480;
    ASSERT_TRUE(ver_render_view_set_dimensions(view, width, height));

    // View projection from the initial SceneCamera values
    const float viewProj[16] = {1.500f,  0.000f,  0.000f,  0.000f,   //
                                0.000f,  -1.993f, -0.083f, -0.083f,  //
                                -0.000f, 0.166f,  -0.999f, -0.997f,  //
                                -0.090f, -0.060f, -0.202f, -0.002f};

    ver_render_view_set_view_projection(view, viewProj);

    bool rendered = ver_render_view(scene, view, []() {}, nullptr);
    if (!rendered) {
        ver_destroy_render_view(view);
        ver_destroy_scene(scene);
        FAIL() << "Rendering failed for mode: " << modeName;
    }

    const uint8_t* fbData = nullptr;
    uint64_t fbSize = 0;
    ver_render_view_get_framebuffer(view, &fbData, &fbSize);
    ASSERT_NE(fbData, nullptr);
    ASSERT_EQ(fbSize, (uint64_t)width * height * 4);

    verifyOrCompareWithGolden(fbData, width, height, "scene_" + modeName + "_golden.png",
                              "scene_" + modeName);

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}

INSTANTIATE_TEST_SUITE_P(SceneModes, SceneRenderingTest,
                         ::testing::Combine(::testing::Values("vulkan"),
                                            ::testing::Values("mesh3d", "videofile", "imagefile",
                                                              "color", "image360", "streetview",
                                                              "video360")));

TEST(SceneRenderingTestSimple, InvalidDimensions) {
    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int32_t width = -1;
    int32_t height = -1;

    // Default dimensions should be 0
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);

    // Set valid dimensions - should return true
    EXPECT_TRUE(ver_render_view_set_dimensions(view, 640, 480));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid negative dimensions - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, -10, 480));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid too large dimensions - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, 20000, 480));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid height - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, 640, -1));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid too large height - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, 640, 20000));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    ver_destroy_render_view(view);
}

TEST(SceneRenderingTestSimple, BackendSelectionEnvVar) {
    setupTestLibraryPaths();
    std::filesystem::path testProgDir(getTestProgramDirectory());
    std::vector<std::filesystem::path> resourcePaths;
    resourcePaths.push_back(testProgDir / "resources");

    std::filesystem::path vulkanDir = testProgDir / "lib64" / "vulkan";

    // Test Vulkan forcing
    android::base::System::Get()->EnvSet("VER_RENDERER_BACKEND", "vulkan");
    EXPECT_TRUE(ver_initialize(resourcePaths, vulkanDir));
    ver_cleanup();

    // Test Auto mode
    android::base::System::Get()->EnvSet("VER_RENDERER_BACKEND", "auto");
    EXPECT_TRUE(ver_initialize(resourcePaths, vulkanDir));
    ver_cleanup();

    android::base::System::Get()->EnvSet("VER_RENDERER_BACKEND", "");
}

class PosterSideBySideTest : public ::testing::TestWithParam<std::string> {
  protected:
    void SetUp() override {
        setupTestLibraryPaths();
        const std::string& backend = GetParam();
        System::Get()->EnvSet("VER_RENDERER_BACKEND", backend);
    }
    void TearDown() override {
        ver_cleanup();
        System::Get()->EnvSet("VER_RENDERER_BACKEND", "");
    }
};

TEST_P(PosterSideBySideTest, PosterSideBySide) {
    const std::string& backend = GetParam();
    std::filesystem::path testProgDir(getTestProgramDirectory());
    std::filesystem::path testdataDir = testProgDir / "testdata";
    std::string objPath = (testdataDir / "poster_test.obj").string();
    std::string postersPath = (testdataDir / "poster_test.posters").string();

    std::vector<std::filesystem::path> resourcePaths;
    resourcePaths.push_back(testProgDir / "resources");
    resourcePaths.push_back(testdataDir);

    std::filesystem::path vulkanDir = testProgDir / "lib64" / "vulkan";

    bool success = ver_initialize(resourcePaths, vulkanDir);
    ASSERT_TRUE(success) << "Failed to initialize VER with backend: " << backend;
    VerSceneConfig config(VerSceneConfig::Mode::Mesh3D, "poster_test.obj");
    VerSceneHandle scene = ver_create_scene(config);
    ASSERT_NE(scene, (VerSceneHandle)VER_INVALID_HANDLE);

    std::ifstream in(postersPath);
    std::string str;
    std::vector<android::ver::PosterInfo> posters;
    android::ver::PosterInfo poster;
    while (in >> str) {
        if (str == "poster") {
            if (!poster.name.empty()) {
                posters.push_back(poster);
            }
            poster = android::ver::PosterInfo();
            in >> poster.name;
        } else if (str == "position") {
            in >> poster.position.x >> poster.position.y >> poster.position.z;
        } else if (str == "rotation") {
            glm::vec3 euler;
            in >> euler.x >> euler.y >> euler.z;
            float pitch = glm::radians(euler.x);
            float yaw = glm::radians(euler.y);
            float roll = glm::radians(euler.z);
            glm::quat qX = glm::angleAxis(pitch, glm::vec3(1, 0, 0));
            glm::quat qY = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
            glm::quat qZ = glm::angleAxis(roll, glm::vec3(0, 0, 1));
            poster.rotation = qX * qY * qZ;
        } else if (str == "size") {
            in >> poster.size.x >> poster.size.y;
        }
    }
    if (!poster.name.empty()) {
        posters.push_back(poster);
    }

    ver_scene_load_user_resources(scene, [&]() {
        ASSERT_EQ(posters.size(), 2u);
        for (const auto& posterInfo : posters) {
            bool created = ver_scene_create_poster_location(scene, posterInfo);
            ASSERT_TRUE(created);
        }

        std::string img1Path = (testdataDir / "256x256_android.png").string();
        std::string img2Path = (testdataDir / "jpeg_rgb24.jpg").string();

        ver_scene_load_poster(scene, "poster_left", img1Path.c_str(), 1.0f);
        ver_scene_load_poster(scene, "poster_right", img2Path.c_str(), 1.0f);
    });

    ver_scene_update(scene, true);

    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int width = 640;
    int height = 480;
    ASSERT_TRUE(ver_render_view_set_dimensions(view, width, height));

    const float viewProj[16] = {1.500f,  0.000f,  0.000f,  0.000f,   //
                                0.000f,  -1.993f, -0.083f, -0.083f,  //
                                -0.000f, 0.166f,  -0.999f, -0.997f,  //
                                -0.090f, -0.060f, -0.202f, -0.002f};
    ver_render_view_set_view_projection(view, viewProj);

    bool rendered = ver_render_view(scene, view, []() {}, nullptr);
    ASSERT_TRUE(rendered);

    const uint8_t* fbData = nullptr;
    uint64_t fbSize = 0;
    ver_render_view_get_framebuffer(view, &fbData, &fbSize);
    ASSERT_NE(fbData, nullptr);
    ASSERT_EQ(fbSize, (uint64_t)width * height * 4);

    // Poll until the background loader thread finishes decoding both poster
    // textures and the scene renders them, instead of relying on a fixed sleep
    // that flakes under ASAN + software Lavapipe (b/579560086).
    constexpr absl::Duration kTextureLoadTimeout = absl::Seconds(10);
    constexpr absl::Duration kTexturePollInterval = absl::Milliseconds(10);
    constexpr uint64_t kRgbaBytesPerPixel = 4;
    constexpr char kPosterSideBySideGolden[] = "scene_poster_side_by_side_golden.png";

    const absl::Time deadline = absl::Now() + kTextureLoadTimeout;
    while (absl::Now() < deadline) {
        ver_scene_update(scene, true);
        rendered = ver_render_view(scene, view, []() {}, nullptr);
        ASSERT_TRUE(rendered);

        ver_render_view_get_framebuffer(view, &fbData, &fbSize);
        ASSERT_NE(fbData, nullptr);
        ASSERT_EQ(fbSize, static_cast<uint64_t>(width) * height * kRgbaBytesPerPixel);
        if (compareWithGolden(fbData, width, height, kPosterSideBySideGolden)) {
            break;
        }
        absl::SleepFor(kTexturePollInterval);
    }

    verifyOrCompareWithGolden(fbData, width, height, kPosterSideBySideGolden,
                              "scene_poster_side_by_side");

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}
INSTANTIATE_TEST_SUITE_P(PosterSideBySide, PosterSideBySideTest, ::testing::Values("vulkan"));

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};

// Bounding box of the pixels matching a color in an RGBA image.
struct ColorRegion {
    int pixelCount = 0;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = -1;
    int maxY = -1;

    int width() const { return pixelCount ? maxX - minX + 1 : 0; }
    int height() const { return pixelCount ? maxY - minY + 1 : 0; }
};

ColorRegion findColorRegion(const uint8_t* rgba, int width, int height, Rgb color) {
    constexpr int kTolerance = 48;
    ColorRegion region;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* pixel = rgba + (static_cast<size_t>(y) * width + x) * 4;
            if (std::abs(pixel[0] - color.r) <= kTolerance &&
                std::abs(pixel[1] - color.g) <= kTolerance &&
                std::abs(pixel[2] - color.b) <= kTolerance) {
                region.pixelCount++;
                region.minX = std::min(region.minX, x);
                region.minY = std::min(region.minY, y);
                region.maxX = std::max(region.maxX, x);
                region.maxY = std::max(region.maxY, y);
            }
        }
    }
    return region;
}

bool writeSolidColorPng(const std::filesystem::path& path, int width, int height, Rgb color) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = color.r;
        rgba[i + 1] = color.g;
        rgba[i + 2] = color.b;
        rgba[i + 3] = 0xFF;
    }
    return saveRgbaPng(path.string().c_str(), width, height, rgba.data());
}

class PosterUpdateTest : public ::testing::Test {
  protected:
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 480;
    static constexpr int kMinPosterPixels = 1000;
    static constexpr const char* kPosterName = "poster_left";

    void SetUp() override {
        setupTestLibraryPaths();
        System::Get()->EnvSet("VER_RENDERER_BACKEND", "vulkan");

        const std::filesystem::path testProgDir(getTestProgramDirectory());
        const std::filesystem::path testdataDir = testProgDir / "testdata";
        ASSERT_TRUE(ver_initialize({testProgDir / "resources", testdataDir},
                                   testProgDir / "lib64" / "vulkan"));

        mTempDir = System::Get()->GetTempDir() /
                   (std::string("ver_poster_update_") +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        std::filesystem::create_directories(mTempDir, ec);
        ASSERT_FALSE(ec) << ec.message();

        mScene = ver_create_scene(VerSceneConfig(VerSceneConfig::Mode::Mesh3D, "poster_test.obj"));
        ASSERT_NE(mScene, (VerSceneHandle)VER_INVALID_HANDLE);

        bool posterCreated = false;
        ver_scene_load_user_resources(mScene, [&]() {
            PosterInfo info;
            info.name = kPosterName;
            info.size = glm::vec2(0.6f, 0.6f);
            info.position = glm::vec3(-0.5f, 0.0f, -2.0f);
            info.rotation = glm::angleAxis(0.0f, glm::vec3(1.0f, 0.0f, 0.0f));
            posterCreated = ver_scene_create_poster_location(mScene, info);
        });
        ASSERT_TRUE(posterCreated);

        // Pause the scene time, as done when the scene animations are disabled,
        // so that only scene changes can invalidate the cached view contents.
        ver_scene_set_frame_time_us(mScene, 0);

        mView = ver_create_render_view();
        ASSERT_NE(mView, (VerRenderViewHandle)VER_INVALID_HANDLE);
        ASSERT_TRUE(ver_render_view_set_dimensions(mView, kWidth, kHeight));

        const float viewProj[16] = {1.500f,  0.000f,  0.000f,  0.000f,   //
                                    0.000f,  -1.993f, -0.083f, -0.083f,  //
                                    -0.000f, 0.166f,  -0.999f, -0.997f,  //
                                    -0.090f, -0.060f, -0.202f, -0.002f};
        ver_render_view_set_view_projection(mView, viewProj);
    }

    void TearDown() override {
        if (mView != VER_INVALID_HANDLE) {
            ver_destroy_render_view(mView);
        }
        if (mScene != VER_INVALID_HANDLE) {
            ver_destroy_scene(mScene);
        }
        ver_cleanup();
        System::Get()->EnvSet("VER_RENDERER_BACKEND", "");

        std::error_code ec;
        std::filesystem::remove_all(mTempDir, ec);
    }

    // Updates and renders the scene without advancing the scene time, until
    // |predicate| accepts the rendered frame or the timeout expires.
    template <typename Predicate>
    bool renderUntil(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            ver_scene_update(mScene, false);
            if (!ver_render_view(mScene, mView, []() {}, nullptr)) {
                ADD_FAILURE() << "ver_render_view failed";
                return false;
            }

            const uint8_t* fbData = nullptr;
            uint64_t fbSize = 0;
            ver_render_view_get_framebuffer(mView, &fbData, &fbSize);
            if (fbData && fbSize == static_cast<uint64_t>(kWidth) * kHeight * 4 &&
                predicate(fbData)) {
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    static ColorRegion findColor(const uint8_t* fbData, Rgb color) {
        return findColorRegion(fbData, kWidth, kHeight, color);
    }

    std::filesystem::path mTempDir;
    VerSceneHandle mScene = VER_INVALID_HANDLE;
    VerRenderViewHandle mView = VER_INVALID_HANDLE;
};

// Poster images are loaded asynchronously, the new image has to be rendered
// once it is available even if the scene time is paused.
TEST_F(PosterUpdateTest, PosterChangeIsRenderedWhileTimeIsPaused) {
    const std::filesystem::path redPath = mTempDir / "red.png";
    const std::filesystem::path greenPath = mTempDir / "green_wide.png";
    ASSERT_TRUE(writeSolidColorPng(redPath, 64, 64, kRed));
    ASSERT_TRUE(writeSolidColorPng(greenPath, 256, 64, kGreen));

    ver_scene_load_poster(mScene, kPosterName, redPath.string().c_str(), 1.0f);
    ASSERT_TRUE(renderUntil([](const uint8_t* fbData) {
        return findColor(fbData, kRed).pixelCount > kMinPosterPixels;
    })) << "Poster image is not rendered after loading it";

    ver_scene_load_poster(mScene, kPosterName, greenPath.string().c_str(), 1.0f);
    ColorRegion green;
    ASSERT_TRUE(renderUntil([&green](const uint8_t* fbData) {
        green = findColor(fbData, kGreen);
        return green.pixelCount > kMinPosterPixels && findColor(fbData, kRed).pixelCount == 0;
    })) << "Changed poster image is not rendered";

    // The poster should be resized to match the 4:1 aspect ratio of the new
    // image, instead of keeping the size of the previous one.
    ASSERT_GT(green.height(), 0);
    EXPECT_NEAR(static_cast<float>(green.width()) / green.height(), 4.0f, 0.5f);
}

// Loading a poster again from the same path has to pick up the changes made to
// the image file since it was previously loaded.
TEST_F(PosterUpdateTest, PosterReloadPicksUpModifiedImageFile) {
    const std::filesystem::path posterPath = mTempDir / "poster.png";
    ASSERT_TRUE(writeSolidColorPng(posterPath, 64, 64, kRed));

    ver_scene_load_poster(mScene, kPosterName, posterPath.string().c_str(), 1.0f);
    ASSERT_TRUE(renderUntil([](const uint8_t* fbData) {
        return findColor(fbData, kRed).pixelCount > kMinPosterPixels;
    })) << "Poster image is not rendered after loading it";

    // Also move the modification time forward, in case the file system
    // timestamps are too coarse to tell the two writes apart.
    std::error_code ec;
    const auto previousWriteTime = std::filesystem::last_write_time(posterPath, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(writeSolidColorPng(posterPath, 32, 32, kGreen));
    std::filesystem::last_write_time(posterPath, previousWriteTime + std::chrono::seconds(2), ec);
    ASSERT_FALSE(ec) << ec.message();

    ver_scene_load_poster(mScene, kPosterName, posterPath.string().c_str(), 1.0f);
    ASSERT_TRUE(renderUntil([](const uint8_t* fbData) {
        return findColor(fbData, kGreen).pixelCount > kMinPosterPixels &&
               findColor(fbData, kRed).pixelCount == 0;
    })) << "Modified poster image is not reloaded";
}

}  // namespace
