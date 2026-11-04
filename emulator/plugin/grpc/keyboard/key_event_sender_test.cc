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

#include "android/emulation/control/keyboard/key_event_sender.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "android/emulation/control/keyboard/key_conversion.h"
#include "emulator_controller.pb.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "key_event_emitter.h"

namespace {

using ::android::emulation::control::keyboard::QemuKeyEvent;

// Records the QEMU keyboard state calls made by the sender, in order.
std::vector<std::string> g_kbd_calls;
std::vector<QemuKeyEvent> g_recorded_keys;

QKbdState* FakeKeyboardState() {
    static int storage;
    return reinterpret_cast<QKbdState*>(&storage);
}

}  // namespace

// Recording replacements for the QEMU input functions the keyboard libraries link against.
extern "C" {
QKbdState* qkbd_state_init(QemuConsole* /*con*/) {  // NOLINT(readability-identifier-naming)
    return FakeKeyboardState();
}

void qkbd_state_free(QKbdState* kbd) {  // NOLINT(readability-identifier-naming)
    g_kbd_calls.push_back(kbd == FakeKeyboardState() ? "free" : "free(unknown)");
}

void qkbd_state_key_event(  // NOLINT(readability-identifier-naming)
        QKbdState* kbd, QKeyCode qcode, bool down) {
    g_kbd_calls.push_back(kbd == FakeKeyboardState() ? "key" : "key(unknown)");
    if (kbd == FakeKeyboardState()) {
        g_recorded_keys.push_back({qcode, down});
    }
}

int qemu_input_linux_to_qcode(unsigned int /*lnx*/) {
    return 0;
}
}

namespace android::emulation::control::keyboard {
namespace {

using ::goldfish::async::testing::TestEventLoop;
using ::testing::ElementsAre;

std::vector<QemuKeyEvent> CollectUtf8Events(std::string_view utf8) {
    std::vector<QemuKeyEvent> events;
    EmitUtf8String(utf8, [&events](QemuKeyEvent ev) { events.push_back(ev); });
    return events;
}

class KeyEventSenderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        g_kbd_calls.clear();
        g_recorded_keys.clear();
    }

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
};

TEST_F(KeyEventSenderTest, QueuedKeyEventsRunBeforeKeyboardStateIsFreed) {
    {
        auto sender = createKeyEventSender(/*console=*/nullptr, loop_.get());
        KeyboardEvent event;
        event.set_codetype(KeyboardEvent::Evdev);
        event.set_eventtype(KeyboardEvent::keydown);
        event.set_keycode(30);
        sender->send(event);
        // The sender goes away while its key event is still queued on the loop.
    }
    loop_->RunAll();

    EXPECT_THAT(g_kbd_calls, ElementsAre("key", "free"));
}

TEST_F(KeyEventSenderTest, KeyboardStateIsFreedWithoutQueuedEvents) {
    {
        auto sender = createKeyEventSender(/*console=*/nullptr, loop_.get());
    }
    loop_->RunAll();

    EXPECT_THAT(g_kbd_calls, ElementsAre("free"));
}

TEST_F(KeyEventSenderTest, SendTextPreservesAsciiFollowingMultibyteUtf8) {
    {
        auto sender = createKeyEventSender(/*console=*/nullptr, loop_.get());
        KeyboardEvent event;
        event.set_text("a 🚀 2");
        sender->send(event);
    }
    loop_->RunAll();

    EXPECT_THAT(g_recorded_keys,
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}, QemuKeyEvent{Q_KEY_CODE_A, false},
                            QemuKeyEvent{Q_KEY_CODE_SPC, true}, QemuKeyEvent{Q_KEY_CODE_SPC, false},
                            QemuKeyEvent{Q_KEY_CODE_SPC, true}, QemuKeyEvent{Q_KEY_CODE_SPC, false},
                            QemuKeyEvent{Q_KEY_CODE_2, true}, QemuKeyEvent{Q_KEY_CODE_2, false}));
}

