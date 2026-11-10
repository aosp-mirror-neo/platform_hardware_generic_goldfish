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
#include "emulator/plugin/grpc/input/dispatcher_session.h"

#include <utility>

#include "absl/log/check.h"
#include "absl/status/status.h"

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::InputEvent;

std::shared_ptr<IInputDispatcher> RequireDispatcher(std::shared_ptr<IInputDispatcher> dispatcher) {
    DCHECK(dispatcher != nullptr) << "DispatcherSession requires a non-null IInputDispatcher";
    return dispatcher;
}

void Reject(IInputDispatcher::DispatchCallback done, absl::Status status) {
    if (done) {
        std::move(done)({std::move(status)});
    }
}

std::shared_ptr<SessionResetTracker> EnsureResetTracker(IInputDispatcher::SessionOptions& options) {
    if (options.reset_tracker == nullptr) {
        options.reset_tracker = std::make_shared<SessionResetTracker>();
    }
    return options.reset_tracker;
}

}  // namespace

DispatcherSession::DispatcherSession(std::shared_ptr<IInputDispatcher> dispatcher,
                                     IInputDispatcher::SessionOptions options)
        : dispatcher_(RequireDispatcher(std::move(dispatcher)))
        , reset_tracker_(EnsureResetTracker(options))
        , id_(dispatcher_->OpenSession(std::move(options))) {}

DispatcherSession::~DispatcherSession() {
    dispatcher_->CloseSession(id_);
}

void DispatcherSession::Dispatch(const InputEvent& event, DispatchCallback done) {
    switch (event.event_case()) {
    case InputEvent::kPointer:
        dispatcher_->DispatchPointer(id_, event.pointer(), std::move(done));
        return;
    case InputEvent::kKey:
        dispatcher_->DispatchKey(id_, event.key(), std::move(done));
        return;
    case InputEvent::kText:
        dispatcher_->DispatchText(id_, event.text().text(), std::move(done));
        return;
    case InputEvent::kAxis:
        Reject(std::move(done),
               absl::UnimplementedError(
                       "Cannot dispatch axis input because no guest rotary or axis device is "
                       "registered; send pointer, key, or text events instead."));
        return;
    case InputEvent::EVENT_NOT_SET:
        Reject(std::move(done),
               absl::InvalidArgumentError(
                       "Cannot dispatch InputEvent because its event payload is unset; populate "
                       "pointer, key, or text before dispatching."));
        return;
    }
}

}  // namespace goldfish::grpc::v2
