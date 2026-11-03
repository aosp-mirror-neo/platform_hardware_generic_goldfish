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
#include "android/emulation/control/keyboard/key_conversion.h"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/log.h"

#include "dom_key.h"
#include "standard-headers/linux/input-event-codes.h"

// clang-format off
// IWYU pragma: begin_keep
extern "C" {
#include "ui/input.h"
#include "ui/kbd-state.h"
}
// IWYU pragma: end_keep
// clang-format on

namespace android {
namespace emulation {
namespace control {
namespace keyboard {

typedef struct SkinKeyEntry {
    QKeyCode code;
    unsigned short base;
    unsigned short caps;
    unsigned short fn;
    unsigned short caps_fn;
    unsigned short number;
} SkinKeyEntry;

static const SkinKeyEntry kSkinKeyCharacterMap[] = {
    /* keycode                   base   caps    fn  caps+fn   number */

    {Q_KEY_CODE_A, 'a', 'A', 0xe1, 0xc1, 'a'},
    {Q_KEY_CODE_B, 'b', 'B', 'b', 'B', 'b'},
    {Q_KEY_CODE_C, 'c', 'C', 0xa9, 0xa2, 'c'},
    {Q_KEY_CODE_D, 'd', 'D', 0xf0, 0xd0, '\''},
    {Q_KEY_CODE_E, 'e', 'E', 0xe9, 0xc9, '"'},
    {Q_KEY_CODE_F, 'f', 'F', '[', '[', '['},
    {Q_KEY_CODE_G, 'g', 'G', ']', ']', ']'},
    {Q_KEY_CODE_H, 'h', 'H', '<', '<', '<'},
    {Q_KEY_CODE_I, 'i', 'I', 0xed, 0xcd, '-'},
    {Q_KEY_CODE_J, 'j', 'J', '>', '>', '>'},
    {Q_KEY_CODE_K, 'k', 'K', ';', 'K', ';'},
    {Q_KEY_CODE_L, 'l', 'L', 0xf8, 0xd8, ':'},
    {Q_KEY_CODE_M, 'm', 'M', 0xb5, 'M', '%'},
    {Q_KEY_CODE_N, 'n', 'N', 0xf1, 0xd1, 'n'},
    {Q_KEY_CODE_O, 'o', 'O', 0xf3, 0xd3, '+'},
    {Q_KEY_CODE_P, 'p', 'P', 0xf6, 0xd6, '='},
    {Q_KEY_CODE_Q, 'q', 'Q', 0xe4, 0xc4, '|'},
    {Q_KEY_CODE_R, 'r', 'R', 0xae, 'R', '`'},
    {Q_KEY_CODE_S, 's', 'S', 0xdf, 0xa7, '\\'},
    {Q_KEY_CODE_T, 't', 'T', 0xfe, 0xde, '}'},
    {Q_KEY_CODE_U, 'u', 'U', 0xfa, 0xda, '_'},
    {Q_KEY_CODE_V, 'v', 'V', 'v', 'V', 'v'},
    {Q_KEY_CODE_W, 'w', 'W', 0xe5, 0xc5, '~'},
    {Q_KEY_CODE_X, 'x', 'X', 'x', 'X', 'x'},
    {Q_KEY_CODE_Y, 'y', 'Y', 0xfc, 0xdc, '}'},
    {Q_KEY_CODE_Z, 'z', 'Z', 0xe6, 0xc6, 'z'},
    {Q_KEY_CODE_COMMA, ',', '<', 0xe7, 0xc7, ','},
    {Q_KEY_CODE_DOT, '.', '>', '.', 0x2026, '.'},
    {Q_KEY_CODE_SLASH, '/', '?', 0xbf, '?', '/'},
    {Q_KEY_CODE_SPC, 0x20, 0x20, 0x9, 0x9, 0x20},
    {Q_KEY_CODE_LF, 0xa, 0xa, 0xa, 0xa, 0xa},
    {Q_KEY_CODE_0, '0', ')', 0x2bc, ')', '0'},
    {Q_KEY_CODE_1, '1', '!', 0xa1, 0xb9, '1'},
    {Q_KEY_CODE_2, '2', '@', 0xb2, '@', '2'},
    {Q_KEY_CODE_3, '3', '#', 0xb3, '#', '3'},
    {Q_KEY_CODE_4, '4', '$', 0xa4, 0xa3, '4'},
    {Q_KEY_CODE_5, '5', '%', 0x20ac, '%', '5'},
    {Q_KEY_CODE_6, '6', '^', 0xbc, 0x0302, '6'},
    {Q_KEY_CODE_7, '7', '&', 0xbd, '&', '7'},
    {Q_KEY_CODE_8, '8', '*', 0xbe, '*', '8'},
    {Q_KEY_CODE_9, '9', '(', 0x2bb, '(', '9'},
    {Q_KEY_CODE_TAB, 0x9, 0x9, 0x9, 0x9, 0x9},
    {Q_KEY_CODE_GRAVE_ACCENT, '`', '~', 0x300, 0x0303, '`'},
    {Q_KEY_CODE_MINUS, '-', '_', 0xa5, '_', '-'},
    {Q_KEY_CODE_EQUAL, '=', '+', 0xd7, 0xf7, '='},
    {Q_KEY_CODE_BRACKET_LEFT, '[', '{', 0xab, '{', '['},
    {Q_KEY_CODE_BRACKET_RIGHT, ']', '}', 0xbb, '}', ']'},
    {Q_KEY_CODE_BACKSLASH, '\\', '|', 0xac, 0xa6, '\\'},
    {Q_KEY_CODE_SEMICOLON, ';', ':', 0xb6, 0xb0, ';'},
    {Q_KEY_CODE_APOSTROPHE, '\'', '"', 0x301, 0x308, '\''},
};

QKeyCode evdev_to_qcode(uint32_t evdev) {
    auto qkc = static_cast<QKeyCode>(qemu_input_linux_to_qcode(evdev));
    if (qkc == Q_KEY_CODE_UNMAPPED) {
        VLOG(1) << "unmapped evdev: " << std::hex << evdev << std::dec;
    }
    return qkc;
}

std::vector<QemuKeyEvent> ascii_to_qcode(unsigned short unicode, bool down) {
    VLOG(2) << "ascii_to_qcode: code="
            << ((unicode <= 255 && std::isprint(unicode)) ? static_cast<char>(unicode) : '.')
            << ", " << std::hex << std::setfill('0') << std::setw(4) << unicode << std::dec
            << " down=" << down;
    /* check base keys */
    for (const auto& entry : kSkinKeyCharacterMap) {
        if (entry.base == unicode) {
            return {{entry.code, down}};
        }
    }

    /* check caps + keys */
    for (const auto& entry : kSkinKeyCharacterMap) {
        if (entry.caps == unicode) {
            if (down) {
                return {{Q_KEY_CODE_SHIFT, down}, {entry.code, down}};
            }
            return {{entry.code, down}, {Q_KEY_CODE_SHIFT, down}};
        }
    }

    /* check fn + keys */
    for (const auto& entry : kSkinKeyCharacterMap) {
        if (entry.fn == unicode) {
            if (down) {
                return {{Q_KEY_CODE_ALT, down}, {entry.code, down}};
            }
            return {{entry.code, down}, {Q_KEY_CODE_ALT, down}};
        }
    }

    /* check caps + fn + keys */
    for (const auto& entry : kSkinKeyCharacterMap) {
        if (entry.caps_fn == unicode) {
            if (down) {
                return {{Q_KEY_CODE_SHIFT, down}, {Q_KEY_CODE_ALT, down}, {entry.code, down}};
            }

            return {{entry.code, down}, {Q_KEY_CODE_ALT, down}, {Q_KEY_CODE_SHIFT, down}};
        }
    }
    return {};
}

bool IsTypeableCodePoint(int32_t code_point) {
    // Text typing skips code points above ASCII, even those the key map reaches through an Alt
    // chord.
    constexpr int32_t kMaxAsciiCodePoint = 0x7F;
    return code_point >= 0 && code_point <= kMaxAsciiCodePoint &&
           !ascii_to_qcode(static_cast<unsigned short>(code_point), /*down=*/true).empty();
}

void QemuKeyEvent::send(QKbdState* kbd) const {
    VLOG(1) << "Sending QKeyCode: " << code << " " << (down ? "down" : "up");
    qkbd_state_key_event(kbd, code, down);
}

namespace {
constexpr std::string_view ToKeycodeStringView(const char* str) {
    return str ? std::string_view(str) : std::string_view();
}
}  // namespace

#define USB_KEYMAP(usb, evdev, xkb, win, mac, code, id) \
    {DomCode::id, usb, evdev, xkb, win, mac, ToKeycodeStringView(code)}
#define USB_KEYMAP_DECLARATION static const std::vector<KeycodeMapEntry> usb_keycode_map =
#include "keycode_converter_data.inc"

#undef USB_KEYMAP
#undef USB_KEYMAP_DECLARATION

const std::vector<KeycodeMapEntry>& keymap() {
    return usb_keycode_map;
}

QKeyCode dom_to_qcode(DomCode key) {
    uint32_t evdev = dom_to_evdev(key);
    QKeyCode qcode = evdev_to_qcode(evdev);
    // Uppercase hex is used here to match the representation in dom_key_data.inc
    VLOG(2) << "dom_to_qcode: key=" << std::uppercase << std::hex << static_cast<int>(key)
            << std::nouppercase << std::dec << " -> evdev=" << std::hex << std::setfill('0')
            << std::setw(4) << evdev << std::dec << " -> qcode=" << qcode;
    return qcode;
}

uint32_t dom_to_evdev(DomCode key) {
    for (const auto entry : usb_keycode_map) {
        if (entry.id == key) {
            return entry.evdev;
        }
    }
    return 0;
}

uint32_t dom_code_to_evdev(std::string_view code) {
    if (code.empty()) {
        return 0;
    }

    // Alias common shorthands to canonical W3C DOM Code names
    if (code == "VolumeUp") {
        code = "AudioVolumeUp";
    } else if (code == "VolumeDown") {
        code = "AudioVolumeDown";
    } else if (code == "VolumeMute") {
        code = "AudioVolumeMute";
    } else if (code == "Back") {
        code = "GoBack";
    }

    for (const auto& entry : usb_keycode_map) {
        if (entry.code == code) {
            return entry.evdev;
        }
    }
    return 0;
}

uint32_t keycode_to_evdev(uint32_t from, KeyCodeType source) {
    // The emulator-controller.proto defines that bit 11 (0x400) is set for keydown events.
    // Some clients might send this bit in the keyCode field when using Evdev.
    if (source == KeyCodeType::evdev) {
        from &= ~0x400;
    }
    for (const auto& entry : usb_keycode_map) {
        switch (source) {
        case KeyCodeType::usb:
            if (entry.usb == from) return entry.evdev;
            break;
        case KeyCodeType::evdev:
            if (entry.evdev == from) return entry.evdev;
            break;
        case KeyCodeType::xkb:
            if (entry.xkb == from) return entry.evdev;
            break;
        case KeyCodeType::win:
            if (entry.win == from) return entry.evdev;
            break;
        case KeyCodeType::mac:
            if (entry.mac == from) return entry.evdev;
            break;
        }
    }
    return 0;
}

uint32_t android_keycode_to_evdev(int32_t keycode) {
    switch (keycode) {
    case 3:  // AKEYCODE_HOME
        return KEY_HOMEPAGE;
    case 4:  // AKEYCODE_BACK
        return KEY_BACK;
    case 24:  // AKEYCODE_VOLUME_UP
        return KEY_VOLUMEUP;
    case 25:  // AKEYCODE_VOLUME_DOWN
        return KEY_VOLUMEDOWN;
    case 26:  // AKEYCODE_POWER
        return KEY_POWER;
    case 27:  // AKEYCODE_CAMERA
        return KEY_CAMERA;
    case 82:  // AKEYCODE_MENU
        return KEY_MENU;
    case 84:  // AKEYCODE_SEARCH
        return KEY_SEARCH;
    case 187:  // AKEYCODE_APP_SWITCH
        return KEY_APPSELECT;
    default:
        return 0;
    }
}

std::string_view evdev_to_dom_code(uint32_t evdev) {
    if (evdev == 0) {
        return {};
    }
    for (auto it = usb_keycode_map.rbegin(); it != usb_keycode_map.rend(); ++it) {
        if (it->evdev == evdev && !it->code.empty()) {
            return it->code;
        }
    }
    return {};
}

uint32_t qcode_to_evdev(QKeyCode qcode) {
    switch (qcode) {
    case Q_KEY_CODE_SHIFT:
        return KEY_LEFTSHIFT;
    case Q_KEY_CODE_ALT:
        return KEY_LEFTALT;
    case Q_KEY_CODE_A:
        return KEY_A;
    case Q_KEY_CODE_B:
        return KEY_B;
    case Q_KEY_CODE_C:
        return KEY_C;
    case Q_KEY_CODE_D:
        return KEY_D;
    case Q_KEY_CODE_E:
        return KEY_E;
    case Q_KEY_CODE_F:
        return KEY_F;
    case Q_KEY_CODE_G:
        return KEY_G;
    case Q_KEY_CODE_H:
        return KEY_H;
    case Q_KEY_CODE_I:
        return KEY_I;
    case Q_KEY_CODE_J:
        return KEY_J;
    case Q_KEY_CODE_K:
        return KEY_K;
    case Q_KEY_CODE_L:
        return KEY_L;
    case Q_KEY_CODE_M:
        return KEY_M;
    case Q_KEY_CODE_N:
        return KEY_N;
    case Q_KEY_CODE_O:
        return KEY_O;
    case Q_KEY_CODE_P:
        return KEY_P;
    case Q_KEY_CODE_Q:
        return KEY_Q;
    case Q_KEY_CODE_R:
        return KEY_R;
    case Q_KEY_CODE_S:
        return KEY_S;
    case Q_KEY_CODE_T:
        return KEY_T;
    case Q_KEY_CODE_U:
        return KEY_U;
    case Q_KEY_CODE_V:
        return KEY_V;
    case Q_KEY_CODE_W:
        return KEY_W;
    case Q_KEY_CODE_X:
        return KEY_X;
    case Q_KEY_CODE_Y:
        return KEY_Y;
    case Q_KEY_CODE_Z:
        return KEY_Z;
    case Q_KEY_CODE_COMMA:
        return KEY_COMMA;
    case Q_KEY_CODE_DOT:
        return KEY_DOT;
    case Q_KEY_CODE_SLASH:
        return KEY_SLASH;
    case Q_KEY_CODE_SPC:
        return KEY_SPACE;
    case Q_KEY_CODE_LF:
        return KEY_ENTER;
    case Q_KEY_CODE_0:
        return KEY_0;
    case Q_KEY_CODE_1:
        return KEY_1;
    case Q_KEY_CODE_2:
        return KEY_2;
    case Q_KEY_CODE_3:
        return KEY_3;
    case Q_KEY_CODE_4:
        return KEY_4;
    case Q_KEY_CODE_5:
        return KEY_5;
    case Q_KEY_CODE_6:
        return KEY_6;
    case Q_KEY_CODE_7:
        return KEY_7;
    case Q_KEY_CODE_8:
        return KEY_8;
    case Q_KEY_CODE_9:
        return KEY_9;
    case Q_KEY_CODE_TAB:
        return KEY_TAB;
    case Q_KEY_CODE_GRAVE_ACCENT:
        return KEY_GRAVE;
    case Q_KEY_CODE_MINUS:
        return KEY_MINUS;
    case Q_KEY_CODE_EQUAL:
        return KEY_EQUAL;
    case Q_KEY_CODE_BRACKET_LEFT:
        return KEY_LEFTBRACE;
    case Q_KEY_CODE_BRACKET_RIGHT:
        return KEY_RIGHTBRACE;
    case Q_KEY_CODE_BACKSLASH:
        return KEY_BACKSLASH;
    case Q_KEY_CODE_SEMICOLON:
        return KEY_SEMICOLON;
    case Q_KEY_CODE_APOSTROPHE:
        return KEY_APOSTROPHE;
    default:
        return 0;
    }
}

namespace {

struct DomKeyMapEntry {
    DomKey dom_key;
    const char* string;
};

#define DOM_KEY_MAP_DECLARATION const DomKeyMapEntry kDomKeyMap[] =
#define DOM_KEY_UNI(key, id, value) {DomKey::id, key}
#define DOM_KEY_MAP(key, id, value) {DomKey::id, key}
#include "dom_key_data.inc"
#undef DOM_KEY_MAP_DECLARATION
#undef DOM_KEY_MAP
#undef DOM_KEY_UNI

// From
// https://cs.chromium.org/chromium/src/ui/events/keycodes/dom_us_layout_data.h
const struct NonPrintableCodeEntry {
    DomCode dom_code;
    DomKey::Base dom_key;
} kNonPrintableCodeMap[] = {
    {DomCode::ABORT, DomKey::CANCEL},
    {DomCode::AGAIN, DomKey::AGAIN},
    {DomCode::ALT_LEFT, DomKey::ALT},
    {DomCode::ALT_RIGHT, DomKey::ALT},
    {DomCode::ARROW_DOWN, DomKey::ARROW_DOWN},
    {DomCode::ARROW_LEFT, DomKey::ARROW_LEFT},
    {DomCode::ARROW_RIGHT, DomKey::ARROW_RIGHT},
    {DomCode::ARROW_UP, DomKey::ARROW_UP},
    {DomCode::BACKSPACE, DomKey::BACKSPACE},
    {DomCode::BASS_BOOST, DomKey::AUDIO_BASS_BOOST_TOGGLE},
    {DomCode::BRIGHTNESS_DOWN, DomKey::BRIGHTNESS_DOWN},
    {DomCode::BRIGHTNESS_UP, DomKey::BRIGHTNESS_UP},
    {DomCode::BROWSER_BACK, DomKey::BROWSER_BACK},
    {DomCode::BROWSER_FAVORITES, DomKey::BROWSER_FAVORITES},
    {DomCode::BROWSER_FORWARD, DomKey::BROWSER_FORWARD},
    {DomCode::BROWSER_HOME, DomKey::BROWSER_HOME},
    {DomCode::BROWSER_REFRESH, DomKey::BROWSER_REFRESH},
    {DomCode::BROWSER_SEARCH, DomKey::BROWSER_SEARCH},
    {DomCode::BROWSER_STOP, DomKey::BROWSER_STOP},
    {DomCode::CAPS_LOCK, DomKey::CAPS_LOCK},
    {DomCode::CHANNEL_DOWN, DomKey::CHANNEL_DOWN},
    {DomCode::CHANNEL_UP, DomKey::CHANNEL_UP},
    {DomCode::CLOSE, DomKey::CLOSE},
    {DomCode::CLOSED_CAPTION_TOGGLE, DomKey::CLOSED_CAPTION_TOGGLE},
    {DomCode::CONTEXT_MENU, DomKey::CONTEXT_MENU},
    {DomCode::CONTROL_LEFT, DomKey::CONTROL},
    {DomCode::CONTROL_RIGHT, DomKey::CONTROL},
    {DomCode::CONVERT, DomKey::CONVERT},
    {DomCode::COPY, DomKey::COPY},
    {DomCode::CUT, DomKey::CUT},
    {DomCode::DEL, DomKey::DEL},
    {DomCode::EJECT, DomKey::EJECT},
    {DomCode::END, DomKey::END},
    {DomCode::ENTER, DomKey::ENTER},
    {DomCode::ESCAPE, DomKey::ESCAPE},
    {DomCode::EXIT, DomKey::EXIT},
    {DomCode::F1, DomKey::F1},
    {DomCode::F2, DomKey::F2},
    {DomCode::F3, DomKey::F3},
    {DomCode::F4, DomKey::F4},
    {DomCode::F5, DomKey::F5},
    {DomCode::F6, DomKey::F6},
    {DomCode::F7, DomKey::F7},
    {DomCode::F8, DomKey::F8},
    {DomCode::F9, DomKey::F9},
    {DomCode::F10, DomKey::F10},
    {DomCode::F11, DomKey::F11},
    {DomCode::F12, DomKey::F12},
    {DomCode::F13, DomKey::F13},
    {DomCode::F14, DomKey::F14},
    {DomCode::F15, DomKey::F15},
    {DomCode::F16, DomKey::F16},
    {DomCode::F17, DomKey::F17},
    {DomCode::F18, DomKey::F18},
    {DomCode::F19, DomKey::F19},
    {DomCode::F20, DomKey::F20},
    {DomCode::F21, DomKey::F21},
    {DomCode::F22, DomKey::F22},
    {DomCode::F23, DomKey::F23},
    {DomCode::F24, DomKey::F24},
    {DomCode::FIND, DomKey::FIND},
    {DomCode::FN, DomKey::FN},
    {DomCode::FN_LOCK, DomKey::FN_LOCK},
    {DomCode::HELP, DomKey::HELP},
    {DomCode::HOME, DomKey::HOME},
    {DomCode::HYPER, DomKey::HYPER},
    {DomCode::INFO, DomKey::INFO},
    {DomCode::INSERT, DomKey::INSERT},
    {DomCode::KANA_MODE, DomKey::KANA_MODE},
    {DomCode::LANG1, DomKey::HANGUL_MODE},
    {DomCode::LANG2, DomKey::HANJA_MODE},
    {DomCode::LANG3, DomKey::KATAKANA},
    {DomCode::LANG4, DomKey::HIRAGANA},
    {DomCode::LANG5, DomKey::ZENKAKU_HANKAKU},
    {DomCode::LAUNCH_APP1, DomKey::LAUNCH_MY_COMPUTER},
    {DomCode::LAUNCH_APP2, DomKey::LAUNCH_CALCULATOR},
    {DomCode::LAUNCH_ASSISTANT, DomKey::LAUNCH_ASSISTANT},
    {DomCode::LAUNCH_AUDIO_BROWSER, DomKey::LAUNCH_MUSIC_PLAYER},
    {DomCode::LAUNCH_CALENDAR, DomKey::LAUNCH_CALENDAR},
    {DomCode::LAUNCH_CONTACTS, DomKey::LAUNCH_CONTACTS},
    {DomCode::LAUNCH_CONTROL_PANEL, DomKey::SETTINGS},
    {DomCode::LAUNCH_INTERNET_BROWSER, DomKey::LAUNCH_WEB_BROWSER},
    {DomCode::LAUNCH_MAIL, DomKey::LAUNCH_MAIL},
    {DomCode::LAUNCH_PHONE, DomKey::LAUNCH_PHONE},
    {DomCode::LAUNCH_SCREEN_SAVER, DomKey::LAUNCH_SCREEN_SAVER},
    {DomCode::LAUNCH_SPREADSHEET, DomKey::LAUNCH_SPREADSHEET},
    {DomCode::LOCK_SCREEN, DomKey::LAUNCH_SCREEN_SAVER},
    {DomCode::LOG_OFF, DomKey::LOG_OFF},
    {DomCode::MAIL_FORWARD, DomKey::MAIL_FORWARD},
    {DomCode::MAIL_REPLY, DomKey::MAIL_REPLY},
    {DomCode::MAIL_SEND, DomKey::MAIL_SEND},
    {DomCode::MEDIA_FAST_FORWARD, DomKey::MEDIA_FAST_FORWARD},
    {DomCode::MEDIA_LAST, DomKey::MEDIA_LAST},
    {DomCode::MEDIA_PLAY, DomKey::MEDIA_PLAY},
    {DomCode::MEDIA_PLAY_PAUSE, DomKey::MEDIA_PLAY_PAUSE},
    {DomCode::MEDIA_RECORD, DomKey::MEDIA_RECORD},
    {DomCode::MEDIA_REWIND, DomKey::MEDIA_REWIND},
    {DomCode::MEDIA_SELECT, DomKey::LAUNCH_MEDIA_PLAYER},
    {DomCode::MEDIA_STOP, DomKey::MEDIA_STOP},
    {DomCode::MEDIA_TRACK_NEXT, DomKey::MEDIA_TRACK_NEXT},
    {DomCode::MEDIA_TRACK_PREVIOUS, DomKey::MEDIA_TRACK_PREVIOUS},
    {DomCode::NEW, DomKey::NEW},
    {DomCode::NON_CONVERT, DomKey::NON_CONVERT},
    {DomCode::NUM_LOCK, DomKey::NUM_LOCK},
    {DomCode::NUMPAD_BACKSPACE, DomKey::BACKSPACE},
    {DomCode::NUMPAD_CLEAR, DomKey::CLEAR},
    {DomCode::NUMPAD_ENTER, DomKey::ENTER},
    {DomCode::OPEN, DomKey::OPEN},
    {DomCode::META_LEFT, DomKey::META},
    {DomCode::META_RIGHT, DomKey::META},
    {DomCode::PAGE_DOWN, DomKey::PAGE_DOWN},
    {DomCode::PAGE_UP, DomKey::PAGE_UP},
    {DomCode::PASTE, DomKey::PASTE},
    {DomCode::PAUSE, DomKey::PAUSE},
    {DomCode::POWER, DomKey::POWER},
    {DomCode::PRINT, DomKey::PRINT},
    {DomCode::PRINT_SCREEN, DomKey::PRINT_SCREEN},
    {DomCode::PROGRAM_GUIDE, DomKey::GUIDE},
    {DomCode::PROPS, DomKey::PROPS},
    {DomCode::REDO, DomKey::REDO},
    {DomCode::SAVE, DomKey::SAVE},
    {DomCode::SCROLL_LOCK, DomKey::SCROLL_LOCK},
    {DomCode::SELECT, DomKey::SELECT},
    {DomCode::SELECT_TASK, DomKey::APP_SWITCH},
    {DomCode::SHIFT_LEFT, DomKey::SHIFT},
    {DomCode::SHIFT_RIGHT, DomKey::SHIFT},
    {DomCode::SPEECH_INPUT_TOGGLE, DomKey::SPEECH_INPUT_TOGGLE},
    {DomCode::SPELL_CHECK, DomKey::SPELL_CHECK},
    {DomCode::SUPER, DomKey::SUPER},
    {DomCode::TAB, DomKey::TAB},
    {DomCode::UNDO, DomKey::UNDO},
    {DomCode::VOLUME_DOWN, DomKey::AUDIO_VOLUME_DOWN},
    {DomCode::VOLUME_MUTE, DomKey::AUDIO_VOLUME_MUTE},
    {DomCode::VOLUME_UP, DomKey::AUDIO_VOLUME_UP},
    {DomCode::WAKE_UP, DomKey::WAKE_UP},
    {DomCode::ZOOM_IN, DomKey::ZOOM_IN},
    {DomCode::ZOOM_OUT, DomKey::ZOOM_OUT},
    {DomCode::ZOOM_TOGGLE, DomKey::ZOOM_TOGGLE},
    {DomCode::GO_BACK, DomKey::GO_BACK},
    {DomCode::GO_HOME, DomKey::GO_HOME},
    {DomCode::APP_SWITCH, DomKey::APP_SWITCH},
    {DomCode::SLEEP, DomKey::STANDBY},
};

DomKey BrowserKeyToDomKey(std::string_view key) {
    if (key.empty()) {
        return DomKey::NONE;
    }
    for (const auto& entry : kDomKeyMap) {
        if (entry.string != nullptr && key == entry.string) {
            return entry.dom_key;
        }
    }
    if (key == "Dead") {
        return DomKey::DeadKeyFromCombiningCharacter(0xFFFF);
    }
    DomKey from_utf8 = DomKey::FromCharacter(std::string(key));
    if (from_utf8 != DomKey::NONE) {
        return from_utf8;
    }
    if (key.size() == 1) {
        return DomKey::FromCharacter(static_cast<uint8_t>(key[0]));
    }
    return DomKey::NONE;
}

DomCode DomKeyAsNonPrintableDomCode(DomKey key) {
    for (const auto& entry : kNonPrintableCodeMap) {
        if (entry.dom_key == key) {
            return entry.dom_code;
        }
    }
    return DomCode::NONE;
}

}  // namespace

DomKeyResolution resolve_dom_key(std::string_view key) {
    const DomKey dom_key = BrowserKeyToDomKey(key);
    if (dom_key == DomKey::NONE || dom_key.IsDeadKey()) {
        return {DomKeyResolution::Kind::kNone, 0, 0};
    }
    const DomCode code = DomKeyAsNonPrintableDomCode(dom_key);
    if (code != DomCode::NONE) {
        return {DomKeyResolution::Kind::kNonPrintable, dom_to_evdev(code), 0};
    }
    return {DomKeyResolution::Kind::kCharacter, 0, dom_key.ToCharacter()};
}

static const char* KeyCodeTypeToString(KeyCodeType source) {
    switch (source) {
    case KeyCodeType::usb:
        return "usb";
    case KeyCodeType::evdev:
        return "evdev";
    case KeyCodeType::xkb:
        return "xkb";
    case KeyCodeType::win:
        return "win";
    case KeyCodeType::mac:
        return "mac";
    default:
        return "unknown";
    }
}

QKeyCode keycode_to_qcode(uint32_t from, KeyCodeType source) {
    uint32_t evdev = keycode_to_evdev(from, source);
    QKeyCode qcode = evdev_to_qcode(evdev);
    VLOG(2) << "keycode_to_qcode: from=" << from << " source=" << KeyCodeTypeToString(source)
            << " -> evdev=" << std::hex << std::setfill('0') << std::setw(4) << evdev << std::dec
            << " -> qcode=" << qcode;
    return qcode;
}
}  // namespace keyboard
}  // namespace control
}  // namespace emulation
}  // namespace android
