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

#include "goldfish/avd/fake_avd_universe.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"

#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "input_service.pb.h"

namespace goldfish::avd {
namespace {

using ::absl_testing::IsOk;
using ::absl_testing::StatusIs;
using ::android::emulation::v2::PointerAction;
using ::android::emulation::v2::PointerEvent;
using ::goldfish::grpc::v2::IInputDispatcher;
using ::goldfish::grpc::v2::InputSessionId;

class FakeAvdUniverseTest : public ::testing::Test {
  protected:
    // Runs `dispatch` with a callback that records the status, then the loop.
    template <typename Dispatch>
    std::optional<absl::Status> Complete(Dispatch dispatch) {
        std::optional<absl::Status> result;
        dispatch([&result](IInputDispatcher::DispatchResult done) {
            result = std::move(done.status);
        });
        universe_.input_loop().RunAll();
        return result;
    }

    FakeAvdUniverse universe_;
    IInputDispatcher& dispatcher_ = *universe_.GetInputDispatcher();
};

TEST_F(FakeAvdUniverseTest, TextReachesNoKeyboardAndCompletesOk) {
    const InputSessionId session = dispatcher_.OpenSession({});

    EXPECT_THAT(Complete([&](IInputDispatcher::DispatchCallback done) {
                    dispatcher_.DispatchText(session, "hello 测试_🚀_é", std::move(done));
                }),
                testing::Optional(IsOk()));
}

TEST_F(FakeAvdUniverseTest, TouchHasNoDisplayAndCompletesUnavailable) {
    const InputSessionId session = dispatcher_.OpenSession({});
    PointerEvent touch;
    touch.set_action(PointerAction::POINTER_ACTION_DOWN);
    touch.add_pointers()->set_pointer_id(0);

    EXPECT_THAT(Complete([&](IInputDispatcher::DispatchCallback done) {
                    dispatcher_.DispatchPointer(session, touch, std::move(done));
                }),
                testing::Optional(StatusIs(absl::StatusCode::kUnavailable)));
}

TEST_F(FakeAvdUniverseTest, ShutdownIsIdempotentAndRejectsLaterEvents) {
    const InputSessionId session = dispatcher_.OpenSession({});
    dispatcher_.Shutdown();
    dispatcher_.Shutdown();

    EXPECT_THAT(Complete([&](IInputDispatcher::DispatchCallback done) {
                    dispatcher_.DispatchText(session, "late", std::move(done));
                }),
                testing::Optional(StatusIs(absl::StatusCode::kUnavailable)));
}

TEST_F(FakeAvdUniverseTest, OnPreGrpcShutdownCompletesPendingInputUnavailable) {
    const InputSessionId session = dispatcher_.OpenSession({});
    std::optional<absl::Status> result;
    // Stays pending: the input loop runs only when the test runs it.
    dispatcher_.DispatchText(
            session, "pending 测试_🚀_é",
            [&result](IInputDispatcher::DispatchResult done) { result = std::move(done.status); });
    ASSERT_FALSE(result.has_value());

    universe_.OnPreGrpcShutdown();

    EXPECT_THAT(result, testing::Optional(StatusIs(absl::StatusCode::kUnavailable)));
}

TEST(FakeAvdUniverseLifetimeTest, DispatcherHeldPastTheUniverseIsShutDown) {
    std::shared_ptr<IInputDispatcher> dispatcher;
    InputSessionId session = 0;
    {
        FakeAvdUniverse universe;
        dispatcher = universe.GetInputDispatcher();
        session = dispatcher->OpenSession({});
    }

    std::optional<absl::Status> result;
    dispatcher->DispatchText(session, "late", [&result](IInputDispatcher::DispatchResult done) {
        result = std::move(done.status);
    });
    dispatcher->CloseSession(session);

    EXPECT_THAT(result, testing::Optional(StatusIs(absl::StatusCode::kUnavailable)));
}

}  // namespace
}  // namespace goldfish::avd
