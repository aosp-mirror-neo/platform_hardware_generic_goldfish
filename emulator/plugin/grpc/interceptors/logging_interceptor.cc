// Copyright (C) 2019 The Android Open Source Project
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
#include "android/control/interceptor/logging_interceptor.h"

#include <algorithm>
#include <cinttypes>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"

// #define DEBUG 0
/* set  for very verbose debugging */
#ifndef DEBUG
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

namespace android::control::interceptor {

using grpc::experimental::ClientRpcInfo;
using grpc::experimental::InterceptorBatchMethods;
using grpc::experimental::ServerRpcInfo;

const std::array<std::string, 4> InvocationRecord::kTypes{"UNARY", "CLIENT_STREAMING",
                                                          "SERVER_STREAMING", "BIDI_STREAMING"};

static const std::array<std::string, 17> kStatus{
    "OK",        "CANCELLED",      "UNKNOWN",           "INVALID_ARGUMENT",   "DEADLINE_EXCEEDED",
    "NOT_FOUND", "ALREADY_EXISTS", "PERMISSION_DENIED", "RESOURCE_EXHAUSTED", "FAILED_PRECONDITION",
    "ABORTED",   "OUT_OF_RANGE",   "UNIMPLEMENTED",     "INTERNAL",           "UNAVAILABLE",
    "DATA_LOSS", "UNAUTHENTICATED"};

namespace {
uint64_t GetTimeDiffUs(const InvocationRecord& loginfo, InterceptionHookPoints from,
                       InterceptionHookPoints to) {
    DCHECK(loginfo.timestamps[static_cast<int>(to)] >= loginfo.timestamps[static_cast<int>(from)]);

    return loginfo.timestamps[static_cast<int>(to)] - loginfo.timestamps[static_cast<int>(from)];
}

void PrintLog(const InvocationRecord& loginfo) {
    auto status_msg = kStatus[std::min<int>(static_cast<int>(loginfo.status.error_code()), 16)];
    LOG(INFO) << "from: " << loginfo.peer
              << ", start: " << loginfo.timestamps[InvocationRecord::kStartTimeIdx]
              << ", rcvTime: " << loginfo.rcv_time << ", sndTime: " << loginfo.snd_time
              << ", rcv: " << loginfo.rcv_bytes << ", snd: " << loginfo.snd_bytes
              << ", rcv_cnt: " << loginfo.rcv_messages << ", snd_cnt: " << loginfo.snd_messages
              << ", " << status_msg << " " << loginfo.status.error_message() << ", "
              << loginfo.method << "(" << loginfo.incoming << ") -> [" << loginfo.response << "]";
}

template <typename RpcType>
CallType ToCallType(RpcType type) {
    switch (type) {
    case RpcType::UNARY:
        return CallType::kUnary;
    case RpcType::CLIENT_STREAMING:
        return CallType::kClientStreaming;
    case RpcType::SERVER_STREAMING:
        return CallType::kServerStreaming;
    case RpcType::BIDI_STREAMING:
        return CallType::kBidiStreaming;
    default:
        return CallType::kUnknown;
    }
}
}  // namespace

LoggingInterceptor::LoggingInterceptor(ServerRpcInfo* info, ReportingFunction reporter)
        : reporter_(std::move(reporter)) {
    if (info) {
        loginfo_.method = std::string(info->method()).substr(0, kMaxStringLen);
        loginfo_.type = ToCallType(info->type());
        loginfo_.direction = Direction::kIncoming;
    }
    loginfo_.timestamps[InvocationRecord::kStartTimeIdx] = absl::ToUnixMicros(absl::Now());
}

LoggingInterceptor::LoggingInterceptor(ClientRpcInfo* info, ReportingFunction reporter)
        : reporter_(std::move(reporter)) {
    if (info) {
        loginfo_.method = std::string(info->method()).substr(0, kMaxStringLen);
        loginfo_.type = ToCallType(info->type());
        loginfo_.direction = Direction::kOutgoing;
        loginfo_.peer = info->client_context()->peer();
    }
    loginfo_.timestamps[InvocationRecord::kStartTimeIdx] = absl::ToUnixMicros(absl::Now());
}

LoggingInterceptor::~LoggingInterceptor() {
    auto ts = absl::ToUnixMicros(absl::Now());
    loginfo_.duration = ts - loginfo_.timestamps[InvocationRecord::kStartTimeIdx];
    reporter_(loginfo_);
}

std::string LoggingInterceptor::FormatProtobufMessage(  // NOLINT
        const ::google::protobuf::Message* msg) const {
    std::string debug_string;

    ::google::protobuf::TextFormat::Printer printer;
    printer.SetSingleLineMode(true);
    printer.SetExpandAny(true);
    printer.SetTruncateStringFieldLongerThan(kMaxProtobufStrLen);
    if (!printer.PrintToString(*msg, &debug_string)) {
        return "<failed to format message>";
    }

    // Single line mode currently might have an extra space at the end.
    if (!debug_string.empty() && debug_string[debug_string.size() - 1] == ' ') {
        debug_string.resize(debug_string.size() - 1);
    }

    return debug_string;
}

std::string LoggingInterceptor::ChopStr(std::string str) const {  // NOLINT
    if (str.size() <= kMaxStringLen) {
        return str;
    }
    return str.substr(0, kMaxStringLen - 3) + "...";
}

/*
Creation of interceptor object
Phase: [POST_RECV_INITIAL_METADATA, POST_RECV_MESSAGE]
Method invocation inside server.
Phase: [PRE_SEND_INITIAL_METADATA, PRE_SEND_MESSAGE,PRE_SEND_STATUS]
Phase: [POST_SEND_MESSAGE]
Phase: [POST_RECV_CLOSE]
 */
void LoggingInterceptor::Intercept(InterceptorBatchMethods* methods) {
    auto ts = absl::ToUnixMicros(absl::Now());
    DD("Intercepting -- %d", ts);

    if (methods->QueryInterceptionHookPoint(InterceptionHookPoints::POST_RECV_MESSAGE)) {
        // Special case for streaming.. This is really just an approximation
        // of what is happening.. Increment the time spend on receiving
        // bytes..
        int selector = InvocationRecord::kStartTimeIdx;
        if (loginfo_.type == CallType::kClientStreaming ||
            loginfo_.type == CallType::kBidiStreaming) {
            selector = static_cast<int>(loginfo_.rcv_time == 0
                                                ? InterceptionHookPoints::POST_RECV_INITIAL_METADATA
                                                : InterceptionHookPoints::POST_RECV_MESSAGE);
        }

        loginfo_.rcv_time += (ts - loginfo_.timestamps[selector]);
    }

    // Note you can get many pre/post send in case of server streaming/bidi
    // Note you can get many pre/post recv in case of client streaming/bidi
    for (int i = 0; i < static_cast<int>(InterceptionHookPoints::NUM_INTERCEPTION_HOOKS); i++) {
        if (methods->QueryInterceptionHookPoint(static_cast<InterceptionHookPoints>(i))) {
            loginfo_.timestamps[i] = ts;
        }
    }

    if (methods->QueryInterceptionHookPoint(InterceptionHookPoints::POST_RECV_MESSAGE)) {
        // We just received a message from the client
        auto* msg = reinterpret_cast<::google::protobuf::Message*>(methods->GetRecvMessage());
        loginfo_.rcv_messages++;
        if (msg) {
            auto size = msg->SpaceUsedLong();
            if (size < kMaxProtobufMsgLogSize && loginfo_.rcv_bytes == 0) {
                loginfo_.incoming = FormatProtobufMessage(msg);
            }
            loginfo_.rcv_bytes += size;
        }
    }

    if (methods->QueryInterceptionHookPoint(InterceptionHookPoints::PRE_SEND_MESSAGE)) {
        // We are ready to ship this message overseas!
        const auto* msg =
                reinterpret_cast<const ::google::protobuf::Message*>(methods->GetSendMessage());
        if (msg) {
            auto size = msg->SpaceUsedLong();
            if (size < kMaxProtobufMsgLogSize && loginfo_.snd_bytes == 0) {
                loginfo_.response = FormatProtobufMessage(msg);
            }
            loginfo_.snd_bytes += size;
        }
    }
    if (methods->QueryInterceptionHookPoint(InterceptionHookPoints::PRE_SEND_STATUS)) {
        loginfo_.status = methods->GetSendStatus();
    }

    if (methods->QueryInterceptionHookPoint(InterceptionHookPoints::POST_SEND_MESSAGE)) {
        // Increment the time spend on sending bytes..
        loginfo_.snd_time += GetTimeDiffUs(loginfo_, InterceptionHookPoints::PRE_SEND_MESSAGE,
                                           InterceptionHookPoints::POST_SEND_MESSAGE);
        loginfo_.snd_messages++;
    }

    methods->Proceed();
}

LoggingInterceptorFactory::LoggingInterceptorFactory(ReportingFunction reporter)
        : reporter_(std::move(reporter)) {}

grpc::experimental::Interceptor* LoggingInterceptorFactory::CreateServerInterceptor(
        ServerRpcInfo* info) {
    DD("Creating a server interceptor!");
    return new LoggingInterceptor(info, reporter_);
}

grpc::experimental::Interceptor* LoggingInterceptorFactory::CreateClientInterceptor(
        ClientRpcInfo* info) {
    DD("Creating a client interceptor!");
    return new LoggingInterceptor(info, reporter_);
}

StdOutLoggingInterceptorFactory::StdOutLoggingInterceptorFactory()
        : LoggingInterceptorFactory(PrintLog) {}

}  // namespace android::control::interceptor
