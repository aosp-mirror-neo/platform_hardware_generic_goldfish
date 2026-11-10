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

#include "absl/log/check.h"

#define FAILURE_DEBUG_PREFIX "VirtualsceneImageProvider"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>

#include "goldfish/debug.h"
#include "goldfish/imaging/query_info.h"
#include "goldfish/physics/glm_helpers.h"
#include "goldfish/sensors/physical_model.h"
#include "libyuv/convert.h"
#include "libyuv/convert_from_argb.h"
#include "libyuv/rotate_argb.h"
#include "libyuv/scale_argb.h"

namespace goldfish::camera_image_providers::virtualscene {
namespace {

constexpr const char* kDefaultSceneModelPath = "data/virtualscene/Toren1BD/Toren1BD.obj";

constexpr float kDefaultZNear = 0.1f;
constexpr float kDefaultZFar = 100.0f;
constexpr float kDefaultFx = 1.0f;
constexpr float kDefaultFy = 1.0f;

const glm::mat4& getCameraFromSensors() {
    static const glm::mat4 kCameraFromSensors = []() {
        const float sqrt2_over_2 = std::sqrt(2.0f) / 2.0f;
        glm::mat4 sensorLocalMatrix =
                glm::mat4_cast(glm::quat(sqrt2_over_2, 0.0f, 0.0f, sqrt2_over_2));
        sensorLocalMatrix[3] = glm::vec4(-0.03f, -0.06f, 0.002f, 1.0f);
        const glm::mat4 cameraLocalTransform(
                glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, -1.0f, 0.0f, 0.0f),
                glm::vec4(0.0f, 0.0f, 1.0f, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        return cameraLocalTransform * sensorLocalMatrix;
    }();
    return kCameraFromSensors;
}

glm::mat4 projectionMatrixForCameraIntrinsics(float width, float height, float fx, float fy,
                                              float cx, float cy, float zNear, float zFar) {
    const float xscale = zNear / fx;
    const float yscale = zNear / fy;
    const float xoffset = (cx - (width / 2.0f)) * xscale;
    // Color camera's coordinates has y pointing downwards so we negate this term.
    const float yoffset = -(cy - (height / 2.0f)) * yscale;

    return glm::frustum(xscale * -width / 2.0f - xoffset, xscale * width / 2.0f - xoffset,
                        yscale * -height / 2.0f - yoffset, yscale * height / 2.0f - yoffset, zNear,
                        zFar);
}

}  // namespace

const char* VirtualsceneImageProvider::getId() const {
    return mId.c_str();
}

glm::mat4 VirtualsceneImageProvider::computeProjectionMatrix(const CameraImageProviderRect size) {
    DCHECK(size.width);
    DCHECK(size.height);
    const float aspectRatio = static_cast<float>(size.width) / static_cast<float>(size.height);
    return projectionMatrixForCameraIntrinsics(aspectRatio, 1.0f, kDefaultFx, kDefaultFy,
                                               aspectRatio / 2.0f, 0.5f, kDefaultZNear,
                                               kDefaultZFar);
}

glm::mat4 VirtualsceneImageProvider::computeViewProjectionMatrix(
        const glm::mat4& projection, const glm::vec3& position,
        const glm::vec3& rotationEulerDegrees, const bool isBackFacing,
        const bool supportsPosition) {
    glm::vec3 extraRotationEulerDegrees(0.0f);
    if (!isBackFacing) {
        extraRotationEulerDegrees.y += 180.0f;
    }

    const glm::mat4 sensorsExtraRotation =
            glm::mat4_cast(FromEulerAnglesXyz(glm::radians(extraRotationEulerDegrees)));
    const glm::mat4 rotationMat =
            glm::mat4_cast(FromEulerAnglesXyz(glm::radians(rotationEulerDegrees))) *
            sensorsExtraRotation;

    glm::mat4 inverseSensorsPose = glm::inverse(rotationMat);
    if (supportsPosition) {
        inverseSensorsPose *= glm::translate(glm::mat4(1.0f), -position);
    }

    const glm::mat4 viewFromWorld = getCameraFromSensors() * inverseSensorsPose;
    return projection * viewFromWorld;
}

int VirtualsceneImageProvider::computePostRenderRotation(const VerSceneConfig::Mode sceneMode,
                                                         const int sceneBaseRotationDegrees,
                                                         const glm::vec3& accelerometer,
                                                         const bool isBackFacing) {
    if (VerSceneConfig::modeSupportsViewRotations(sceneMode)) {
        return 0;
    }

    // Coarse device orientation (matching AndroidCoarseOrientation):
    // 0: Portrait (0 deg), 1: Reverse Landscape (90 deg),
    // 2: Reverse Portrait (180 deg), 3: Landscape (270 deg).
    int convertOrientation = 0;
    if (std::abs(accelerometer.x) <= std::abs(accelerometer.y)) {
        convertOrientation = (accelerometer.y < 0.0f) ? 2 : 0;
    } else {
        convertOrientation = (accelerometer.x < 0.0f) ? 1 : 3;
    }

    const int deviceSteps =
            isBackFacing ? (7 - convertOrientation) % 4 : (3 + convertOrientation) % 4;
    const int sceneSteps = (sceneBaseRotationDegrees / 90) % 4;
    int rotationSteps = (deviceSteps - sceneSteps) % 4;
    if (rotationSteps < 0) {
        rotationSteps += 4;
    }
    return rotationSteps * 90;
}

glm::mat4 VirtualsceneImageProvider::updateViewProjection(const glm::mat4& projection) const {
    glm::vec3 position(0.0f);
    glm::vec3 rotationEulerDegrees(0.0f);
    if (mPhysicalModel) {
        int64_t timestamp = 0;
        mPhysicalModel->GetTransform(&position.x, &position.y, &position.z, &rotationEulerDegrees.x,
                                     &rotationEulerDegrees.y, &rotationEulerDegrees.z, &timestamp);
    }
    const bool supportsPosition =
            (mScene != VER_INVALID_HANDLE)
                    ? VerSceneConfig::modeSupportsCameraTranslation(ver_scene_get_mode(mScene))
                    : true;
    return computeViewProjectionMatrix(projection, position, rotationEulerDegrees, mIsBackFacing,
                                       supportsPosition);
}

int VirtualsceneImageProvider::getPostRenderRotationDegrees() const {
    if (mScene == VER_INVALID_HANDLE) {
        return 0;
    }
    const VerSceneConfig::Mode sceneMode = ver_scene_get_mode(mScene);
    if (VerSceneConfig::modeSupportsViewRotations(sceneMode)) {
        return 0;
    }
    glm::vec3 accel(0.0f, 9.81f, 0.0f);
    if (mPhysicalModel) {
        size_t measurementId = 0;
        accel = mPhysicalModel->GetAccelerometer(&measurementId);
    }
    const int sceneRotation = ver_scene_get_scene_rotation(mScene);
    return computePostRenderRotation(sceneMode, sceneRotation, accel, mIsBackFacing);
}

int VirtualsceneImageProvider::start(const CameraImageProviderStreamConfig* const streamConfigs,
                                     const unsigned streamConfigsSize) {
    const RenderOutputConfig roc = getRenderOutputConfig(streamConfigs, streamConfigsSize);
    if (!roc.size.width || !roc.size.height) {
        return FAILURE_STR("render output size size is zero", -1);
    }

    const imaging::ImageFormat framebufferFormat = roc.getFormat();
    if (framebufferFormat == imaging::ImageFormat::kNone) {
        return FAILURE(-1);
    }

    std::vector<CaptureStreamBuffers> captureStreamBuffers(streamConfigsSize);
    size_t maxScaledBufferSize = 0;
    size_t maxYuvConversionBufferSize = 0;
    for (unsigned i = 0; i < streamConfigsSize; ++i) {
        const CameraImageProviderStreamConfig& cfg = streamConfigs[i];
        if (!cfg.size.width || !cfg.size.height) {
            return FAILURE_STR("stream size is zero", -1);
        }

        const imaging::ImageFormat streamFormat = static_cast<imaging::ImageFormat>(cfg.format);
        switch (streamFormat) {
        case imaging::ImageFormat::kNone:
        case imaging::ImageFormat::kRgba8888:
            break;

        case imaging::ImageFormat::kYuV4203P:
        case imaging::ImageFormat::kYuV420NV12:
            if ((cfg.size.width | cfg.size.height) & 1U) {
                return FAILURE_STR("YUV stream with odd size", -1);
            }
            break;
        }

        const imaging::ImageFormat stagingFormat = getStagingFormat(streamFormat);
        if (stagingFormat == imaging::ImageFormat::kNone) {
            return FAILURE_STR("stagingFormat", -1);
        }

        const uint32_t dstBufferSize =
                imaging::GetDataSize(stagingFormat, cfg.size.width, cfg.size.height);
        if (!dstBufferSize) {
            return FAILURE_STR("dstBufferSize", -1);
        }

        CaptureStreamBuffers& bufs = captureStreamBuffers[i];
        bufs.requiresScaling = (framebufferFormat != stagingFormat) ||
                               (roc.size.width != cfg.size.width) ||
                               (roc.size.height != cfg.size.height);
        if (bufs.requiresScaling) {
            maxScaledBufferSize = std::max(maxScaledBufferSize, static_cast<size_t>(dstBufferSize));
        }

        switch (streamFormat) {
        case imaging::ImageFormat::kNone:
        case imaging::ImageFormat::kRgba8888:
            break;

        case imaging::ImageFormat::kYuV4203P:
        case imaging::ImageFormat::kYuV420NV12:
            maxYuvConversionBufferSize =
                    std::max(maxYuvConversionBufferSize,
                             imaging::GetDataSize(streamFormat, cfg.size.width, cfg.size.height));
            break;
        }

        bufs.srcCropRegion = getCropRegion(roc.size, cfg.size);
        bufs.size = cfg.size;
        bufs.bufferSizeBytes = dstBufferSize;
        bufs.id = cfg.id;
    }

    VerRenderViewHandle view = ver_create_render_view();
    if (view == VER_INVALID_HANDLE) {
        return FAILURE_STR("ver_create_render_view", -1);
    }

    const glm::mat4 projection = computeProjectionMatrix(roc.size);
    const glm::mat4 viewProj = updateViewProjection(projection);
    ver_render_view_set_view_projection(view, &viewProj[0][0]);
    const int rotationDegrees = getPostRenderRotationDegrees();
    const bool swapDimensions = (rotationDegrees == 90 || rotationDegrees == 270);
    const uint16_t viewWidth = swapDimensions ? roc.size.height : roc.size.width;
    const uint16_t viewHeight = swapDimensions ? roc.size.width : roc.size.height;
    if (!ver_render_view_set_dimensions(view, static_cast<int32_t>(viewWidth),
                                        static_cast<int32_t>(viewHeight))) {
        ver_destroy_render_view(view);
        return FAILURE_STR("ver_render_view_set_dimensions", -1);
    }

    const bool enableSceneControls =
            mPhysicalModel && mScene != VER_INVALID_HANDLE &&
            VerSceneConfig::modeSupportsSceneControls(ver_scene_get_mode(mScene));
    if (enableSceneControls) {
        // TODO(b/513154005): Const casting is not ideal, and a temporary solution until we
        // refactor for environment scene manager, this is safe as we don't support background
        // rendering and there can only be one camera using the VirtualSceneCamera.
        const_cast<sensors::PhysicalModel*>(mPhysicalModel)->GetVirtualSceneCamera().AddSceneUser();
    }

    stop();

    CaptureSession captureSession;
    captureSession.captureStreamBuffers = std::move(captureStreamBuffers);
    captureSession.scaledFramebuffer.resize(maxScaledBufferSize);
    captureSession.yuvConversionBuffer.resize(maxYuvConversionBufferSize);
    if (mScene != VER_INVALID_HANDLE &&
        !VerSceneConfig::modeSupportsViewRotations(ver_scene_get_mode(mScene))) {
        captureSession.rotatedFramebuffer.resize(static_cast<size_t>(roc.size.width) *
                                                 static_cast<size_t>(roc.size.height) * 4);
    }
    captureSession.projection = projection;
    captureSession.view = view;
    captureSession.framebuffer_size = roc.size;
    captureSession.sceneControlsActive = enableSceneControls;

    mCaptureSession = std::move(captureSession);
    return 0;
}

VirtualsceneImageProvider::RenderOutputConfig VirtualsceneImageProvider::getRenderOutputConfig(
        const CameraImageProviderStreamConfig* s, unsigned n) {
    return std::accumulate(
            s, s + n, RenderOutputConfig(),
            [](RenderOutputConfig result, const CameraImageProviderStreamConfig& cfg) {
                const size_t cfgArea = size_t(cfg.size.width) * cfg.size.height;
                const size_t resultArea = size_t(result.size.width) * result.size.height;
                if (resultArea < cfgArea) {
                    result.size = cfg.size;
                }
                return result;
            });
}

imaging::ImageFormat VirtualsceneImageProvider::RenderOutputConfig::getFormat() const {
    // TODO: HDR
    return imaging::ImageFormat::kRgba8888;
}

imaging::ImageFormat VirtualsceneImageProvider::getStagingFormat(const imaging::ImageFormat fmt) {
    using imaging::ImageFormat;

    switch (fmt) {
    case ImageFormat::kNone:
        break;

    // vkCmdBlitImage into YUV support is optional,
    // we have to convert on the CPU via libuyv.
    case ImageFormat::kYuV4203P:
    case ImageFormat::kYuV420NV12:
    case ImageFormat::kRgba8888:
        return ImageFormat::kRgba8888;
    }

    return ImageFormat::kNone;
}

VirtualsceneImageProvider::CropRegion VirtualsceneImageProvider::getCropRegion(
        const CameraImageProviderRect srcSize, const CameraImageProviderRect dstSize) {
    DCHECK(srcSize.width);
    DCHECK(srcSize.height);
    DCHECK(dstSize.width);
    DCHECK(dstSize.height);

    using Dimension = decltype(CameraImageProviderRect::width);

    const double srcAspectRatio = static_cast<double>(srcSize.width) / srcSize.height;
    const double dstAspectRatio = static_cast<double>(dstSize.width) / dstSize.height;

    CropRegion result;

    if (srcAspectRatio > dstAspectRatio) {
        // Source is wider than destination, so we crop the width to match the
        // destination's aspect ratio (pillarboxing).
        const Dimension newWidth = static_cast<Dimension>(srcSize.height * dstAspectRatio);
        result.size = {newWidth, srcSize.height};
        result.offset = {static_cast<Dimension>((srcSize.width - newWidth) / 2), 0};
    } else if (srcAspectRatio < dstAspectRatio) {
        // Source is taller than destination, so we crop the height to match the
        // destination's aspect ratio (letterboxing).
        const Dimension newHeight = static_cast<Dimension>(srcSize.width / dstAspectRatio);
        result.size = {srcSize.width, newHeight};
        result.offset = {0, static_cast<Dimension>((srcSize.height - newHeight) / 2)};
    } else {
        // Aspect ratios are the same, no crop needed.
        result.size = srcSize;
    }

    return result;
}

const VirtualsceneImageProvider::CaptureStreamBuffers*
VirtualsceneImageProvider::findCaptureStreamBuffers(const int32_t id, const CaptureStreamBuffers* a,
                                                    size_t n) {
    const CaptureStreamBuffers* i = std::find_if(
            a, a + n, [id](const CaptureStreamBuffers& bufs) { return bufs.id == id; });
    return (i == (a + n)) ? nullptr : i;
}

int VirtualsceneImageProvider::capture(const CameraImageProviderCaptureOpts& /*opts*/,
                                       const CameraImageProviderStreamCaptureSink sink,
                                       void* const sinkOpaque,
                                       const CameraImageProviderStreamCaptureInfo* scis,
                                       const unsigned scisSize) {
    if (!scisSize) {
        return 0;
    } else if (mCaptureSession) {
        CaptureSession& session = *mCaptureSession;
        if (!captureImpl(session, sink, sinkOpaque, scis, scisSize,
                         session.scaledFramebuffer.data(), session.yuvConversionBuffer.data(),
                         session.rotatedFramebuffer.data())) {
            return FAILURE_STR("captureImpl", -1);
        }

        return 0;
    } else {
        return FAILURE_STR("!mCaptureSession", -1);
    }
}

bool VirtualsceneImageProvider::captureImpl(const CaptureSession& session,
                                            const CameraImageProviderStreamCaptureSink sink,
                                            void* const sinkOpaque,
                                            const CameraImageProviderStreamCaptureInfo* scis,
                                            const unsigned scisSize, uint8_t* scaledFramebuffer,
                                            uint8_t* yuvConversionBuffer,
                                            uint8_t* rotatedFramebuffer) {
    ver_scene_update(mScene, true);

    const VerSceneConfig::Mode sceneMode = (mScene != VER_INVALID_HANDLE)
                                                   ? ver_scene_get_mode(mScene)
                                                   : VerSceneConfig::Mode::Unknown;
    int rotationDegrees = 0;
    if (VerSceneConfig::modeSupportsViewRotations(sceneMode)) {
        const glm::mat4 viewProj = updateViewProjection(session.projection);
        ver_render_view_set_view_projection(session.view, &viewProj[0][0]);
    } else {
        rotationDegrees = getPostRenderRotationDegrees();
    }

    const bool swapDimensions = (rotationDegrees == 90 || rotationDegrees == 270);
    const uint16_t viewWidth =
            swapDimensions ? session.framebuffer_size.height : session.framebuffer_size.width;
    const uint16_t viewHeight =
            swapDimensions ? session.framebuffer_size.width : session.framebuffer_size.height;
    if (!ver_render_view_set_dimensions(session.view, static_cast<int32_t>(viewWidth),
                                        static_cast<int32_t>(viewHeight))) {
        return FAILURE_STR("ver_render_view_set_dimensions", false);
    }

    if (!ver_render_view(mScene, session.view, []() {}, nullptr)) {
        return FAILURE_STR("ver_render_view", false);
    }

    const uint8_t* rawFbData = nullptr;
    uint64_t fbSize = 0;
    ver_render_view_get_framebuffer(session.view, &rawFbData, &fbSize);

    const uint64_t expectedFbSize = static_cast<uint64_t>(session.framebuffer_size.width) *
                                    static_cast<uint64_t>(session.framebuffer_size.height) * 4;
    if (!rawFbData || fbSize < expectedFbSize) {
        return FAILURE_STR("ver_render_view_get_framebuffer", false);
    }

    const int srcStride = static_cast<int>(session.framebuffer_size.width) * 4;
    const uint8_t* fbData = rawFbData;
    if (rotationDegrees != 0) {
        if (!rotatedFramebuffer) {
            return FAILURE_STR("!rotatedFramebuffer", false);
        }
        libyuv::RotationMode mode = libyuv::kRotate0;
        switch (rotationDegrees) {
        case 90:
            mode = libyuv::kRotate90;
            break;
        case 180:
            mode = libyuv::kRotate180;
            break;
        case 270:
            mode = libyuv::kRotate270;
            break;
        default:
            break;
        }
        if (libyuv::ARGBRotate(rawFbData, static_cast<int>(viewWidth) * 4, rotatedFramebuffer,
                               srcStride, static_cast<int>(viewWidth), static_cast<int>(viewHeight),
                               mode) != 0) {
            return FAILURE_STR("ARGBRotate", false);
        }
        fbData = rotatedFramebuffer;
    }

    for (unsigned i = 0; i < scisSize; ++i) {
        const CameraImageProviderStreamCaptureInfo& sci = scis[i];
        const CameraImageProviderStreamConfig& cfg = *sci.cfg;
        const CaptureStreamBuffers* bufs = findCaptureStreamBuffers(
                cfg.id, session.captureStreamBuffers.data(), session.captureStreamBuffers.size());
        if (!bufs) {
            continue;
        }

        const uint8_t* bits = fbData;
        if (bufs->requiresScaling) {
            if (!scaledFramebuffer || session.scaledFramebuffer.size() < bufs->bufferSizeBytes) {
                return FAILURE_STR("!scaledFramebuffer", false);
            }
            const int srcOffsetX = static_cast<int>(bufs->srcCropRegion.offset.width);
            const int srcOffsetY = static_cast<int>(bufs->srcCropRegion.offset.height);
            const int srcCropWidth = static_cast<int>(bufs->srcCropRegion.size.width);
            const int srcCropHeight = static_cast<int>(bufs->srcCropRegion.size.height);
            const int dstWidth = static_cast<int>(bufs->size.width);
            const int dstHeight = static_cast<int>(bufs->size.height);
            const int dstStride = dstWidth * 4;

            const uint8_t* srcPtr = fbData + (srcOffsetY * srcStride) + (srcOffsetX * 4);
            if (libyuv::ARGBScale(srcPtr, srcStride, srcCropWidth, srcCropHeight, scaledFramebuffer,
                                  dstStride, dstWidth, dstHeight, libyuv::kFilterBilinear) != 0) {
                return FAILURE_STR("ARGBScale", false);
            }
            bits = scaledFramebuffer;
        }

        switch (static_cast<imaging::ImageFormat>(cfg.format)) {
        case imaging::ImageFormat::kNone:
            break;

        case imaging::ImageFormat::kRgba8888:
            sink(sinkOpaque, &sci, bits, bufs->bufferSizeBytes);
            break;

        case imaging::ImageFormat::kYuV4203P: {
            const int width = static_cast<int>(cfg.size.width);
            const int width2 = width / 2;
            const int height = static_cast<int>(cfg.size.height);
            uint8_t* y = yuvConversionBuffer;
            uint8_t* u = y + width * height;
            uint8_t* v = u + width * height / 4;

            libyuv::ABGRToI420(bits, width * 4, y, width, u, width2, v, width2, width, height);
            sink(sinkOpaque, &sci, yuvConversionBuffer, width * height * 3 / 2);
        } break;

        case imaging::ImageFormat::kYuV420NV12: {
            const int width = static_cast<int>(cfg.size.width);
            const int height = static_cast<int>(cfg.size.height);
            uint8_t* y = yuvConversionBuffer;
            uint8_t* uv = y + width * height;

            libyuv::ABGRToNV12(bits, width * 4, y, width, uv, width, width, height);
            sink(sinkOpaque, &sci, yuvConversionBuffer, width * height * 3 / 2);
        } break;
        }
    }

    return true;
}

void VirtualsceneImageProvider::stop() {
    if (mCaptureSession) {
        if (mCaptureSession->sceneControlsActive && mPhysicalModel) {
            // TODO(b/513154005): Const casting is not ideal, and a temporary solution until we
            // refactor for environment scene manager, this is safe as we don't support background
            // rendering and there can only be one camera using the VirtualSceneCamera.
            const_cast<sensors::PhysicalModel*>(mPhysicalModel)
                    ->GetVirtualSceneCamera()
                    .RemoveSceneUser();
            mCaptureSession->sceneControlsActive = false;
        }
        if (mCaptureSession->view != VER_INVALID_HANDLE) {
            ver_destroy_render_view(mCaptureSession->view);
            mCaptureSession->view = VER_INVALID_HANDLE;
        }
        mCaptureSession.reset();
    }
}

size_t VirtualsceneImageProvider::getScaledFramebufferSizeBytes() const {
    return mCaptureSession ? mCaptureSession->scaledFramebuffer.size() : 0;
}

VirtualsceneImageProvider::~VirtualsceneImageProvider() {
    stop();
    if (mScene != VER_INVALID_HANDLE) {
        ver_destroy_scene(mScene);
        mScene = VER_INVALID_HANDLE;
    }
}

void* VirtualsceneImageProvider::create(const CameraImageProviderInfo& info) {
    const auto* args = static_cast<const CreateArgs*>(info.createArg);
    VerSceneConfig sceneConfig =
            (args && args->sceneConfig.mSceneMode != VerSceneConfig::Mode::Unknown)
                    ? args->sceneConfig
                    : VerSceneConfig(VerSceneConfig::Mode::Mesh3D, kDefaultSceneModelPath);
    if (sceneConfig.mArgument.empty()) {
        if (sceneConfig.mSceneMode == VerSceneConfig::Mode::Mesh3D) {
            sceneConfig.mArgument = kDefaultSceneModelPath;
        } else {
            sceneConfig.mArgument = VerSceneConfig::defaultArgumentForMode(sceneConfig.mSceneMode);
        }
    }

    VerSceneHandle scene = ver_create_scene(sceneConfig);
    if (scene == VER_INVALID_HANDLE) {
        return FAILURE_STR("ver_create_scene", nullptr);
    }

    bool resourcesLoaded = false;
    ver_scene_load_user_resources(scene, [&resourcesLoaded]() { resourcesLoaded = true; });
    if (VerSceneConfig::modeRequiresRenderer(sceneConfig.mSceneMode) && !resourcesLoaded) {
        ver_destroy_scene(scene);
        return FAILURE_STR("ver_scene_load_user_resources", nullptr);
    }

    auto imageProvider = std::make_unique<VirtualsceneImageProvider>();
    imageProvider->mScene = scene;
    imageProvider->mPhysicalModel = args ? args->physicalModel : nullptr;
    imageProvider->mId = std::string(VerSceneConfig::modeToString(sceneConfig.mSceneMode)) + ":" +
                         sceneConfig.mArgument;
    imageProvider->mIsBackFacing = (info.isBackFacing != 0);

    return imageProvider.release();
}

}  // namespace goldfish::camera_image_providers::virtualscene
