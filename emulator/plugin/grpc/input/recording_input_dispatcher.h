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

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2::testing {

/**
 * @brief An `IInputDispatcher` test double that records every call and
 * completes each dispatch with OK at `kDispatchedAt` on the calling thread.
 *
 * Calls are recorded in `calls` as `"<method> <session> <payload>"`:
 * `"pointer <session> display <id>"`, `"key <session> <dom_code>"`,
 * `"text <session> <utf8>"`, `"close <session>"`, and `"shutdown"`.
 * `OpenSession()` stores its options in `opened_with` and always returns
 * `kSessionId`.
 *
 * ### Thread Safety:
 * Not thread-safe; for tests that drive the dispatcher from one thread.
 */
class RecordingInputDispatcher : public IInputDispatcher {
  public:
    static constexpr InputSessionId kSessionId = 7;
    static constexpr absl::Duration kDispatchedAt = absl::Nanoseconds(1234);

    ActivityEventSource& activity() override { return activity_; }
    void Shutdown() override { calls.emplace_back("shutdown"); }

    InputSessionId OpenSession(SessionOptions options) override {
        opened_with = options;
        return kSessionId;
    }

    void CloseSession(InputSessionId session) override {
        calls.push_back("close " + std::to_string(session));
    }

    void DispatchPointer(InputSessionId session, ::android::emulation::v2::PointerEvent event,
                         DispatchCallback done) override {
        calls.push_back("pointer " + std::to_string(session) + " display " +
                        std::to_string(event.display_id()));
        Complete(std::move(done));
    }

    void DispatchKey(InputSessionId session, ::android::emulation::v2::KeyEvent event,
                     DispatchCallback done) override {
        calls.push_back("key " + std::to_string(session) + " " + event.dom_code());
        Complete(std::move(done));
    }

    void DispatchText(InputSessionId session, std::string utf8, DispatchCallback done) override {
        calls.push_back("text " + std::to_string(session) + " " + utf8);
        Complete(std::move(done));
    }

    std::optional<SessionOptions> opened_with;
    std::vector<std::string> calls;

  private:
    static void Complete(DispatchCallback done) {
        if (done) {
            std::move(done)({absl::OkStatus(), kDispatchedAt});
        }
    }

    ActivityEventSource activity_;
};

}  // namespace goldfish::grpc::v2::testing
