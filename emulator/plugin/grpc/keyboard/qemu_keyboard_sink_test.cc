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

#include "android/emulation/control/keyboard/qemu_keyboard_sink.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "android/emulation/control/keyboard/key_conversion.h"
#include "android/emulation/control/keyboard/key_event_sender.h"
#include "emulator_controller.pb.h"
#include "goldfish/async/event_loop.h"
#include "goldfish/async/testing/test_event_loop.h"
#include "standard-headers/linux/input-event-codes.h"

namespace {

using ::android::emulation::control::keyboard::QemuKeyEvent;

// The key events sent to QEMU's keyboard state, in order.
std::vector<QemuKeyEvent> g_sent_keys;
// Direct key-up and pacing events sent bypassing QKbdState during Reset().
struct DirectEvent {
    QemuConsole* console = nullptr;
    int qcode_or_delay = 0;
    bool is_delay = false;
    bool down = false;
};
std::vector<DirectEvent> g_direct_events;
// The number of times QEMU's keyboard state lifted all keys.
int g_lifts = 0;
// The number of times QEMU's keyboard state was freed.
int g_frees = 0;

QemuConsole* FakeConsole() {
    static int storage;
    return reinterpret_cast<QemuConsole*>(&storage);
}

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
    if (kbd == FakeKeyboardState()) {
        ++g_frees;
    }
}

void qkbd_state_key_event(  // NOLINT(readability-identifier-naming)
        QKbdState* kbd, QKeyCode qcode, bool down) {
    if (kbd == FakeKeyboardState()) {
        g_sent_keys.push_back({qcode, down});
    }
}

void qkbd_state_lift_all_keys(QKbdState* kbd) {  // NOLINT(readability-identifier-naming)
    if (kbd == FakeKeyboardState()) {
        ++g_lifts;
    }
}

void qemu_input_event_send_key_qcode(  // NOLINT(readability-identifier-naming)
        QemuConsole* src, QKeyCode q, bool down) {
    g_direct_events.push_back({src, static_cast<int>(q), /*is_delay=*/false, down});
}

void qemu_input_event_send_key_delay(uint32_t delay_ms) {  // NOLINT(readability-identifier-naming)
    g_direct_events.push_back({nullptr, static_cast<int>(delay_ms), /*is_delay=*/true, false});
}

// How qemu_input_linux_to_qcode() maps Linux key codes; unlisted codes are unmapped.
std::map<unsigned int, int> g_linux_to_qcode;

int qemu_input_linux_to_qcode(unsigned int lnx) {  // NOLINT(readability-identifier-naming)
    auto it = g_linux_to_qcode.find(lnx);
    return it == g_linux_to_qcode.end() ? Q_KEY_CODE_UNMAPPED : it->second;
}
}

namespace android::emulation::control::keyboard {
namespace {

using ::goldfish::async::EventLoop;
using ::goldfish::async::testing::TestEventLoop;
using ::testing::ElementsAre;

class QemuKeyboardSinkTest : public ::testing::Test {
  protected:
    void SetUp() override {
        g_sent_keys.clear();
        g_direct_events.clear();
        g_lifts = 0;
        g_frees = 0;
        // Only KEY_A, which is enough to see that WriteKey() translates its code.
        g_linux_to_qcode = {{KEY_A, Q_KEY_CODE_A}};
        sink_ = std::make_unique<QemuKeyboardSink>(FakeConsole(), *loop_);
    }

    void TearDown() override {
        RunOnLoop([this] { sink_.reset(); });
    }

    // Runs `task` on the loop thread and waits for it to finish.
    void RunOnLoop(EventLoop::Task task) {
        ASSERT_TRUE(loop_->Post(std::move(task)).ok());
        loop_->RunAll();
    }

    std::unique_ptr<TestEventLoop> loop_ = TestEventLoop::Create();
    std::unique_ptr<QemuKeyboardSink> sink_;
};

TEST_F(QemuKeyboardSinkTest, WriteKeySendsOneKeyEventBeforeReturning) {
    std::vector<QemuKeyEvent> sent_by_press;
    std::vector<QemuKeyEvent> sent_by_release;
    RunOnLoop([&] {
        sink_->WriteKey(KEY_A, /*down=*/true);
        sent_by_press = g_sent_keys;
        sink_->WriteKey(KEY_A, /*down=*/false);
        sent_by_release = g_sent_keys;
    });

    EXPECT_THAT(sent_by_press, ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}));
    EXPECT_THAT(sent_by_release,
                ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}, QemuKeyEvent{Q_KEY_CODE_A, false}));
}

