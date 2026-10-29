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

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "grpcpp/grpcpp.h"

#include "android/emulation/control/screen_recording_constants.h"
#include "goldfish/display/abstract_multi_display.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/video_recorder.h"
#include "screen_recording_service.grpc.pb.h"

using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

namespace android {
namespace emulation {
namespace control {
namespace incubating {

using android::emulation::control::kDefaultTimeLimit;
using android::emulation::control::kDefaultVideoBitrate;
using android::emulation::control::kFPS;
using android::emulation::control::kMaxFPS;
using android::emulation::control::kMaxTimeLimit;
using android::emulation::control::kMaxVideoBitrate;
using android::emulation::control::kMinVideoBitrate;

// Populates VideoRecorder's persistent RGB888 frame buffer from the active
// display without maintaining a duplicate full-frame copy.
class ScreenRecordingFrameGenerator {
  public:
    explicit ScreenRecordingFrameGenerator(::goldfish::display::SharedDisplay display);

    void operator()(uint8_t* pixels, int w, int h, int frame_index, double time);

    ::goldfish::display::FrameId LastFrameId() const { return last_frame_id_; }
    size_t BufferedBytes() const { return resize_scratch_.capacity(); }

  private:
    bool HasSameInitializedBuffer(const uint8_t* pixels, size_t dst_bytes) const;
    void RetainLastOrClear(uint8_t* pixels, size_t dst_bytes);
    void RecordFrame(::goldfish::display::FrameId frame_id, const uint8_t* pixels, size_t dst_bytes,
                     int frame_index);

    ::goldfish::display::SharedDisplay display_;
    ::goldfish::display::FrameId last_frame_id_ = 0;
    const uint8_t* initialized_buffer_ = nullptr;
    size_t initialized_bytes_ = 0;
    std::vector<uint8_t> resize_scratch_;
};

class ScreenRecordingServiceImpl final : public ScreenRecording::Service {
  public:
    ScreenRecordingServiceImpl(::goldfish::display::IMultiDisplay* display);

    // RPC: Start a new recording
    Status StartRecording(ServerContext* context, const RecordingInfo* request,
                          RecordingInfo* response) override;

    // RPC: Stop an active recording
    Status StopRecording(ServerContext* context, const RecordingInfo* request,
                         RecordingInfo* response) override;

    // RPC: List all known recordings
    Status ListRecordings(ServerContext* context, const RecordingInfo* request,
                          RecordingInfoList* response) override;

    // RPC: Stream events (e.g., status updates)
    Status ReceiveRecordingEvents(ServerContext* context, const google::protobuf::Empty* request,
                                  ServerWriter<RecordingInfo>* writer) override;

    // Returns the number of frames the active recording has encoded, or 0 if
    // nothing is recording.
    uint64_t FramesEncodedForTesting() ABSL_LOCKS_EXCLUDED(mu_);

  private:
    // Helper to validate request parameters
    bool ValidateRecordingParams(const RecordingInfo* info, std::string& error_msg);

    // Internal storage for active recordings
    // Key: file_name, Value: RecordingInfo
    std::map<std::string, RecordingInfo> recordings_ ABSL_GUARDED_BY(mu_);

    // Mutex to protect the recordings_ map
    absl::Mutex mu_;

    // Display manager to get hold of the requested display
    ::goldfish::display::IMultiDisplay& display_;

    // Recorer: for now, there is only one active recorder
    std::unique_ptr<::goldfish::display::VideoRecorder> recorder_;
    ::goldfish::display::IDisplay::ScopedFrameListener recording_listener_;
};

}  // namespace incubating
}  // namespace control
}  // namespace emulation
}  // namespace android
