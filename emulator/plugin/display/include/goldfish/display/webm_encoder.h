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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace goldfish::display {

class WebMEncoder {
  public:
    // Maximum number of RGB24 frames buffered in the encoding queue before the
    // oldest unencoded frame is dropped and its buffer is reused.
    static constexpr size_t kMaxQueuedFrames = 2;

    WebMEncoder(std::string fname, int w, int h, int f, int br);
    ~WebMEncoder() ABSL_LOCKS_EXCLUDED(queue_mutex_);

    // Initializes FFmpeg resources and starts the background thread.
    // Returns true if successful, false otherwise.
    bool Init() ABSL_LOCKS_EXCLUDED(queue_mutex_);

    // Adds a frame to the encoding queue.
    void AddFrame(const uint8_t* rgb_data) ABSL_LOCKS_EXCLUDED(queue_mutex_);

    // Signals the encoder to stop and waits for completion.
    void Finish() ABSL_LOCKS_EXCLUDED(queue_mutex_);

    // Returns the number of frames sent to the codec so far. Finish writes
    // all of them to the file; frames still queued are dropped.
    uint64_t FramesEncoded() const { return frames_encoded_.load(); }

    // Returns the number of frames currently waiting in the encoding queue.
    size_t QueuedFrames() const ABSL_LOCKS_EXCLUDED(queue_mutex_);

    // Returns the total capacity in bytes of queued, in-flight, and pooled RGB
    // frame buffers.
    size_t BufferedBytes() const ABSL_LOCKS_EXCLUDED(queue_mutex_);

    // Returns the number of RGB frame buffer heap allocations performed by
    // AddFrame.
    uint64_t FrameBufferAllocations() const {
        return frame_buffer_allocations_.load(std::memory_order_relaxed);
    }

    // Returns the total capacity in bytes of the allocated YUV420P AVFrame
    // plane buffers used for encoding.
    size_t YuvFrameBufferBytes() const {
        return yuv_frame_buffer_bytes_.load(std::memory_order_relaxed);
    }

  private:
    bool FailInit(std::string_view reason, int av_err = 0) ABSL_LOCKS_EXCLUDED(queue_mutex_);
    void EncodeLoop() ABSL_LOCKS_EXCLUDED(queue_mutex_);
    void ProcessFrame(const std::vector<uint8_t>& raw_data, int64_t frame_index);
    void DrainPackets();
    void Cleanup() ABSL_LOCKS_EXCLUDED(queue_mutex_);
    bool IsFrameAvailable() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(queue_mutex_);

    // Config
    std::string filename_;
    int width_, height_, fps_, bitrate_;
    bool is_initialized_ = false;

    // FFmpeg Pointers
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;

    // Threading
    std::thread encoder_thread_;
    mutable absl::Mutex queue_mutex_;
    std::deque<std::vector<uint8_t>> frame_queue_ ABSL_GUARDED_BY(queue_mutex_);
    std::vector<std::vector<uint8_t>> free_buffers_ ABSL_GUARDED_BY(queue_mutex_);
    size_t in_flight_buffer_bytes_ ABSL_GUARDED_BY(queue_mutex_) = 0;
    std::atomic<bool> stop_signal_ ABSL_GUARDED_BY(queue_mutex_){false};
    std::atomic<bool> finished_{false};
    int64_t pts_counter_ = 0;
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frame_buffer_allocations_{0};
    std::atomic<size_t> yuv_frame_buffer_bytes_{0};
};
}  // namespace goldfish::display
