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

#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "android/camera/camera_image_provider_api.h"
#include "ver/virtual_environment_renderer.h"

namespace goldfish::sensors {
class PhysicalModel;
}  // namespace goldfish::sensors

namespace goldfish::camera_image_providers::virtualscene {

struct VirtualsceneImageProvider {
    struct CreateArgs {
        VerSceneConfig sceneConfig;
        const sensors::PhysicalModel* physicalModel = nullptr;
    };

    struct RenderOutputConfig {
        CameraImageProviderRect size = {};
        imaging::ImageFormat getFormat() const;
    };

    struct CropRegion {
        CameraImageProviderRect offset = {};
        CameraImageProviderRect size = {};
    };

    struct CaptureStreamBuffers {
        CameraImageProviderRect size = {};
        CropRegion srcCropRegion = {};
        uint32_t bufferSizeBytes = 0;
        int32_t id = -1;  // CameraImageProviderStreamConfig::id
        // If false, the framebuffer has the exact same format and size,
        // so we can read from the VER framebuffer directly.
        bool requiresScaling = false;
    };

    VirtualsceneImageProvider() = default;
    ~VirtualsceneImageProvider();

    const char* getId() const;
    int start(const CameraImageProviderStreamConfig* s, unsigned n);
    int capture(const CameraImageProviderCaptureOpts&, const CameraImageProviderStreamCaptureSink,
                void* sinkOpaque, const CameraImageProviderStreamCaptureInfo* sci, unsigned scin);
    void stop();
    size_t getScaledFramebufferSizeBytes() const;

    static void* create(const CameraImageProviderInfo&);

    static RenderOutputConfig getRenderOutputConfig(const CameraImageProviderStreamConfig* s,
                                                    unsigned n);

    static imaging::ImageFormat getStagingFormat(imaging::ImageFormat);

    // NOTE: all dimensions must be non-zero
    static CropRegion getCropRegion(CameraImageProviderRect srcSize,
                                    CameraImageProviderRect dstSize);

    static const CaptureStreamBuffers* findCaptureStreamBuffers(int32_t id,
                                                                const CaptureStreamBuffers* a,
                                                                size_t n);

    static glm::mat4 computeProjectionMatrix(CameraImageProviderRect size);
    static glm::mat4 computeViewProjectionMatrix(const glm::mat4& projection,
                                                 const glm::vec3& position,
                                                 const glm::vec3& rotationEulerDegrees,
                                                 bool isBackFacing, bool supportsPosition);
    static int computePostRenderRotation(VerSceneConfig::Mode sceneMode,
                                         int sceneBaseRotationDegrees,
                                         const glm::vec3& accelerometer, bool isBackFacing);

  private:
    struct CaptureSession {
        std::vector<CaptureStreamBuffers> captureStreamBuffers;
        std::vector<uint8_t> scaledFramebuffer;
        std::vector<uint8_t> yuvConversionBuffer;
        std::vector<uint8_t> rotatedFramebuffer;
        glm::mat4 projection = glm::mat4(1.0f);
        VerRenderViewHandle view = VER_INVALID_HANDLE;
        CameraImageProviderRect framebuffer_size = {};
        bool sceneControlsActive = false;
    };

    glm::mat4 updateViewProjection(const glm::mat4& projection) const;
    int getPostRenderRotationDegrees() const;

    bool captureImpl(const CaptureSession&, CameraImageProviderStreamCaptureSink, void* sinkOpaque,
                     const CameraImageProviderStreamCaptureInfo* sci, unsigned scin,
                     uint8_t* scaledFramebuffer, uint8_t* yuvConversionBuffer,
                     uint8_t* rotatedFramebuffer);

    VerSceneHandle mScene = VER_INVALID_HANDLE;
    const sensors::PhysicalModel* mPhysicalModel = nullptr;
    std::string mId = "virtualscene";
    bool mIsBackFacing = true;

    std::optional<CaptureSession> mCaptureSession;
};

}  // namespace goldfish::camera_image_providers::virtualscene