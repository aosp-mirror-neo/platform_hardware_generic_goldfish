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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "absl/time/time.h"

#include "emulator/plugin/grpc/input/input_dispatcher_interface.h"
#include "emulator/plugin/grpc/input/recording_input_dispatcher.h"
#include "input_service.pb.h"

namespace goldfish::grpc::v2 {
namespace {

using ::android::emulation::v2::InputEvent;
using ::goldfish::grpc::v2::testing::RecordingInputDispatcher;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using DispatchResult = IInputDispatcher::DispatchResult;

class DispatcherSessionTest : public ::testing::Test {
  protected:
    // Dispatches `event` on a new session and returns the result it completed
    // with on the calling thread, if it did.
    std::optional<DispatchResult> DispatchAndWait(const InputEvent& event) {
        DispatcherSession session(dispatcher_, {.human = true, .source_id = 0});
        std::optional<DispatchResult> result;
        session.Dispatch(event, [&result, caller = std::this_thread::get_id()](DispatchResult r) {
            EXPECT_EQ(std::this_thread::get_id(), caller);
            result = std::move(r);
        });
        return result;
    }

    std::shared_ptr<RecordingInputDispatcher> dispatcher_ =
            std::make_shared<RecordingInputDispatcher>();
};

TEST_F(DispatcherSessionTest, OpensWithOptionsAndClosesOnDestruction) {
    {
        DispatcherSession session(dispatcher_, {.human = true, .source_id = 42});
        ASSERT_TRUE(dispatcher_->opened_with.has_value());
        EXPECT_TRUE(dispatcher_->opened_with->human);
        EXPECT_EQ(dispatcher_->opened_with->source_id, 42);
        EXPECT_THAT(dispatcher_->calls, IsEmpty());
    }
    EXPECT_THAT(dispatcher_->calls, ElementsAre("close 7"));
}

TEST_F(DispatcherSessionTest, RoutesPointerKeyAndTextWithItsSessionId) {
    {
        DispatcherSession session(dispatcher_, {});
        InputEvent pointer;
        pointer.mutable_pointer()->set_display_id(2);
        InputEvent key;
        key.mutable_key()->set_dom_code("KeyA");
        InputEvent text;
        text.mutable_text()->set_text("hi 测试_🚀_é");

        session.Dispatch(pointer);
        session.Dispatch(key);
        session.Dispatch(text);
    }

    EXPECT_THAT(dispatcher_->calls,
                ElementsAre("pointer 7 display 2", "key 7 KeyA", "text 7 hi 测试_🚀_é", "close 7"));
}

TEST_F(DispatcherSessionTest, ForwardsTheCompletion) {
    InputEvent pointer;
    pointer.mutable_pointer();

    std::optional<DispatchResult> result = DispatchAndWait(pointer);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, absl::OkStatus());
    EXPECT_EQ(result->dispatched_at, RecordingInputDispatcher::kDispatchedAt);
}

TEST_F(DispatcherSessionTest, AxisIsUnimplementedWithoutReachingTheDispatcher) {
    InputEvent axis;
    axis.mutable_axis();

    std::optional<DispatchResult> result = DispatchAndWait(axis);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status.code(), absl::StatusCode::kUnimplemented);
    EXPECT_EQ(result->dispatched_at, std::nullopt);
    EXPECT_THAT(dispatcher_->calls, ElementsAre("close 7"));
}

TEST_F(DispatcherSessionTest, UnsetEventIsInvalidArgument) {
    std::optional<DispatchResult> result = DispatchAndWait(InputEvent());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status.code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(result->dispatched_at, std::nullopt);
    EXPECT_THAT(dispatcher_->calls, ElementsAre("close 7"));
}

TEST_F(DispatcherSessionTest, RejectsWithoutACallback) {
    DispatcherSession session(dispatcher_, {});

    session.Dispatch(InputEvent());
    InputEvent axis;
    axis.mutable_axis();
    session.Dispatch(axis);

    EXPECT_THAT(dispatcher_->calls, IsEmpty());
}

TEST_F(DispatcherSessionTest, SharesResetTrackerWithOpenedSessionOptions) {
    DispatcherSession session(dispatcher_, {});
    ASSERT_TRUE(dispatcher_->opened_with.has_value());
    ASSERT_NE(dispatcher_->opened_with->reset_tracker, nullptr);
    EXPECT_EQ(session.ResetTracker().Version(), 0U);

    dispatcher_->opened_with->reset_tracker->RecordDisplayReset(2);
    EXPECT_EQ(session.ResetTracker().Version(), 1U);
    const auto snapshot1 = session.ResetTracker().ReadSnapshot();
    EXPECT_EQ(snapshot1.full_generation, 0U);
    ASSERT_TRUE(snapshot1.display_generations.contains(2));
    EXPECT_EQ(snapshot1.display_generations.at(2), 1U);

    dispatcher_->opened_with->reset_tracker->RecordFullReset();
    EXPECT_EQ(session.ResetTracker().Version(), 2U);
    const auto snapshot2 = session.ResetTracker().ReadSnapshot();
    EXPECT_EQ(snapshot2.full_generation, 1U);
    EXPECT_THAT(snapshot2.display_generations, IsEmpty());
}

}  // namespace
}  // namespace goldfish::grpc::v2
