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
#include "goldfish/adb/adb_message_logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

#include "absl/log/log.h"
#include "absl/strings/str_format.h"

#include "goldfish/adb/adb_breadcrumb_tracker.h"
#include "goldfish/perfetto/perfetto_categories.h"

namespace goldfish::adb {

namespace {

constexpr size_t kMaxAdbPacketLength = size_t{1024} * 1024;
constexpr size_t kShortSnippetLength = 32;

bool IsValidHeader(const AMessage* message) {
    return (message->magic ^ 0xffffffffU) == message->command;
}

bool Accumulate(const char*& ptr, size_t& remaining, char* dest, size_t& dest_size, size_t needed) {
    if (dest_size >= needed) return true;
    const size_t to_copy = std::min(remaining, needed - dest_size);
    std::memcpy(dest + dest_size, ptr, to_copy);
    ptr += to_copy;
    remaining -= to_copy;
    dest_size += to_copy;
    return dest_size == needed;
}

bool AccumulateVector(const char*& ptr, size_t& remaining, std::vector<char>& dest, size_t needed) {
    const size_t present = dest.size();
    if (present >= needed) return true;
    const size_t to_copy = std::min(remaining, needed - present);
    dest.insert(dest.end(), ptr, ptr + to_copy);
    ptr += to_copy;
    remaining -= to_copy;
    return dest.size() == needed;
}

}  // namespace

template <typename Sink>
void AbslStringify(Sink& sink, const APacket& packet) {
    absl::Format(&sink, "{");  // Start of JSON object

    // Message fields
    absl::Format(&sink, "\"mesg\":{");
    absl::Format(&sink, "\"command\":\"%c%c%c%c\",",
                 static_cast<char>(packet.message.command & 0xFF),
                 static_cast<char>((packet.message.command >> 8) & 0xFF),
                 static_cast<char>((packet.message.command >> 16) & 0xFF),
                 static_cast<char>((packet.message.command >> 24) & 0xFF));
    absl::Format(&sink, "\"arg0\":0x%08x,", packet.message.arg0);
    absl::Format(&sink, "\"arg1\":0x%08x,", packet.message.arg1);
    absl::Format(&sink, "\"data_length\":%u,", packet.message.data_length);
    absl::Format(&sink, "\"data_check\":0x%08x,", packet.message.data_check);
    absl::Format(&sink, "\"magic\":0x%08x,\"magic_valid\":%s", packet.message.magic,
                 IsValidHeader(&packet.message) ? "true" : "false");
    absl::Format(&sink, "},");  // End of "mesg" object

    // Data field
    if (packet.message.data_length > 0) {
        absl::Format(&sink, "\"data\":\"");
        for (size_t i = 0; i < std::min<size_t>(packet.message.data_length, kMaxDataLogLength);
             ++i) {
            if (std::isprint(packet.data[i])) {
                absl::Format(&sink, "%c", packet.data[i]);
            } else {
                // Escape special characters for JSON
                switch (packet.data[i]) {
                case '"':
                    absl::Format(&sink, "\\\"");
                    break;
                case '\\':
                    absl::Format(&sink, "\\\\");
                    break;
                case '\b':
                    absl::Format(&sink, "\\b");
                    break;
                case '\f':
                    absl::Format(&sink, "\\f");
                    break;
                case '\n':
                    absl::Format(&sink, "\\n");
                    break;
                case '\r':
                    absl::Format(&sink, "\\r");
                    break;
                case '\t':
                    absl::Format(&sink, "\\t");
                    break;
                default:
                    absl::Format(&sink, "\\u%04x", packet.data[i]);
                }
            }
        }
        if (packet.message.data_length > kMaxDataLogLength) {
            absl::Format(&sink, "...<snip>");
        }
        absl::Format(&sink, "\"");
    }
    // End of JSON object
}

AdbMessageLogger::~AdbMessageLogger() {
    VLOG(1) << "~AdbMessageLogger()";
}

void AdbMessageLogger::Observe(const void* data, size_t size) {
    if (out_of_sync_) {
        return;
    }

    const char* ptr = static_cast<const char*>(data);
    size_t remaining = size;

    while (remaining > 0 && !out_of_sync_) {
        switch (parse_state_) {
        case ParseState::kExpectHeader: {
            if (!Accumulate(ptr, remaining, header_buf_, header_buf_size_, sizeof(AMessage))) {
                return;  // Need more data
            }
            std::memcpy(&header_, header_buf_, sizeof(AMessage));
            header_buf_size_ = 0;

            if (!IsValidHeader(&header_)) {
                ReportOutOfSync(
                        "ADB passive logging parser got out of sync and is disabling further ADB "
                        "logging.",
                        absl::StrFormat(
                                "Invalid header: cmd=0x%08x (magic=0x%08x, expected cmd=0x%08x)",
                                header_.command, header_.magic, header_.magic ^ 0xffffffffU));
                return;
            }

            if (header_.data_length > kMaxAdbPacketLength) {
                ReportOutOfSync(
                        "ADB passive logging parser detected an excessively large packet. "
                        "Disabling further ADB logging.",
                        absl::StrFormat(
                                "Packet too large: header.data_length is %u bytes (limit is 1MB)",
                                header_.data_length));
                return;
            }

            if (TRACE_EVENT_CATEGORY_ENABLED("qemu")) {
                const char cmd_str[4] = {
                    static_cast<char>(header_.command & 0xFF),
                    static_cast<char>((header_.command >> 8) & 0xFF),
                    static_cast<char>((header_.command >> 16) & 0xFF),
                    static_cast<char>((header_.command >> 24) & 0xFF),
                };
                TRACE_EVENT_INSTANT(
                        "qemu", "Adb::Packet", "dir", to_guest_ ? "to_guest" : "to_host", "cmd",
                        std::string_view(cmd_str, sizeof(cmd_str)), "arg0", header_.arg0, "arg1",
                        header_.arg1, "data_length", header_.data_length);
            }

            if (header_.data_length > 0) {
                const size_t max_snippet = verbose_ ? kMaxDataLogLength : kShortSnippetLength;
                snippet_needed_ = std::min<size_t>(header_.data_length, max_snippet);
                parse_state_ = ParseState::kExpectSnippet;
                snippet_accumulator_.clear();
            } else {
                EmitPacket(nullptr, 0);
            }
            break;
        }
        case ParseState::kExpectSnippet: {
            if (!AccumulateVector(ptr, remaining, snippet_accumulator_, snippet_needed_)) {
                return;  // Need more data
            }
            EmitPacket(snippet_accumulator_.data(), snippet_needed_);

            if (header_.data_length > snippet_needed_) {
                skip_remaining_ = header_.data_length - snippet_needed_;
                parse_state_ = ParseState::kSkipPayload;
            } else {
                parse_state_ = ParseState::kExpectHeader;
            }
            break;
        }
        case ParseState::kSkipPayload: {
            const size_t to_skip = std::min(remaining, skip_remaining_);
            ptr += to_skip;
            remaining -= to_skip;
            skip_remaining_ -= to_skip;

            if (skip_remaining_ == 0) {
                parse_state_ = ParseState::kExpectHeader;
            }
            break;
        }
        }
    }
}

void AdbMessageLogger::ReportOutOfSync(std::string_view summary, const std::string& reason) {
    out_of_sync_ = true;
    LOG(WARNING) << summary
                 << " Note: The actual guest/host ADB connection is unaffected and should work "
                    "normally. Details: "
                 << reason;
    if (callback_) {
        callback_->OnOutOfSync(reason, to_guest_);
    }
}

void AdbMessageLogger::EmitPacket(const char* snippet_data, size_t snippet_size) {
    if (verbose_) {
        std::vector<char> log_buf(sizeof(AMessage) + snippet_size);
        std::memcpy(log_buf.data(), &header_, sizeof(AMessage));
        if (snippet_size > 0) {
            std::memcpy(log_buf.data() + sizeof(AMessage), snippet_data, snippet_size);
        }
        const auto* packet = reinterpret_cast<const APacket*>(log_buf.data());
        LOG(INFO) << prefix_ << *packet;
    }
    if (callback_) {
        callback_->OnPacket(header_, snippet_data, to_guest_);
    }
}

AdbLogger::AdbLogger(int host_port, int guest_port, bool verbose, bool capture_snippets)
        : to_guest_(absl::StrFormat(">> (%d) ", guest_port), true, verbose)
        , to_host_(absl::StrFormat("<< (%d) ", host_port), false, verbose) {
#if defined(CONFIG_TRACE_GOLDFISH)
    tracker_ = std::make_unique<AdbBreadcrumbTracker>(capture_snippets);
    to_guest_.SetCallback(tracker_.get());
    to_host_.SetCallback(tracker_.get());
#endif
}

void AdbLogger::ToSocket(const void* data, size_t data_size) {
    to_guest_.Observe(data, data_size);
}
void AdbLogger::ToPlug(const void* data, size_t data_size) {
    to_host_.Observe(data, data_size);
}

}  // namespace goldfish::adb
