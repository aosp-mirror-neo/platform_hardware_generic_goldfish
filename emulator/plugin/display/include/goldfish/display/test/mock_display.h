// Copyright (C) 2025 The Android Open Source Project
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

#include <gmock/gmock.h>

#include "goldfish/display/display.h"
#include "goldfish/display/test/display_test_peer.h"

namespace goldfish::display::test {

// NOLINTBEGIN(modernize-type-traits)
class MockDisplay : public IDisplay {
  public:
    MockDisplay(EventLoop* loop, uint8_t id, uint32_t width, uint32_t height)
            : IDisplay(loop, id, width, height) {}
    MOCK_METHOD(absl::StatusOr<FrameInfo>, GetPixels,
                (PixelFormat, int, int, ImageRotation, uint8_t*, size_t*), (const, override));
    MOCK_METHOD(void, SendEvDevEvent, (uint16_t type, uint16_t code, uint32_t value), (override));
    MOCK_METHOD(void, SendEvDevReport, (EvDevEvents events), (override));
    MOCK_METHOD(void, WriteEvDevReportOnLoop, (absl::Span<const EvDevEvent> events), (override));
    MOCK_METHOD(bool, IsInputActive, (), (const, override));
    MOCK_METHOD(uint64_t, InputActiveGeneration, (), (const, override));
    MOCK_METHOD(void, OnDemand, (bool has_consumers), (override));
    MOCK_METHOD(void, OnFrameRequested, (), (override));

    const std::shared_ptr<DisplayFrames>& mutable_frames() {
        return DisplayTestPeer::frames(*this);
    }
};
// NOLINTEND(modernize-type-traits)

}  // namespace goldfish::display::test
