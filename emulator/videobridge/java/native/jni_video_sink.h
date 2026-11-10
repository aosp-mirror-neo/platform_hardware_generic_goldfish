// Copyright (C) 2026 The Android Open Source Project
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

#pragma once

#include <jni.h>

#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-reference-return"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#pragma clang diagnostic pop

#include "frame_buffer_pool.h"
#include "goldfish/base/unique_handle.h"

/**
 * @file jni_video_sink.h
 * @brief Native WebRTC video frame sink bridging decoded video frames to Java/Kotlin callbacks via
 * JNI.
 */

namespace goldfish::videobridge::java {

/**
 * @struct JniEnvDeleter
 * @brief Deleter for `goldfish::base::UniqueHandle` that detaches the current thread from the JVM
 * when `jvm_` is non-null (indicating `ScopedJniEnv` attached the thread).
 */
struct JniEnvDeleter {
    struct Empty {};
    explicit JniEnvDeleter(Empty) {}
    explicit JniEnvDeleter(JavaVM* jvm) : jvm_(jvm) {}
    void operator()(JNIEnv* /*env*/) const {
        if (jvm_) {
            jvm_->DetachCurrentThread();
        }
    }
    JavaVM* jvm_ = nullptr;
};

/**
 * @class ScopedJniEnv
 * @brief RAII helper built on `goldfish::base::UniqueHandle` that attaches the current native
 * thread to the JVM if needed and detaches on destruction.
 */
class ScopedJniEnv : public goldfish::base::UniqueHandle<JNIEnv*, nullptr, JniEnvDeleter> {
  public:
    explicit ScopedJniEnv(JavaVM* jvm, jint version = JNI_VERSION_1_8)
            : UniqueHandle(Acquire(jvm, version)) {}

    JNIEnv* operator->() const { return get(); }

  private:
    static UniqueHandle Acquire(JavaVM* jvm, jint version) {
        if (!jvm) return {};
        JNIEnv* env = nullptr;
        if (jvm->GetEnv(reinterpret_cast<void**>(&env), version) != JNI_EDETACHED) {
            return UniqueHandle(env, goldfish::videobridge::java::JniEnvDeleter(nullptr));
        }
#if defined(__ANDROID__)
        bool attached = (jvm->AttachCurrentThread(&env, nullptr) == JNI_OK);
#else
        bool attached =
                (jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) ==
                 JNI_OK);
#endif
        return UniqueHandle(env,
                            goldfish::videobridge::java::JniEnvDeleter(attached ? jvm : nullptr));
    }
};

/**
 * @enum PixelFormat
 * @brief Supported target pixel formats for decoded video frame color conversion.
 */
enum class PixelFormat {
    /** 32-bit RGBA pixel format (8 bits per channel). */
    kRgba8888 = 0,
    /** 32-bit BGRA pixel format (8 bits per channel). */
    kBgra8888 = 1,
    /** Planar YUV 4:2:0 pixel format. */
    kI420 = 2,
};

/**
 * @class JniVideoSink
 * @brief Receives decoded video frames from WebRTC VideoTrack, converts pixel format using libyuv,
 * and passes them as zero-copy JNI DirectByteBuffers to Java callbacks.
 *
 * `JniVideoSink` implements `webrtc::VideoSinkInterface<webrtc::VideoFrame>`. On receiving a new
 * frame, it acquires a pooled native memory buffer from `FrameBufferPool`, performs color
 * conversion using `libyuv`, constructs a Java `com.android.emulator.webrtc.VideoFrame` object
 * containing a JNI `DirectByteBuffer`, and dispatches it to the registered `VideoFrameListener`
 * Java object.
 */
class JniVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
  public:
    /**
     * @brief Constructs a new JniVideoSink instance.
     * @param jvm Pointer to the Java Virtual Machine instance.
     * @param format Desired target pixel format for frame color conversion.
     * @param pool Shared memory buffer pool for zero-copy frame buffer allocation.
     */
    JniVideoSink(JavaVM* jvm, PixelFormat format, std::shared_ptr<FrameBufferPool> pool);

    /** @brief Destructor. Releases JNI global references cleanly. */
    ~JniVideoSink() override;

    /**
     * @brief Sets or updates the Java video frame listener callback object.
     * @param env JNI environment pointer.
     * @param listener Global/local reference to a Java `VideoFrameListener` instance, or nullptr to
     * unregister.
     */
    void SetListener(JNIEnv* env, jobject listener);

    /**
     * @brief Callback invoked by WebRTC when a new decoded video frame is ready.
     *
     * Performs color conversion via `libyuv`, wraps native buffer in a Java `DirectByteBuffer`, and
     * invokes Java listener callback.
     *
     * @param frame Decoded WebRTC video frame.
     */
    void OnFrame(const webrtc::VideoFrame& frame) override;

  private:
    JavaVM* jvm_;
    PixelFormat target_format_;
    std::shared_ptr<FrameBufferPool> pool_;

    absl::Mutex listener_mutex_;
    jobject listener_global_ref_ ABSL_GUARDED_BY(listener_mutex_) = nullptr;
    jclass video_frame_cls_ ABSL_GUARDED_BY(listener_mutex_) = nullptr;
    jmethodID video_frame_ctor_ ABSL_GUARDED_BY(listener_mutex_) = nullptr;
    jmethodID on_video_frame_mid_ ABSL_GUARDED_BY(listener_mutex_) = nullptr;
};

}  // namespace goldfish::videobridge::java
