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
#include "screen_record_commands.h"

#include <cerrno>
#include <filesystem>
#include <fstream>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"

#include "android/emulation/control/absl_status_translate.h"
#include "android/emulation/control/screen_recording_constants.h"
#include "android/status/status_macros.h"
#include "legacy_console_bridge.h"
#include "screen_recording_service.grpc.pb.h"
#include "screen_recording_service.pb.h"

namespace goldfish::telnet {

using android::emulation::control::GrpcStatusToAbslStatus;
using android::emulation::control::incubating::RecordingInfo;
using android::emulation::control::incubating::ScreenRecording;

using android::emulation::control::kMaxFPS;
using android::emulation::control::kMaxTimeLimit;
using android::emulation::control::kMaxVideoBitrate;
using android::emulation::control::kMinVideoBitrate;

namespace {

absl::Status FileIoErrorToStatus(int err, absl::string_view message) {
    if (err != 0) {
        return absl::ErrnoToStatus(err, message);
    }
    return absl::InternalError(message);
}

absl::StatusOr<uint32_t> ParseDisplayId(ArgStream& args, std::string_view example) {
    if (args.Empty()) {
        return absl::InvalidArgumentError(
                absl::StrCat("Missing value for '--display': expected a non-negative integer "
                             "display ID (e.g., '",
                             example, "')."));
    }
    const std::string raw_display = args.Peek();
    const absl::StatusOr<int> display_id = args.NextInt();
    if (!display_id.ok()) {
        return absl::InvalidArgumentError(
                absl::StrCat("Invalid '--display' value '", raw_display,
                             "': expected a non-negative integer display ID (e.g., '", example,
                             "'): ", display_id.status().message()));
    }
    if (*display_id < 0) {
        return absl::InvalidArgumentError(absl::StrCat(
                "Invalid '--display' value ", *display_id,
                ": display ID must be a non-negative integer (e.g., '", example, "')."));
    }
    return static_cast<uint32_t>(*display_id);
}

absl::Status ParseRecordingInfo(ArgStream& args, RecordingInfo* request) {
    std::string filename;

    while (!args.Empty()) {
        std::string token = args.Peek();
        if (token.starts_with("--")) {
            args.Next();  // consume option
            if (token == "--size") {
                if (args.Empty()) {
                    return absl::InvalidArgumentError(
                            "Missing value for '--size': expected '<width>x<height>' in pixels "
                            "(e.g., '--size 1280x720').");
                }
                const std::string size_str = args.Next();
                const std::vector<std::string_view> parts = absl::StrSplit(size_str, 'x');
                uint32_t width = 0;
                uint32_t height = 0;
                if (parts.size() != 2 || !absl::SimpleAtoi(parts[0], &width) ||
                    !absl::SimpleAtoi(parts[1], &height)) {
                    return absl::InvalidArgumentError(
                            absl::StrCat("Invalid '--size' value '", size_str,
                                         "', must be width x height in pixels (e.g., '--size "
                                         "1280x720')."));
                }
                if (width == 0 || height == 0) {
                    return absl::InvalidArgumentError(absl::StrCat(
                            "Invalid '--size' value ", width, "x", height,
                            ", width and height may not be zero; specify positive pixel "
                            "dimensions (e.g., '--size 1280x720')."));
                }
                request->set_width(width);
                request->set_height(height);
            } else if (token == "--bit-rate") {
                if (args.Empty()) {
                    return absl::InvalidArgumentError(absl::StrCat(
                            "Missing value for '--bit-rate': expected an integer bit rate in bps "
                            "or with an 'M' (Mbps) suffix within [",
                            kMinVideoBitrate, ", ", kMaxVideoBitrate,
                            "] bps (e.g., '--bit-rate 4M')."));
                }
                const std::string rate_str = args.Next();
                std::string_view rate_view(rate_str);
                const uint64_t multiplier =
                        absl::ConsumeSuffix(&rate_view, "M") ? 1000000ULL : 1ULL;
                uint32_t raw_bit_rate = 0;
                if (rate_view.empty() || !absl::SimpleAtoi(rate_view, &raw_bit_rate)) {
                    return absl::InvalidArgumentError(absl::StrCat(
                            "Invalid '--bit-rate' value '", rate_str,
                            "': expected an integer bit rate in bps or with an 'M' (Mbps) suffix "
                            "within [",
                            kMinVideoBitrate, ", ", kMaxVideoBitrate,
                            "] bps (e.g., '--bit-rate 4M' or '--bit-rate 4000000')."));
                }
                const uint64_t bit_rate = static_cast<uint64_t>(raw_bit_rate) * multiplier;
                if (bit_rate < kMinVideoBitrate || bit_rate > kMaxVideoBitrate) {
                    return absl::InvalidArgumentError(absl::StrCat(
                            "Bit rate ", bit_rate, "bps outside acceptable range [",
                            kMinVideoBitrate, ",", kMaxVideoBitrate, "] (e.g., '--bit-rate 4M')"));
                }
                request->set_bit_rate(static_cast<uint32_t>(bit_rate));
            } else if (token == "--time-limit") {
                if (args.Empty()) {
                    return absl::InvalidArgumentError(absl::StrCat(
                            "Missing value for '--time-limit': expected an integer duration in "
                            "seconds within [1, ",
                            kMaxTimeLimit, "] (e.g., '--time-limit 180')."));
                }
                const std::string raw_time_limit = args.Peek();
                const absl::StatusOr<int> time_limit = args.NextInt();
                if (!time_limit.ok()) {
                    return absl::InvalidArgumentError(absl::StrCat(
                            "Invalid '--time-limit' value '", raw_time_limit,
                            "': expected an integer duration in seconds within [1, ", kMaxTimeLimit,
                            "] (e.g., '--time-limit 180'): ", time_limit.status().message()));
                }
                if (*time_limit <= 0 || static_cast<uint32_t>(*time_limit) > kMaxTimeLimit) {
                    return absl::InvalidArgumentError(absl::StrCat("Time limit ", *time_limit,
                                                                   "s outside acceptable range [1,",
                                                                   kMaxTimeLimit, "]"));
                }
                request->set_time_limit(static_cast<uint32_t>(*time_limit));
            } else if (token == "--fps") {
                if (args.Empty()) {
                    return absl::InvalidArgumentError(
                            absl::StrCat("Missing value for '--fps': expected an integer frame "
                                         "rate in [1, ",
                                         kMaxFPS, "] fps (e.g., '--fps 24')."));
                }
                const std::string raw_fps = args.Peek();
                const absl::StatusOr<int> fps = args.NextInt();
                if (!fps.ok()) {
                    return absl::InvalidArgumentError(
                            absl::StrCat("Invalid '--fps' value '", raw_fps,
                                         "': expected an integer frame rate in [1, ", kMaxFPS,
                                         "] fps (e.g., '--fps 24'): ", fps.status().message()));
                }
                if (*fps <= 0 || static_cast<uint32_t>(*fps) > kMaxFPS) {
                    return absl::InvalidArgumentError(
                            absl::StrCat("FPS ", *fps, "fps outside acceptable range [1,", kMaxFPS,
                                         "] (e.g., '--fps 24')"));
                }
                request->set_fps(static_cast<uint32_t>(*fps));
            } else if (token == "--display") {
                ASSIGN_OR_RETURN(const uint32_t display_id, ParseDisplayId(args, "--display 0"));
                request->set_display(display_id);
            } else {
                return absl::InvalidArgumentError(
                        absl::StrCat("Unknown option '", token,
                                     "' for 'screenrecord start'. Supported options are '--size', "
                                     "'--bit-rate', '--time-limit', '--fps', and '--display' "
                                     "(see 'help screenrecord start')."));
            }
        } else {
            filename = std::string(args.Remaining());
            break;  // Assume filename is the rest of the arguments
        }
    }

    if (filename.empty()) {
        return absl::InvalidArgumentError(
                "Must provide an output filename. Usage: 'screenrecord start [options] <filename>' "
                "(see 'help screenrecord start').");
    }

    request->set_file_name(filename);
    return absl::OkStatus();
}

absl::Status ParseScreenshotArgs(ArgStream& args, uint32_t* display, std::filesystem::path* path) {
    std::string path_str;
    *display = 0;

    while (!args.Empty()) {
        std::string token = args.Peek();
        if (token.starts_with("--")) {
            args.Next();  // consume option
            if (token == "--display") {
                ASSIGN_OR_RETURN(*display, ParseDisplayId(args, "--display 0"));
            } else {
                return absl::InvalidArgumentError(absl::StrCat(
                        "Unknown option '", token,
                        "' for 'screenrecord screenshot'. Supported option is '--display <id>' "
                        "(see 'help screenrecord screenshot')."));
            }
        } else {
            path_str = std::string(args.Remaining());
            break;
        }
    }

    absl::Time now = absl::Now();
    std::string file_name = absl::StrFormat("Screenshot_%lld.png",
                                            static_cast<long long>(absl::ToUnixSeconds(now)));

    if (absl::EndsWith(path_str, ".png")) {
        *path = path_str;
    } else {
        *path = path_str.empty() ? std::filesystem::path(file_name)
                                 : (std::filesystem::path(path_str) / file_name);
    }
    return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<grpc::ClientContext>> NewRecordingContext(
        LegacyConsoleBridge::ConsoleContext& ctx) {
    return ctx.NewContext(std::chrono::system_clock::now() + std::chrono::seconds(10));
}
}  // namespace

void RegisterScreenRecordCommands(CommandRegistryBuilder::NodeBuilder& screenrecord) {
    screenrecord.On("start", "start screen recording",
                    [](LegacyConsoleBridge::ConsoleContext& ctx, ArgStream& args) -> absl::Status {
                        RecordingInfo request;
                        RETURN_IF_ERROR(ParseRecordingInfo(args, &request));

                        ASSIGN_OR_RETURN(auto stub, ctx.ScreenRecordingStub());
                        ASSIGN_OR_RETURN(auto context, NewRecordingContext(ctx));

                        RecordingInfo response;
                        const grpc::Status status =
                                stub->StartRecording(context.get(), request, &response);
                        if (!status.ok()) {
                            const absl::Status translated = GrpcStatusToAbslStatus(status);
                            return absl::Status(
                                    translated.code(),
                                    absl::StrCat("Failed to start screen recording to '",
                                                 request.file_name(), "': ", translated.message()));
                        }

                        return absl::OkStatus();
                    });

    screenrecord.On(
            "stop", "stop screen recording",
            [](LegacyConsoleBridge::ConsoleContext& ctx) -> absl::Status {
                ASSIGN_OR_RETURN(auto stub, ctx.ScreenRecordingStub());

                // List recordings
                ASSIGN_OR_RETURN(auto context_list, NewRecordingContext(ctx));
                RecordingInfo list_request;
                android::emulation::control::incubating::RecordingInfoList list_response;
                const grpc::Status status =
                        stub->ListRecordings(context_list.get(), list_request, &list_response);
                if (!status.ok()) {
                    const absl::Status translated = GrpcStatusToAbslStatus(status);
                    return absl::Status(translated.code(),
                                        absl::StrCat("Failed to list active screen recordings: ",
                                                     translated.message()));
                }

                // Stop each active recording
                absl::Status first_error = absl::OkStatus();
                for (const auto& info : list_response.recordings()) {
                    if (info.state() == android::emulation::control::incubating::RecordingInfo::
                                                RECORDER_STATE_RECORDING) {
                        ASSIGN_OR_RETURN(auto context_stop, NewRecordingContext(ctx));
                        RecordingInfo stop_response;
                        const grpc::Status status_stop =
                                stub->StopRecording(context_stop.get(), info, &stop_response);
                        if (!status_stop.ok()) {
                            const absl::Status translated = GrpcStatusToAbslStatus(status_stop);
                            LOG(WARNING) << "Failed to stop screen recording '" << info.file_name()
                                         << "' on display " << info.display() << ": " << translated;
                            if (first_error.ok()) {
                                first_error = absl::Status(
                                        translated.code(),
                                        absl::StrCat("Failed to stop screen recording '",
                                                     info.file_name(), "' on display ",
                                                     info.display(), ": ", translated.message()));
                            }
                        }
                    }
                }

                return first_error;
            });

    screenrecord.On(
            "screenshot" /* do_screenrecord_screenshot */, "Take a screenshot",
            "'screenrecord screenshot [options] <dirname>'\r\n"
            "\r\nTakes a single screenshot of emulator's display "
            "and saves the resulting PNG in <dirname>.\r\n"
            "\r\nOptions:\r\n"
            "  --display ID\r\n"
            "    Set display to take screenshot. Default is the device's "
            "main display ID = 0\r\n",
            [](LegacyConsoleBridge::ConsoleContext& ctx, ArgStream& args) -> absl::Status {
                uint32_t display;
                std::filesystem::path path;
                RETURN_IF_ERROR(ParseScreenshotArgs(args, &display, &path));

                ASSIGN_OR_RETURN(auto stub, ctx.EmulatorControllerStub());
                ASSIGN_OR_RETURN(auto context, ctx.NewContext());

                android::emulation::control::ImageFormat request;
                request.set_format(android::emulation::control::ImageFormat::PNG);
                request.set_display(display);

                android::emulation::control::Image response;
                const grpc::Status status = stub->getScreenshot(context.get(), request, &response);
                if (!status.ok()) {
                    const absl::Status translated = GrpcStatusToAbslStatus(status);
                    return absl::Status(translated.code(),
                                        absl::StrCat("Failed to capture screenshot for display ",
                                                     display, ": ", translated.message()));
                }

                errno = 0;
                std::ofstream file(path, std::ios::binary);
                if (!file.is_open()) {
                    const int err = errno;
                    return FileIoErrorToStatus(
                            err,
                            absl::StrCat("Failed to open screenshot output file '", path.string(),
                                         "' for writing. Verify that the target directory "
                                         "exists and is writable"));
                }
                errno = 0;
                file.write(response.image().data(),
                           static_cast<std::streamsize>(response.image().size()));
                file.flush();
                if (!file.good()) {
                    const int err = errno;
                    return FileIoErrorToStatus(
                            err,
                            absl::StrCat("Failed to write screenshot data (",
                                         response.image().size(), " bytes) to '", path.string(),
                                         "'. Check available disk space and file "
                                         "permissions"));
                }
                file.close();

                return absl::OkStatus();
            });
}

}  // namespace goldfish::telnet