TEST_F(KeyEventSenderTest, EmitUtf8StringSkipsCjkEmojiAndLatin1WithoutSpuriousChords) {
    // Continuation byte 0xA1 in "á" (\xC3\xA1) coincides with kSkinKeyCharacterMap's Alt+1 fn
    // entry if treated as an unsigned single-byte character; only ASCII 'A' should be emitted.
    EXPECT_THAT(
            CollectUtf8Events("\xC3\xA1"
                              "A"),
            ElementsAre(QemuKeyEvent{Q_KEY_CODE_SHIFT, true}, QemuKeyEvent{Q_KEY_CODE_A, true},
                        QemuKeyEvent{Q_KEY_CODE_A, false}, QemuKeyEvent{Q_KEY_CODE_SHIFT, false}));

    EXPECT_THAT(
            CollectUtf8Events("测试_🚀_é!"),
            ElementsAre(QemuKeyEvent{Q_KEY_CODE_SHIFT, true}, QemuKeyEvent{Q_KEY_CODE_MINUS, true},
                        QemuKeyEvent{Q_KEY_CODE_MINUS, false},
                        QemuKeyEvent{Q_KEY_CODE_SHIFT, false}, QemuKeyEvent{Q_KEY_CODE_SHIFT, true},
                        QemuKeyEvent{Q_KEY_CODE_MINUS, true}, QemuKeyEvent{Q_KEY_CODE_MINUS, false},
                        QemuKeyEvent{Q_KEY_CODE_SHIFT, false}, QemuKeyEvent{Q_KEY_CODE_SHIFT, true},
                        QemuKeyEvent{Q_KEY_CODE_1, true}, QemuKeyEvent{Q_KEY_CODE_1, false},
                        QemuKeyEvent{Q_KEY_CODE_SHIFT, false}));
}

TEST_F(KeyEventSenderTest, EmitUtf8StringHandlesMalformedAndTrailingSequencesWithoutAborting) {
    EXPECT_THAT(CollectUtf8Events("az🚀"),
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}, QemuKeyEvent{Q_KEY_CODE_A, false},
                            QemuKeyEvent{Q_KEY_CODE_Z, true}, QemuKeyEvent{Q_KEY_CODE_Z, false}));

    // >= 4 consecutive continuation bytes and truncated multi-byte sequences must not trigger
    // DCHECK failures or swallow trailing ASCII bytes.
    EXPECT_THAT(CollectUtf8Events("\x80\x80\x80\x80\x80z"),
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_Z, true}, QemuKeyEvent{Q_KEY_CODE_Z, false}));
    EXPECT_THAT(CollectUtf8Events("\xF0\x80\x80\x80\x80z"),
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_Z, true}, QemuKeyEvent{Q_KEY_CODE_Z, false}));
    EXPECT_THAT(CollectUtf8Events("\xF0z"),
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_Z, true}, QemuKeyEvent{Q_KEY_CODE_Z, false}));
    EXPECT_THAT(CollectUtf8Events(std::string("a\0z", 3)),
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}, QemuKeyEvent{Q_KEY_CODE_A, false},
                            QemuKeyEvent{Q_KEY_CODE_Z, true}, QemuKeyEvent{Q_KEY_CODE_Z, false}));
}

// V2InputValidator accepts the code points for which IsTypeableCodePoint() is true, so text typing
// must type each of them and nothing else.
TEST_F(KeyEventSenderTest, EmitUtf8StringTypesExactlyTheTypeableAsciiCodePoints) {
    for (int code_point = 0; code_point <= 0x7F; ++code_point) {
        const std::string text(1, static_cast<char>(code_point));
        EXPECT_EQ(!CollectUtf8Events(text).empty(), IsTypeableCodePoint(code_point)) << code_point;
    }
}

}  // namespace
}  // namespace android::emulation::control::keyboard
