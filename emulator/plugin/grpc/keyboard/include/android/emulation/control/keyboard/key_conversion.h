
// Copyright (C) 2023 The Android Open Source Project
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

#include "qemu/osdep.h"

extern "C" {
// clang-format off
// IWYU pragma: begin_keep

#include "ui/input.h"

typedef struct QKbdState QKbdState;
// IWYU pragma: end_keep
// clang-format on
}

#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

// Necessary on Windows.
#undef send

namespace android {
namespace emulation {
namespace control {
namespace keyboard {

// Forwarde declare DomCode as it's tricky to include dom_key.h above.
enum class DomCode;

/**
 * @struct QemuKeyEvent
 * @brief Represents a single keyboard event for QEMU.
 *
 * This structure encapsulates a QEMU keycode and the state of the key
 * (pressed or released).
 */
struct QemuKeyEvent {
    /**
     * @brief Sends the keyboard event to the QEMU keyboard state.
     *
     * @note This method should be invoked on the qemu event loop.
     * @param kbd A pointer to the QEMU keyboard state.
     */
    void send(QKbdState* kbd) const;

    /**
     * @brief The QEMU keycode.
     */
    QKeyCode code;

    /**
     * @brief True if the key is pressed (key down), false if released (key up).
     */
    bool down;

    friend bool operator==(const QemuKeyEvent&, const QemuKeyEvent&) = default;
    friend std::ostream& operator<<(std::ostream& os, const QemuKeyEvent& key) {
        return os << "{qcode " << key.code << (key.down ? " down}" : " up}");
    }
};

/**
 * @enum KeyCodeType
 * @brief Enumerates the different types of keycode representations.
 */
enum class KeyCodeType : uint8_t {
    usb = 0,    ///< USB HID keycode.
    evdev = 1,  ///< Linux evdev keycode.
    xkb = 2,    ///< Linux XKB keycode.
    win = 3,    ///< Windows OEM scan code.
    mac = 4,    ///< Mac keycode
};

/**
 * @brief Represents a mapping between different keycode representations.
 *
 * This structure defines a mapping between a DOM Code and its corresponding
 * representations in USB HID, evdev, XKB, Windows, and Mac keycode systems.
 */
struct KeycodeMapEntry {
    /**
     * @brief The DOM Code (UIEvents code value).
     *
     * @see http://www.w3.org/TR/DOM-Level-3-Events-code/
     */
    DomCode id;

    /**
     * @brief USB HID keycode.
     *
     * Upper 16-bits: USB Usage Page.
     * Lower 16-bits: USB Usage Id (assigned ID within this usage page).
     */
    uint32_t usb;

    /**
     * @brief evdev keycode.
     */
    uint32_t evdev;

    /**
     * @brief Linux XKB keycode.
     */
    uint32_t xkb;

    /**
     * @brief Windows OEM scan code.
     */
    uint32_t win;

    /**
     * @brief Mac keycode.
     */
    uint32_t mac;

    /**
     * @brief The UIEvents (aka: DOM4Events) |code| value as defined in:
     * http://www.w3.org/TR/DOM-Level-3-Events-code/
     */
    std::string_view code;
};

/**
 * @brief Returns the keymap, a vector of KeycodeMapEntry.
 *
 * @return A constant reference to the keymap vector.
 */
const std::vector<KeycodeMapEntry>& keymap();

/**
 * @brief Converts an evdev keycode to a QEMU keycode.
 *
 * @param evdev The evdev keycode to convert.
 * @return The corresponding QEMU keycode.
 */
QKeyCode evdev_to_qcode(uint32_t evdev);

/**
 * @brief Converts a DOM Code to a QEMU keycode.
 *
 * @param key The DOM Code to convert.
 * @return The corresponding QEMU keycode.
 */
QKeyCode dom_to_qcode(DomCode key);

/**
 * @brief Converts a keycode of a given type to a QEMU keycode.
 *
 * @param from The keycode to convert.
 * @param source The type of the keycode.
 * @return The corresponding QEMU keycode.
 */
QKeyCode keycode_to_qcode(uint32_t from, KeyCodeType source);

/**
 * @brief Translates an ASCII key event into a sequence of QEMU key events.
 *
 * This function handles modifier keys (e.g., Shift, Alt) as needed to
 * represent the given ASCII character.
 *
 * @param ascii The ASCII character to translate.
 * @param down True if the key is pressed, false if released.
 * @return A vector of QemuKeyEvent representing the key event sequence.
 */
std::vector<QemuKeyEvent> ascii_to_qcode(unsigned short ascii, bool down);

/**
 * @brief Converts a DOM Code to an evdev keycode.
 *
 * @param key The DOM Code to convert.
 * @return The corresponding evdev keycode.
 */
uint32_t dom_to_evdev(DomCode key);

/**
 * @brief Converts a W3C DOM Code string (e.g. "KeyA", "Space", "Enter") to an evdev keycode.
 *
 * @param code The W3C DOM Code string.
 * @return The corresponding evdev keycode, or 0 if unmapped.
 */
uint32_t dom_code_to_evdev(std::string_view code);

/**
 * @brief Converts a keycode of a given type to an evdev keycode.
 *
 * @param from The keycode to convert.
 * @param source The type of the keycode.
 * @return The corresponding evdev keycode.
 */
uint32_t keycode_to_evdev(uint32_t from, KeyCodeType source);

/**
 * @brief Maps a supported Android system keycode (e.g. 3 for HOME) to its Linux evdev code.
 *
 * @param keycode The Android AKEYCODE_* value.
 * @return The evdev KEY_* code, or 0 if the keycode is not supported.
 */
uint32_t android_keycode_to_evdev(int32_t keycode);

/**
 * @brief Maps a Linux evdev keycode to its canonical W3C DOM Code string.
 *
 * @param evdev The Linux evdev KEY_* code.
 * @return The corresponding non-empty W3C DOM Code string (e.g. "KeyA", "GoBack"),
 *         or an empty string_view if unmapped or unnamed in keymap().
 */
std::string_view evdev_to_dom_code(uint32_t evdev);

/**
 * @brief Converts a QEMU QKeyCode to its corresponding Linux evdev keycode.
 *
 * Covers all QKeyCode values produced by ascii_to_qcode() (kSkinKeyCharacterMap
 * plus modifier keys Q_KEY_CODE_SHIFT and Q_KEY_CODE_ALT).
 *
 * @param qcode The QEMU keycode to convert.
 * @return The corresponding Linux evdev KEY_* code, or 0 if unmapped.
 */
uint32_t qcode_to_evdev(QKeyCode qcode);

/**
 * @brief Result of resolving a W3C DOM Key string via resolve_dom_key().
 */
struct DomKeyResolution {
    enum class Kind : uint8_t {
        kNone,
        kNonPrintable,
        kCharacter,
    };
    Kind kind = Kind::kNone;
    uint32_t evdev = 0;
    int32_t character = 0;
};

/**
 * @brief Resolves a W3C DOM Key string into either a non-printable evdev code or a Unicode
 * codepoint.
 *
 * @param key The W3C DOM Key string (e.g. "Enter", "ArrowUp", "a", "A").
 * @return A DomKeyResolution indicating whether the key is non-printable, a printable
 *         character, or unrecognized/dead.
 */
DomKeyResolution resolve_dom_key(std::string_view key);

}  // namespace keyboard
}  // namespace control
}  // namespace emulation
}  // namespace android