TEST_F(QemuKeyboardSinkTest, TypeTextSendsTheKeyEventSenderSequenceBeforeReturning) {
    {
        auto sender = createKeyEventSender(/*console=*/nullptr, loop_.get());
        KeyboardEvent event;
        event.set_text("aB");
        sender->send(event);
    }
    loop_->RunAll();
    const std::vector<QemuKeyEvent> posted = g_sent_keys;
    g_sent_keys.clear();

    std::vector<QemuKeyEvent> typed;
    RunOnLoop([&] {
        sink_->TypeText("aB");
        typed = g_sent_keys;
    });

    EXPECT_EQ(typed, posted);
    EXPECT_THAT(
            typed,
            ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}, QemuKeyEvent{Q_KEY_CODE_A, false},
                        QemuKeyEvent{Q_KEY_CODE_SHIFT, true}, QemuKeyEvent{Q_KEY_CODE_B, true},
                        QemuKeyEvent{Q_KEY_CODE_B, false}, QemuKeyEvent{Q_KEY_CODE_SHIFT, false}));
}

TEST_F(QemuKeyboardSinkTest, TypeTextSkipsMultibyteUtf8WithoutDroppingSubsequentAscii) {
    std::vector<QemuKeyEvent> typed;
    RunOnLoop([&] {
        sink_->TypeText("a🚀 B");
        typed = g_sent_keys;
    });

    EXPECT_THAT(
            typed,
            ElementsAre(QemuKeyEvent{Q_KEY_CODE_A, true}, QemuKeyEvent{Q_KEY_CODE_A, false},
                        QemuKeyEvent{Q_KEY_CODE_SPC, true}, QemuKeyEvent{Q_KEY_CODE_SPC, false},
                        QemuKeyEvent{Q_KEY_CODE_SHIFT, true}, QemuKeyEvent{Q_KEY_CODE_B, true},
                        QemuKeyEvent{Q_KEY_CODE_B, false}, QemuKeyEvent{Q_KEY_CODE_SHIFT, false}));
}

TEST_F(QemuKeyboardSinkTest,
       ResetReinitializesHostStateAndReleasesEachMappedQcodeOnceInPacedBatches) {
    // Linux codes 101..150 map to qcodes 1..50, so Reset() fills two 24-key
    // batches. Another code maps to qcode 7 again, which is released once, and
    // KEY_MAX, the last Linux code, maps to qcode 51.
    g_linux_to_qcode.clear();
    for (int qcode = 1; qcode <= 50; ++qcode) {
        g_linux_to_qcode[100 + qcode] = qcode;
    }
    g_linux_to_qcode[200] = 7;
    g_linux_to_qcode[KEY_MAX] = 51;

    int frees_by_reset = 0;
    std::vector<DirectEvent> direct;
    RunOnLoop([&] {
        sink_->Reset();
        frees_by_reset = g_frees;
        direct = g_direct_events;
    });

    EXPECT_EQ(frees_by_reset, 1);
    EXPECT_TRUE(g_sent_keys.empty());
    // 51 distinct qcodes + 2 delays (after items 24 and 48) = 53 direct entries.
    ASSERT_EQ(direct.size(), 53U);
    EXPECT_TRUE(direct[24].is_delay);
    EXPECT_EQ(direct[24].qcode_or_delay, 1);
    EXPECT_TRUE(direct[49].is_delay);
    EXPECT_EQ(direct[49].qcode_or_delay, 1);

    std::vector<int> released_qcodes;
    for (const DirectEvent& ev : direct) {
        if (!ev.is_delay) {
            EXPECT_EQ(ev.console, FakeConsole());
            EXPECT_FALSE(ev.down);
            released_qcodes.push_back(ev.qcode_or_delay);
        }
    }
    EXPECT_EQ(released_qcodes.size(), 51U);
    EXPECT_EQ(released_qcodes.front(), 1);
    EXPECT_EQ(released_qcodes.back(), 51);
    EXPECT_EQ(std::ranges::count(released_qcodes, 7), 1);
}

TEST_F(QemuKeyboardSinkTest, DestructorFreesTheKeyboardState) {
    int frees_by_destructor = 0;
    RunOnLoop([&] {
        sink_.reset();
        frees_by_destructor = g_frees;
    });

    EXPECT_EQ(frees_by_destructor, 1);
}

}  // namespace
}  // namespace android::emulation::control::keyboard
