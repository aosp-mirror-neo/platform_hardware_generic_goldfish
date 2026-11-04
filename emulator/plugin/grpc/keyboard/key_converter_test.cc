#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "android/emulation/control/keyboard/key_conversion.h"
#include "dom_key.h"
#include "standard-headers/linux/input-event-codes.h"

namespace android {
namespace emulation {
namespace control {
namespace keyboard {

// Function to check if a QKeyCode is valid
bool IsValidQKeyCode(QKeyCode code) {
    // This is a simple check based on the range of valid QKeyCode values.
    // You might need to adjust this based on the actual QKeyCode definition.
    return code > 0 && code <= Q_KEY_CODE__MAX;
}

TEST(KeyConversionTest, DISABLED_EvdevToQKeyCode) {
    // We do not have a complete mapping. This test is mainly here
    // to point out that some of our defined key codes do not have
    // a corresponding QKeyCode. If these codes are crucial for correct
    // functioning of android we will have to update
    // external/keycodemapdb/data/keymaps.csv

    // Get the keymap
    const std::vector<KeycodeMapEntry>& map = keymap();
    int idx = 0;
    // Iterate through all entries in the keymap
    for (const auto& entry : map) {
        // Check if there is a valid evdev code
        if (entry.evdev != 0) {
            idx++;
            // Translate the evdev code to QKeyCode
            QKeyCode qcode = evdev_to_qcode(entry.evdev);

            // Check if the QKeyCode is valid
            EXPECT_TRUE(IsValidQKeyCode(qcode))
                    << "Invalid QKeyCode " << static_cast<int>(qcode) << " for usb code 0x"
                    << std::hex << entry.usb << std::dec << " for evdev code 0x" << std::hex
                    << entry.evdev << std::dec << " (" << entry.code << ") at index " << idx;
        }
    }
}

TEST(KeyConversionTest, HandlesEvdevBit11) {
    // 102 is KEY_HOME in evdev.
    // 1126 is 102 | 0x400 (bit 11 set).
    uint32_t evdev_102 = keycode_to_evdev(102, KeyCodeType::evdev);
    uint32_t evdev_1126 = keycode_to_evdev(1126, KeyCodeType::evdev);

    EXPECT_EQ(evdev_102, 102U);
    EXPECT_EQ(evdev_102, evdev_1126);
}

TEST(KeyConversionTest, AsciiToQcodeLowercase) {
    auto down_events = ascii_to_qcode('e', true);
    ASSERT_EQ(down_events.size(), 1);
    EXPECT_EQ(down_events[0].code, Q_KEY_CODE_E);
    EXPECT_TRUE(down_events[0].down);
    EXPECT_TRUE(IsValidQKeyCode(down_events[0].code));

    auto up_events = ascii_to_qcode('e', false);
    ASSERT_EQ(up_events.size(), 1);
    EXPECT_EQ(up_events[0].code, Q_KEY_CODE_E);
    EXPECT_FALSE(up_events[0].down);
    EXPECT_TRUE(IsValidQKeyCode(up_events[0].code));
}

TEST(KeyConversionTest, AsciiToQcodeUppercaseLetters) {
    // 'E' requires Shift + E
    auto down_events = ascii_to_qcode('E', true);
    ASSERT_EQ(down_events.size(), 2);
    EXPECT_EQ(down_events[0].code, Q_KEY_CODE_SHIFT);
    EXPECT_TRUE(down_events[0].down);
    EXPECT_EQ(down_events[1].code, Q_KEY_CODE_E);
    EXPECT_TRUE(down_events[1].down);
    for (const auto& ev : down_events) {
        EXPECT_TRUE(IsValidQKeyCode(ev.code));
    }

    auto up_events = ascii_to_qcode('E', false);
    ASSERT_EQ(up_events.size(), 2);
    EXPECT_EQ(up_events[0].code, Q_KEY_CODE_E);
    EXPECT_FALSE(up_events[0].down);
    EXPECT_EQ(up_events[1].code, Q_KEY_CODE_SHIFT);
    EXPECT_FALSE(up_events[1].down);
    for (const auto& ev : up_events) {
        EXPECT_TRUE(IsValidQKeyCode(ev.code));
    }
}

TEST(KeyConversionTest, AsciiToQcodeFnKey) {
    // 'a' has fn = 0xe1 -> Alt + A
    auto down_events = ascii_to_qcode(0xe1, true);
    ASSERT_EQ(down_events.size(), 2);
    EXPECT_EQ(down_events[0].code, Q_KEY_CODE_ALT);
    EXPECT_TRUE(down_events[0].down);
    EXPECT_EQ(down_events[1].code, Q_KEY_CODE_A);
    EXPECT_TRUE(down_events[1].down);

    auto up_events = ascii_to_qcode(0xe1, false);
    ASSERT_EQ(up_events.size(), 2);
    EXPECT_EQ(up_events[0].code, Q_KEY_CODE_A);
    EXPECT_FALSE(up_events[0].down);
    EXPECT_EQ(up_events[1].code, Q_KEY_CODE_ALT);
    EXPECT_FALSE(up_events[1].down);
}

TEST(KeyConversionTest, AsciiToQcodeCapsFnKey) {
    // 'a' has caps_fn = 0xc1 -> Shift + Alt + A
    auto down_events = ascii_to_qcode(0xc1, true);
    ASSERT_EQ(down_events.size(), 3);
    EXPECT_EQ(down_events[0].code, Q_KEY_CODE_SHIFT);
    EXPECT_TRUE(down_events[0].down);
    EXPECT_EQ(down_events[1].code, Q_KEY_CODE_ALT);
    EXPECT_TRUE(down_events[1].down);
    EXPECT_EQ(down_events[2].code, Q_KEY_CODE_A);
    EXPECT_TRUE(down_events[2].down);

    auto up_events = ascii_to_qcode(0xc1, false);
    ASSERT_EQ(up_events.size(), 3);
    EXPECT_EQ(up_events[0].code, Q_KEY_CODE_A);
    EXPECT_FALSE(up_events[0].down);
    EXPECT_EQ(up_events[1].code, Q_KEY_CODE_ALT);
    EXPECT_FALSE(up_events[1].down);
    EXPECT_EQ(up_events[2].code, Q_KEY_CODE_SHIFT);
    EXPECT_FALSE(up_events[2].down);
}

TEST(KeyConversionTest, AsciiToQcodeUnmappedReturnsEmpty) {
    auto events = ascii_to_qcode(0xFFFF, true);
    EXPECT_TRUE(events.empty());
}

TEST(KeyConversionTest, AsciiToQcodeAllAlphabetLettersValid) {
    for (char c = 'A'; c <= 'Z'; ++c) {
        auto events = ascii_to_qcode(c, true);
        ASSERT_EQ(events.size(), 2) << "Failed for uppercase letter " << c;
        EXPECT_EQ(events[0].code, Q_KEY_CODE_SHIFT);
        EXPECT_TRUE(IsValidQKeyCode(events[1].code));
    }
    for (char c = 'a'; c <= 'z'; ++c) {
        auto events = ascii_to_qcode(c, true);
        ASSERT_EQ(events.size(), 1) << "Failed for lowercase letter " << c;
        EXPECT_TRUE(IsValidQKeyCode(events[0].code));
    }
}

TEST(KeyConversionTest, TypeableCodePointsArePrintableAsciiNewlineAndTab) {
    for (int32_t code_point = ' '; code_point <= '~'; ++code_point) {
        EXPECT_TRUE(IsTypeableCodePoint(code_point)) << code_point;
    }
    EXPECT_TRUE(IsTypeableCodePoint('\n'));
    EXPECT_TRUE(IsTypeableCodePoint('\t'));

    // 0xAB ('«') has an Alt mapping in the key map but is not ASCII, so typing skips it.
    constexpr int32_t kUntypeable[] = {-1, 0, '\b', '\r', 0x1B, 0x7F, 0xAB, 0xE9, 0x6D4B, 0x1F680};
    for (int32_t code_point : kUntypeable) {
        EXPECT_FALSE(IsTypeableCodePoint(code_point)) << code_point;
    }
}

TEST(KeyConversionTest, DomCodeToEvdevStandardKeys) {
    EXPECT_EQ(dom_code_to_evdev("KeyA"), 0x001eU);       // KEY_A
    EXPECT_EQ(dom_code_to_evdev("KeyB"), 0x0030U);       // KEY_B
    EXPECT_EQ(dom_code_to_evdev("Digit1"), 0x0002U);     // KEY_1
    EXPECT_EQ(dom_code_to_evdev("Enter"), 0x001cU);      // KEY_ENTER
    EXPECT_EQ(dom_code_to_evdev("Backspace"), 0x000eU);  // KEY_BACKSPACE
    EXPECT_EQ(dom_code_to_evdev("Escape"), 0x0001U);     // KEY_ESC
    EXPECT_EQ(dom_code_to_evdev("Space"), 0x0039U);      // KEY_SPACE
    EXPECT_EQ(dom_code_to_evdev("ShiftLeft"), 0x002aU);  // KEY_LEFTSHIFT
    EXPECT_EQ(dom_code_to_evdev("Power"), 0x0074U);      // KEY_POWER
    EXPECT_EQ(dom_code_to_evdev("GoHome"), 0x0066U);     // KEY_HOME
    EXPECT_EQ(dom_code_to_evdev("GoBack"), 0x009eU);     // KEY_BACK
    EXPECT_EQ(dom_code_to_evdev("AppSwitch"), 0x0244U);  // KEY_APPSELECT
}

TEST(KeyConversionTest, DomCodeToEvdevAliasesAndUnknown) {
    // Aliases
    EXPECT_EQ(dom_code_to_evdev("VolumeUp"), 0x0073U);         // KEY_VOLUMEUP
    EXPECT_EQ(dom_code_to_evdev("AudioVolumeUp"), 0x0073U);    // KEY_VOLUMEUP
    EXPECT_EQ(dom_code_to_evdev("VolumeDown"), 0x0072U);       // KEY_VOLUMEDOWN
    EXPECT_EQ(dom_code_to_evdev("AudioVolumeDown"), 0x0072U);  // KEY_VOLUMEDOWN
    EXPECT_EQ(dom_code_to_evdev("VolumeMute"), 0x0071U);       // KEY_MUTE
    EXPECT_EQ(dom_code_to_evdev("AudioVolumeMute"), 0x0071U);  // KEY_MUTE
    EXPECT_EQ(dom_code_to_evdev("Back"), 0x009eU);             // KEY_BACK
    EXPECT_EQ(dom_code_to_evdev("Home"), 0x01c4U);             // KEY_HOMEPAGE

    // Empty and unknown
    EXPECT_EQ(dom_code_to_evdev(""), 0U);
    EXPECT_EQ(dom_code_to_evdev("NonExistentKey"), 0U);
}

TEST(KeyConversionTest, AndroidKeycodeToEvdevSystemKeys) {
    EXPECT_EQ(android_keycode_to_evdev(3), static_cast<uint32_t>(KEY_HOMEPAGE));     // HOME
    EXPECT_EQ(android_keycode_to_evdev(4), static_cast<uint32_t>(KEY_BACK));         // BACK
    EXPECT_EQ(android_keycode_to_evdev(26), static_cast<uint32_t>(KEY_POWER));       // POWER
    EXPECT_EQ(android_keycode_to_evdev(187), static_cast<uint32_t>(KEY_APPSELECT));  // APP_SWITCH
}

TEST(KeyConversionTest, AndroidKeycodeToEvdevUnsupportedIsZero) {
    EXPECT_EQ(android_keycode_to_evdev(0), 0U);
    EXPECT_EQ(android_keycode_to_evdev(29), 0U);  // AKEYCODE_A
    EXPECT_EQ(android_keycode_to_evdev(-1), 0U);
}

TEST(KeyConversionTest, EvdevToDomCodeRoundTripsKnownKeys) {
    EXPECT_EQ(evdev_to_dom_code(KEY_A), "KeyA");
    EXPECT_EQ(evdev_to_dom_code(KEY_ENTER), "Enter");
    EXPECT_EQ(evdev_to_dom_code(KEY_SPACE), "Space");
    EXPECT_EQ(evdev_to_dom_code(KEY_LEFTSHIFT), "ShiftLeft");
    EXPECT_EQ(evdev_to_dom_code(KEY_VOLUMEUP), "AudioVolumeUp");
    EXPECT_EQ(evdev_to_dom_code(KEY_BACK), "GoBack");
    EXPECT_EQ(evdev_to_dom_code(KEY_HOME), "GoHome");
    EXPECT_EQ(evdev_to_dom_code(KEY_APPSELECT), "AppSwitch");
    EXPECT_EQ(evdev_to_dom_code(0), "");
    EXPECT_EQ(evdev_to_dom_code(0xFFFF), "");
}

TEST(KeyConversionTest, QcodeToEvdevCoversAsciiTableAndModifiers) {
    EXPECT_EQ(qcode_to_evdev(Q_KEY_CODE_SHIFT), static_cast<uint32_t>(KEY_LEFTSHIFT));
    EXPECT_EQ(qcode_to_evdev(Q_KEY_CODE_ALT), static_cast<uint32_t>(KEY_LEFTALT));
    EXPECT_EQ(qcode_to_evdev(Q_KEY_CODE_A), static_cast<uint32_t>(KEY_A));
    EXPECT_EQ(qcode_to_evdev(Q_KEY_CODE_SPC), static_cast<uint32_t>(KEY_SPACE));
    EXPECT_EQ(qcode_to_evdev(Q_KEY_CODE_LF), static_cast<uint32_t>(KEY_ENTER));
    EXPECT_EQ(qcode_to_evdev(Q_KEY_CODE_UNMAPPED), 0U);
}

TEST(KeyConversionTest, ResolveDomKeyClassifiesNonPrintablePrintableAndUnknown) {
    const DomKeyResolution enter = resolve_dom_key("Enter");
    EXPECT_EQ(enter.kind, DomKeyResolution::Kind::kNonPrintable);
    EXPECT_EQ(enter.evdev, static_cast<uint32_t>(KEY_ENTER));

    const DomKeyResolution char_a = resolve_dom_key("a");
    EXPECT_EQ(char_a.kind, DomKeyResolution::Kind::kCharacter);
    EXPECT_EQ(char_a.character, 'a');

    const DomKeyResolution dead = resolve_dom_key("Dead");
    EXPECT_EQ(dead.kind, DomKeyResolution::Kind::kNone);

    const DomKeyResolution dom_code = resolve_dom_key("KeyA");
    EXPECT_EQ(dom_code.kind, DomKeyResolution::Kind::kNone);
}

}  // namespace keyboard
}  // namespace control
}  // namespace emulation
}  // namespace android
