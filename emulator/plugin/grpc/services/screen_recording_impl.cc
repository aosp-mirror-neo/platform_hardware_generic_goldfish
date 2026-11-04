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

#include "android/emulation/control/incubating/screen_recording_impl.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"

#include "android/emulation/control/absl_status_translate.h"
#include "goldfish/display/display_frames.h"
#include "goldfish/display/frame.h"
#include "goldfish/display/geometry.h"
#include "goldfish/file/file.h"
#include "goldfish/file/file_atomic.h"
#include "goldfish/perfetto/perfetto_categories.h"

namespace android {
namespace emulation {
namespace control {
namespace incubating {

namespace {

Status MakeGrpcStatusError(const absl::Status& cause, std::string_view context,
                           std::string_view remediation) {
    const std::string_view detail = cause.message();
    if (detail.empty()) {
        return Status(AbslStatusToGrpcStatus(cause).error_code(),
                      absl::StrCat(context, ". ", remediation));
    }
    const std::string_view separator = absl::EndsWith(detail, ".") ? " " : ". ";
    return Status(AbslStatusToGrpcStatus(cause).error_code(),
                  absl::StrCat(context, ": ", detail, separator, remediation));
}

Status MakePathResolutionError(const absl::Status& cause, std::string_view action,
                               std::string_view file_name) {
    return MakeGrpcStatusError(
            cause,
            absl::StrCat("Failed to ", action,
                         " screen recording: unable to resolve absolute path for '", file_name,
                         "'"),
            "Verify that the file path syntax is valid and the working directory is accessible.");
}

}  // namespace

ScreenRecordingFrameGenerator::ScreenRecordingFrameGenerator(
        ::goldfish::display::SharedDisplay display)
        : display_(std::move(display)) {}

bool ScreenRecordingFrameGenerator::HasSameInitializedBuffer(const uint8_t* pixels,
                                                             size_t dst_bytes) const {
    return initialized_buffer_ == pixels && initialized_bytes_ == dst_bytes;
}

void ScreenRecordingFrameGenerator::RetainLastOrClear(uint8_t* pixels, size_t dst_bytes) {
    if (HasSameInitializedBuffer(pixels, dst_bytes)) {
        return;
    }
    std::memset(pixels, 0, dst_bytes);
    last_frame_id_ = 0;
    initialized_buffer_ = pixels;
    initialized_bytes_ = dst_bytes;
}

void ScreenRecordingFrameGenerator::RecordFrame(::goldfish::display::FrameId frame_id,
                                                const uint8_t* pixels, size_t dst_bytes,
                                                int frame_index) {
    last_frame_id_ = frame_id;
    initialized_buffer_ = pixels;
    initialized_bytes_ = dst_bytes;
    TRACE_EVENT_INSTANT("display", "ScreenRecording::FrameGenerated",
                        perfetto::Flow::ProcessScoped(frame_id), "seq", frame_id, "frame_index",
                        frame_index);
}

void ScreenRecordingFrameGenerator::operator()(uint8_t* pixels, int w, int h, int frame_index,
                                               double time) {
    TRACE_EVENT("display", "ScreenRecording::GenerateFrame", "frame_index", frame_index, "time",
                time);
    if (pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const size_t dst_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (!display_) {
        RetainLastOrClear(pixels, dst_bytes);
        return;
    }

    auto latest = display_->AcquireLatestFrame();
    if (!latest.frame) {
        RetainLastOrClear(pixels, dst_bytes);
        return;
    }

    const auto frame_id = latest.frame->event().frame_id;
    if (frame_id == last_frame_id_ && HasSameInitializedBuffer(pixels, dst_bytes)) {
        return;
    }

    const auto f_dims = latest.frame->dimensions();
    if (f_dims.width == static_cast<uint32_t>(w) && f_dims.height == static_cast<uint32_t>(h)) {
        const ::goldfish::display::ReadRequest req{
            .rotation = ::goldfish::display::ImageRotation::kRotation0,
            .output = {0, 0},
        };
        const ::goldfish::display::Rgb888Target target{
            .plane =
                    ::goldfish::display::Plane{
                        .data = absl::MakeSpan(pixels, dst_bytes),
                        .stride = static_cast<size_t>(w) * 3,
                    },
        };
        if (latest.frame->ReadInto(req, target).ok()) {
            if (!resize_scratch_.empty()) {
                std::vector<uint8_t>().swap(resize_scratch_);
            }
            RecordFrame(frame_id, pixels, dst_bytes, frame_index);
        } else {
            RetainLastOrClear(pixels, dst_bytes);
        }
        return;
    }

    const uint32_t fit_w = std::min<uint32_t>(f_dims.width, static_cast<uint32_t>(w));
    const uint32_t fit_h = std::min<uint32_t>(f_dims.height, static_cast<uint32_t>(h));
    if (fit_w == 0 || fit_h == 0) {
        RetainLastOrClear(pixels, dst_bytes);
        return;
    }

    const ::goldfish::display::ReadRequest req{
        .rotation = ::goldfish::display::ImageRotation::kRotation0,
        .output = {fit_w, fit_h},
    };
    if (latest.frame
                ->ReadInto(req,
                           [&](const ::goldfish::display::ReadLayout& layout)
                                   -> absl::StatusOr<::goldfish::display::Rgb888Target> {
                               resize_scratch_.resize(layout.total_bytes);
                               return ::goldfish::display::Rgb888Target{
                                   .plane = {.data = absl::MakeSpan(resize_scratch_)},
                               };
                           })
                .ok()) {
        std::memset(pixels, 0, dst_bytes);
        const uint32_t off_x = (static_cast<uint32_t>(w) - fit_w) / 2;
        const uint32_t off_y = (static_cast<uint32_t>(h) - fit_h) / 2;
        for (uint32_t row = 0; row < fit_h; ++row) {
            std::memcpy(pixels + ((off_y + row) * static_cast<size_t>(w) + off_x) * 3,
                        resize_scratch_.data() + row * static_cast<size_t>(fit_w) * 3,
                        static_cast<size_t>(fit_w) * 3);
        }
        RecordFrame(frame_id, pixels, dst_bytes, frame_index);
    } else {
        RetainLastOrClear(pixels, dst_bytes);
    }
}

ScreenRecordingServiceImpl::ScreenRecordingServiceImpl(::goldfish::display::IMultiDisplay* display)
        : display_(*display) {}

Status ScreenRecordingServiceImpl::StartRecording(ServerContext* context,
                                                  const RecordingInfo* request,
                                                  RecordingInfo* response) {
    absl::MutexLock lock(mu_);

    if (recorder_) {
        return Status(
                grpc::StatusCode::ALREADY_EXISTS,
                absl::StrCat("Failed to start screen recording to '", request->file_name(),
                             "': a screen recording is already in progress. Call StopRecording "
                             "before starting a new recording."));
    }

    // 1. Prepare and apply defaults
    RecordingInfo configured_request = *request;
    if (configured_request.fps() == 0) configured_request.set_fps(kFPS);
    if (configured_request.bit_rate() == 0) configured_request.set_bit_rate(kDefaultVideoBitrate);
    if (configured_request.time_limit() == 0) configured_request.set_time_limit(kDefaultTimeLimit);

    // 2. Validate Input
    std::string error_msg;
    if (!ValidateRecordingParams(&configured_request, error_msg)) {
        return Status(grpc::StatusCode::INVALID_ARGUMENT, error_msg);
    }

    // 2b. Resolve to absolute path to return the complete path in response.
    auto abs_path = android::base::file::make_absolute(configured_request.file_name());
    if (!abs_path.ok()) {
        return MakePathResolutionError(abs_path.status(), "start", configured_request.file_name());
    }
    configured_request.set_file_name(abs_path->string());

    const uint32_t display_id = configured_request.display();
    auto screen = display_.GetDisplay(display_id);
    if (!screen.ok()) {
        LOG(WARNING) << "Failed to start screen recording to '" << configured_request.file_name()
                     << "' on display " << display_id << ": " << screen.status();
        return MakeGrpcStatusError(
                screen.status(),
                absl::StrCat("Failed to start screen recording to '",
                             configured_request.file_name(), "' on display ", display_id),
                absl::StrCat("Verify that display ", display_id,
                             " exists and is active, or use display 0 for the default display."));
    }
    auto display = screen->lock();
    if (!display) {
        LOG(WARNING) << "Failed to start screen recording to '" << configured_request.file_name()
                     << "' on display " << display_id << ": display reference has expired.";
        return Status(grpc::StatusCode::UNAVAILABLE,
                      absl::StrCat("Failed to start screen recording to '",
                                   configured_request.file_name(), "' on display ", display_id,
                                   ": display ", display_id,
                                   " is no longer active. Verify that display ", display_id,
                                   " has not been removed, or use display 0 for the default "
                                   "display."));
    }

    auto dims = display->GetDimensions();

    // 3. Check if already recording to this file
    auto it = recordings_.find(configured_request.file_name());
    if (it != recordings_.end() && it->second.state() == RecordingInfo::RECORDER_STATE_RECORDING) {
        return Status(grpc::StatusCode::ALREADY_EXISTS,
                      absl::StrCat("Failed to start screen recording: an active recording already "
                                   "exists for '",
                                   configured_request.file_name(),
                                   "'. Call StopRecording for this file or choose a different "
                                   "output path."));
    }

    // If a previous recording file already exists on disk, remove it first so that
    // subsequent recordings to the same staging path (e.g. in Studio or Extended Controls)
    // succeed while still creating the new file exclusively with restricted permissions.
    if (android::base::file::exists(configured_request.file_name())) {
        auto rm_status = android::base::file::rm(configured_request.file_name());
        if (!rm_status.ok()) {
            LOG(WARNING) << "Failed to start screen recording: unable to remove existing "
                            "recording file '"
                         << configured_request.file_name() << "': " << rm_status;
            return MakeGrpcStatusError(
                    rm_status,
                    absl::StrCat("Failed to start screen recording: unable to remove existing "
                                 "recording file '",
                                 configured_request.file_name(), "'"),
                    "Ensure the file is not open in another process and that the emulator has "
                    "write permissions for its parent directory.");
        }
    }

    // Create the file exclusively to prevent TOCTOU race and ensure restricted permissions.
    auto file_status =
            android::base::file::CreatePrivateFileExclusive(configured_request.file_name(), "");
    if (!file_status.ok()) {
        LOG(ERROR) << "Failed to start screen recording: unable to create recording file '"
                   << configured_request.file_name() << "' exclusively: " << file_status;
        return MakeGrpcStatusError(
                file_status,
                absl::StrCat("Failed to start screen recording: unable to create recording file '",
                             configured_request.file_name(), "'"),
                "Ensure the parent directory exists, the emulator has write permissions, and "
                "sufficient disk space is available.");
    }

    // 4. Start the Recorder
    const int kFps = configured_request.fps();
    const int kBitRate = configured_request.bit_rate();
    const int kSeconds = configured_request.time_limit();

    recording_listener_ = display->AddFrameListener([](const ::goldfish::display::FrameEvent&) {});
    if (!display->AcquireLatestFrame().frame) {
        (void)display->WaitForFreshFrame(absl::Now() + absl::Milliseconds(500));
    }

    // We just ignore the width and height from request
    const int width2 = dims.width;
    const int height2 = dims.height;
    recorder_.reset(
            new ::goldfish::display::VideoRecorder(width2, height2, kFps, kBitRate, kSeconds));
    if (!recorder_->Start(configured_request.file_name(),
                          ScreenRecordingFrameGenerator(std::move(display)))) {
        recorder_.reset();
        recording_listener_.reset();
        LOG(ERROR) << "Failed to start screen recording to '" << configured_request.file_name()
                   << "' on display " << display_id << " (" << width2 << "x" << height2 << " at "
                   << configured_request.fps() << " FPS, " << configured_request.bit_rate()
                   << " bps): video encoder initialization failed.";
        return Status(
                grpc::StatusCode::INTERNAL,
                absl::StrCat("Failed to start screen recording to '",
                             configured_request.file_name(), "' on display ", display_id,
                             ": unable to initialize WebM video encoder for ", width2, "x", height2,
                             " at ", configured_request.fps(), " FPS and ",
                             configured_request.bit_rate(),
                             " bps. Verify that display dimensions are valid and check emulator "
                             "logs for encoder errors."));
    }

    // 5. Update State
    configured_request.set_state(RecordingInfo::RECORDER_STATE_RECORDING);

    // Store it
    recordings_[configured_request.file_name()] = configured_request;

    // 6. Prepare Response
    *response = configured_request;

    LOG(INFO) << "Started screen recording. File: " << configured_request.file_name()
              << ", Display: " << configured_request.display()
              << ", FPS: " << configured_request.fps()
              << ", Bitrate: " << configured_request.bit_rate();
    return Status::OK;
}

Status ScreenRecordingServiceImpl::StopRecording(ServerContext* context,
                                                 const RecordingInfo* request,
                                                 RecordingInfo* response) {
    absl::MutexLock lock(mu_);

    if (!recorder_) {
        return Status(
                grpc::StatusCode::NOT_FOUND,
                absl::StrCat("Failed to stop screen recording for '", request->file_name(),
                             "': no screen recording is currently active. Call StartRecording "
                             "before calling StopRecording."));
    }

    auto abs_path = android::base::file::make_absolute(request->file_name());
    if (!abs_path.ok()) {
        return MakePathResolutionError(abs_path.status(), "stop", request->file_name());
    }
    const std::string resolved_path = abs_path->string();
    auto it = recordings_.find(resolved_path);
    if (it == recordings_.end()) {
        return Status(grpc::StatusCode::NOT_FOUND,
                      absl::StrCat("Failed to stop screen recording: no recording found for '",
                                   resolved_path,
                                   "'. Verify the recording file path or call ListRecordings to "
                                   "inspect active recordings."));
    }

    // Update state to STOPPED
    it->second.set_state(RecordingInfo::RECORDER_STATE_STOPPED);
    *response = it->second;

    recorder_->Stop();
    recorder_.reset();
    recording_listener_.reset();

    LOG(INFO) << "Stopped screen recording for file: " << resolved_path;
    return Status::OK;
}

Status ScreenRecordingServiceImpl::ListRecordings(ServerContext* context,
                                                  const RecordingInfo* request,
                                                  RecordingInfoList* response) {
    absl::MutexLock lock(mu_);

    for (const auto& pair : recordings_) {
        // We add the recording to the repeated field list
        RecordingInfo* info = response->add_recordings();
        *info = pair.second;
    }

    return Status::OK;
}

Status ScreenRecordingServiceImpl::ReceiveRecordingEvents(ServerContext* context,
                                                          const google::protobuf::Empty* request,
                                                          ServerWriter<RecordingInfo>* writer) {
    // We just stream the current status of all recordings
    // once and then exit.

    absl::MutexLock lock(mu_);

    for (const auto& pair : recordings_) {
        // Write to the stream
        if (!writer->Write(pair.second)) {
            // Stream closed or error
            return Status(
                    grpc::StatusCode::CANCELLED,
                    absl::StrCat("Screen recording event stream closed while sending status for '",
                                 pair.first, "'. Reconnect to ReceiveRecordingEvents to resume."));
        }
    }

    return Status::OK;
}

uint64_t ScreenRecordingServiceImpl::FramesEncodedForTesting() {
    absl::MutexLock lock(mu_);
    return recorder_ ? recorder_->FramesEncoded() : 0;
}

bool ScreenRecordingServiceImpl::ValidateRecordingParams(const RecordingInfo* info,
                                                         std::string& error_msg) {
    // Check file extension
    const std::string& fname = info->file_name();
    if (!absl::EndsWith(fname, ".webm")) {
        error_msg = absl::StrCat(
                "Failed to start screen recording: output file name '", fname,
                "' must end with '.webm'. Specify a destination path with a '.webm' extension.");
        return false;
    }

    // Check FPS range
    if (info->fps() < 1 || info->fps() > kMaxFPS) {
        error_msg =
                absl::StrCat("Failed to start screen recording to '", fname, "': FPS ", info->fps(),
                             " is out of range [1, ", kMaxFPS, "]. Specify an FPS between 1 and ",
                             kMaxFPS, " (or 0 to use the default ", kFPS, " FPS).");
        return false;
    }

    // Check Bitrate (100k - 25M)
    if (info->bit_rate() < kMinVideoBitrate || info->bit_rate() > kMaxVideoBitrate) {
        error_msg = absl::StrCat("Failed to start screen recording to '", fname, "': bitrate ",
                                 info->bit_rate(), " bps is out of range [", kMinVideoBitrate, ", ",
                                 kMaxVideoBitrate, "]. Specify a bitrate between ",
                                 kMinVideoBitrate, " and ", kMaxVideoBitrate,
                                 " bps (or 0 to use the default ", kDefaultVideoBitrate, " bps).");
        return false;
    }

    // Check Time Limit range
    if (info->time_limit() < 1 || info->time_limit() > kMaxTimeLimit) {
        error_msg =
                absl::StrCat("Failed to start screen recording to '", fname, "': time limit ",
                             info->time_limit(), "s is out of range [1, ", kMaxTimeLimit,
                             "]. Specify a time limit between 1 and ", kMaxTimeLimit,
                             " seconds (or 0 to use the default ", kDefaultTimeLimit, " seconds).");
        return false;
    }

    return true;
}

}  // namespace incubating
}  // namespace control
}  // namespace emulation
}  // namespace android
