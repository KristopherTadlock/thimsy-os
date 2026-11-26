// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/InputCodes.h"

#include <linux/input-event-codes.h>

#include <cstdlib>

namespace thimsy {
namespace session {
namespace {

struct Named {
    const char* name;
    int type;
    int code;
};

constexpr Named kCodes[] = {
    {"KEY_POWER", EV_KEY, KEY_POWER},
    {"KEY_SLEEP", EV_KEY, KEY_SLEEP},
    {"KEY_WAKEUP", EV_KEY, KEY_WAKEUP},
    {"KEY_VOLUMEUP", EV_KEY, KEY_VOLUMEUP},
    {"KEY_VOLUMEDOWN", EV_KEY, KEY_VOLUMEDOWN},
    {"KEY_MUTE", EV_KEY, KEY_MUTE},
    {"KEY_ENTER", EV_KEY, KEY_ENTER},
    {"KEY_BACK", EV_KEY, KEY_BACK},
    {"KEY_HOMEPAGE", EV_KEY, KEY_HOMEPAGE},
    {"KEY_F1", EV_KEY, KEY_F1},
    {"SW_LID", EV_SW, SW_LID},
    {"SW_HEADPHONE_INSERT", EV_SW, SW_HEADPHONE_INSERT},
    {"BTN_SOUTH", EV_KEY, BTN_SOUTH},
    {"BTN_A", EV_KEY, BTN_A},
    {"BTN_EAST", EV_KEY, BTN_EAST},
    {"BTN_B", EV_KEY, BTN_B},
    {"BTN_C", EV_KEY, BTN_C},
    {"BTN_NORTH", EV_KEY, BTN_NORTH},
    {"BTN_X", EV_KEY, BTN_X},
    {"BTN_WEST", EV_KEY, BTN_WEST},
    {"BTN_Y", EV_KEY, BTN_Y},
    {"BTN_Z", EV_KEY, BTN_Z},
    {"BTN_TL", EV_KEY, BTN_TL},
    {"BTN_TR", EV_KEY, BTN_TR},
    {"BTN_TL2", EV_KEY, BTN_TL2},
    {"BTN_TR2", EV_KEY, BTN_TR2},
    {"BTN_SELECT", EV_KEY, BTN_SELECT},
    {"BTN_START", EV_KEY, BTN_START},
    {"BTN_MODE", EV_KEY, BTN_MODE},
    {"BTN_THUMBL", EV_KEY, BTN_THUMBL},
    {"BTN_THUMBR", EV_KEY, BTN_THUMBR},
    {"BTN_DPAD_UP", EV_KEY, BTN_DPAD_UP},
    {"BTN_DPAD_DOWN", EV_KEY, BTN_DPAD_DOWN},
    {"BTN_DPAD_LEFT", EV_KEY, BTN_DPAD_LEFT},
    {"BTN_DPAD_RIGHT", EV_KEY, BTN_DPAD_RIGHT},
};

bool parseInt(std::string_view s, int* out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

}  // namespace

bool inputCodeByName(std::string_view name, InputCode* out) {
    for (const Named& n : kCodes) {
        if (name == n.name) {
            out->type = n.type;
            out->code = n.code;
            return true;
        }
    }
    size_t colon = name.find(':');
    if (colon == std::string_view::npos) return false;
    int type = 0, code = 0;
    if (!parseInt(name.substr(0, colon), &type) || !parseInt(name.substr(colon + 1), &code)) return false;
    out->type = type;
    out->code = code;
    return true;
}

std::string inputCodeName(int type, int code) {
    for (const Named& n : kCodes) {
        if (n.type == type && n.code == code) return n.name;
    }
    return std::to_string(type) + ":" + std::to_string(code);
}

}  // namespace session
}  // namespace thimsy
