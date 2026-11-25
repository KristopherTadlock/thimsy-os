/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "NanoButtonMap.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "NanoButtonMapReaders.h"
#include "NanoJson.h"
#include "NanoSystemsHeal.h"

namespace android {
namespace bmap {

// ---- physical controls ----

namespace {

const char* const kPhysIds[kPhysCount] = {
    "south", "east", "north", "west", "l1", "r1", "l2", "r2", "l3", "r3", "start", "select", "home",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right",
};

// Generic names for a control a pad does not label (and for the directions, which no pad prints).
const char* const kPhysGeneric[kPhysCount] = {
    "Bottom", "Right", "Top", "Left", "L1", "R1", "L2", "R2", "L3", "R3", "Start", "Select", "Home",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "Left stick Up", "Left stick Down", "Left stick Left", "Left stick Right",
    "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
};

// Linux codes (linux/input-event-codes.h), kept here so the host build needs no kernel headers.
enum : int {
    BTN_A = 304, BTN_B = 305, BTN_C = 306, BTN_X = 307, BTN_Y = 308, BTN_Z = 309, BTN_TL = 310, BTN_TR = 311,
    BTN_TL2 = 312, BTN_TR2 = 313, BTN_SELECT = 314, BTN_START = 315, BTN_MODE = 316, BTN_THUMBL = 317,
    BTN_THUMBR = 318, BTN_DPAD_UP = 544, BTN_DPAD_DOWN = 545, BTN_DPAD_LEFT = 546, BTN_DPAD_RIGHT = 547,
    ABS_X = 0, ABS_Y = 1, ABS_Z = 2, ABS_RX = 3, ABS_RY = 4, ABS_RZ = 5, ABS_GAS = 9, ABS_BRAKE = 10,
    ABS_HAT0X = 16, ABS_HAT0Y = 17,
};

// frameworks/base/data/keyboards/Generic.kl: what Android makes of the gammapad virtual device's codes.
int genericKl(int evdev) {
    switch (evdev) {
        case BTN_A: return akey::kButtonA;
        case BTN_B: return akey::kButtonB;
        case BTN_C: return akey::kButtonC;
        case BTN_X: return akey::kButtonX;
        case BTN_Y: return akey::kButtonY;
        case BTN_Z: return akey::kButtonZ;
        case BTN_TL: return akey::kL1;
        case BTN_TR: return akey::kR1;
        case BTN_TL2: return akey::kL2;
        case BTN_TR2: return akey::kR2;
        case BTN_SELECT: return akey::kSelect;
        case BTN_START: return akey::kStart;
        case BTN_MODE: return akey::kMode;
        case BTN_THUMBL: return akey::kThumbL;
        case BTN_THUMBR: return akey::kThumbR;
        case BTN_DPAD_UP: return akey::kDpadUp;
        case BTN_DPAD_DOWN: return akey::kDpadDown;
        case BTN_DPAD_LEFT: return akey::kDpadLeft;
        case BTN_DPAD_RIGHT: return akey::kDpadRight;
    }
    return 0;
}

struct KeyName { int code; const char* name; const char* text; };   // text: as a keyboard key, or null
const KeyName kKeyNames[] = {
    {3, "HOME", nullptr}, {4, "BACK", nullptr}, {19, "DPAD_UP", nullptr}, {20, "DPAD_DOWN", nullptr},
    {21, "DPAD_LEFT", nullptr}, {22, "DPAD_RIGHT", nullptr}, {23, "DPAD_CENTER", nullptr},
    {24, "VOLUME_UP", nullptr}, {25, "VOLUME_DOWN", nullptr}, {26, "POWER", nullptr},
    {7, "0", "0"}, {8, "1", "1"}, {9, "2", "2"}, {10, "3", "3"}, {11, "4", "4"}, {12, "5", "5"},
    {13, "6", "6"}, {14, "7", "7"}, {15, "8", "8"}, {16, "9", "9"},
    {29, "A", "A"}, {30, "B", "B"}, {31, "C", "C"}, {32, "D", "D"}, {33, "E", "E"}, {34, "F", "F"},
    {35, "G", "G"}, {36, "H", "H"}, {37, "I", "I"}, {38, "J", "J"}, {39, "K", "K"}, {40, "L", "L"},
    {41, "M", "M"}, {42, "N", "N"}, {43, "O", "O"}, {44, "P", "P"}, {45, "Q", "Q"}, {46, "R", "R"},
    {47, "S", "S"}, {48, "T", "T"}, {49, "U", "U"}, {50, "V", "V"}, {51, "W", "W"}, {52, "X", "X"},
    {53, "Y", "Y"}, {54, "Z", "Z"}, {55, "COMMA", ","}, {56, "PERIOD", "."}, {57, "ALT_LEFT", "Alt"},
    {58, "ALT_RIGHT", "Alt"}, {59, "SHIFT_LEFT", "Shift"}, {60, "SHIFT_RIGHT", "Shift"}, {61, "TAB", "Tab"},
    {62, "SPACE", "Space"}, {66, "ENTER", "Enter"}, {67, "DEL", "Backspace"}, {68, "GRAVE", "`"},
    {69, "MINUS", "-"}, {70, "EQUALS", "="}, {71, "LEFT_BRACKET", "["}, {72, "RIGHT_BRACKET", "]"},
    {73, "BACKSLASH", "\\"}, {74, "SEMICOLON", ";"}, {75, "APOSTROPHE", "'"}, {76, "SLASH", "/"},
    {82, "MENU", nullptr}, {92, "PAGE_UP", "Page Up"}, {93, "PAGE_DOWN", "Page Down"},
    {96, "BUTTON_A", nullptr}, {97, "BUTTON_B", nullptr}, {98, "BUTTON_C", nullptr}, {99, "BUTTON_X", nullptr},
    {100, "BUTTON_Y", nullptr}, {101, "BUTTON_Z", nullptr}, {102, "BUTTON_L1", nullptr}, {103, "BUTTON_R1", nullptr},
    {104, "BUTTON_L2", nullptr}, {105, "BUTTON_R2", nullptr}, {106, "BUTTON_THUMBL", nullptr},
    {107, "BUTTON_THUMBR", nullptr}, {108, "BUTTON_START", nullptr}, {109, "BUTTON_SELECT", nullptr},
    {110, "BUTTON_MODE", nullptr}, {111, "ESCAPE", "Esc"}, {112, "FORWARD_DEL", "Delete"},
    {113, "CTRL_LEFT", "Ctrl"}, {114, "CTRL_RIGHT", "Ctrl"}, {115, "CAPS_LOCK", "Caps Lock"},
    {131, "F1", "F1"}, {132, "F2", "F2"}, {133, "F3", "F3"}, {134, "F4", "F4"}, {135, "F5", "F5"},
    {136, "F6", "F6"}, {137, "F7", "F7"}, {138, "F8", "F8"}, {139, "F9", "F9"}, {140, "F10", "F10"},
    {141, "F11", "F11"}, {142, "F12", "F12"},
    {188, "BUTTON_1", nullptr}, {189, "BUTTON_2", nullptr}, {190, "BUTTON_3", nullptr}, {191, "BUTTON_4", nullptr},
    {192, "BUTTON_5", nullptr}, {193, "BUTTON_6", nullptr}, {194, "BUTTON_7", nullptr}, {195, "BUTTON_8", nullptr},
    {196, "BUTTON_9", nullptr}, {197, "BUTTON_10", nullptr}, {198, "BUTTON_11", nullptr}, {199, "BUTTON_12", nullptr},
    {200, "BUTTON_13", nullptr}, {201, "BUTTON_14", nullptr}, {202, "BUTTON_15", nullptr}, {203, "BUTTON_16", nullptr},
};

int axisFromName(const std::string& n) {
    static const std::pair<const char*, int> kAxes[] = {
        {"X", aaxis::kX}, {"Y", aaxis::kY}, {"Z", aaxis::kZ}, {"RX", aaxis::kRX}, {"RY", aaxis::kRY},
        {"RZ", aaxis::kRZ}, {"HAT_X", aaxis::kHatX}, {"HAT_Y", aaxis::kHatY}, {"LTRIGGER", aaxis::kLTrigger},
        {"RTRIGGER", aaxis::kRTrigger}, {"GAS", aaxis::kGas}, {"BRAKE", aaxis::kBrake},
    };
    for (const auto& a : kAxes) if (n == a.first) return a.second;
    char* end = nullptr;
    const long v = strtol(n.c_str(), &end, 0);
    return !n.empty() && end && *end == '\0' && v >= 0 && v < 64 ? (int)v : -1;
}

int parseHex(const std::string& s) {
    if (s.empty() || s.size() > 8) return -1;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 16);
    return end && *end == '\0' && v >= 0 ? (int)v : -1;
}

int jsonCode(const njson::Value* v, bool android) {
    if (!v) return -1;
    if (v->isNumber()) return v->asInt(-1);
    if (v->isString()) {
        if (android) {
            const int k = keycodeFromName(v->str);
            if (k) return k;
        }
        char* end = nullptr;
        const long n = strtol(v->str.c_str(), &end, 0);
        if (!v->str.empty() && end && *end == '\0') return (int)n;
    }
    return -1;
}

void addSend(PadModel* m, const Input& in, Phys p) {
    for (const auto& s : m->sends) if (s.first == in && s.second == p) return;
    m->sends.emplace_back(in, p);
}

}  // namespace

const char* physId(Phys p) { return (int)p < kPhysCount ? kPhysIds[(int)p] : ""; }

bool physFromId(const std::string& id, Phys* out) {
    for (int i = 0; i < kPhysCount; i++)
        if (id == kPhysIds[i]) { *out = (Phys)i; return true; }
    return false;
}

bool physIsDpad(Phys p) { return p >= Phys::DUp && p <= Phys::DRight; }
bool physIsLStick(Phys p) { return p >= Phys::LUp && p <= Phys::LRight; }
bool physIsRStick(Phys p) { return p >= Phys::RUp && p <= Phys::RRight; }

int keycodeFromName(const std::string& name) {
    std::string n = name;
    if (n.rfind("KEYCODE_", 0) == 0) n = n.substr(8);
    for (const auto& k : kKeyNames) if (n == k.name) return k.code;
    return 0;
}

std::string keycodeName(int keycode) {
    for (const auto& k : kKeyNames) if (k.code == keycode) return k.name;
    return std::string();
}

std::string keyboardKeyText(int keycode) {
    for (const auto& k : kKeyNames) if (k.code == keycode && k.text) return k.text;
    return std::string();
}

bool parseDeviceId(const std::string& s, DeviceId* out) {
    // vendor:product:descriptor:name; the name is the rest and may hold ':'.
    const size_t a = s.find(':');
    if (a == std::string::npos) return false;
    const size_t b = s.find(':', a + 1);
    if (b == std::string::npos) return false;
    const size_t c = s.find(':', b + 1);
    DeviceId d;
    d.vendor = parseHex(s.substr(0, a));
    d.product = parseHex(s.substr(a + 1, b - a - 1));
    if (d.vendor < 0 || d.product < 0 || d.vendor > 0xffff || d.product > 0xffff) return false;
    if (c == std::string::npos) {
        d.descriptor = s.substr(b + 1);
    } else {
        d.descriptor = s.substr(b + 1, c - b - 1);
        d.name = s.substr(c + 1);
    }
    if (d.name.size() > 256 || d.descriptor.size() > 128) return false;
    *out = d;
    return true;
}

bool padSpecFromJson(const std::string& json, PadSpec* out) {
    njson::Value v;
    if (json.size() > (1u << 20) || !njson::parse(json, &v) || !v.isObject()) return false;
    PadSpec s;
    s.id = v.getString("id");
    s.name = v.getString("name");
    s.drawing = v.getString("drawing");
    s.vendor = std::max(0, parseHex(v.getString("vendor")));
    s.product = std::max(0, parseHex(v.getString("product")));
    if (const njson::Value* bs = v.find("buttons"); bs && bs->isArray()) {
        for (const auto& b : bs->arr) {
            PadSpec::Button btn;
            if (!b.isObject() || !physFromId(b.getString("phys"), &btn.phys)) continue;
            btn.label = b.getString("label");
            btn.evdev = jsonCode(b.find("linux"), false);
            btn.android = std::max(0, jsonCode(b.find("android"), true));
            btn.gammapadLinux = jsonCode(b.find("gammapad"), false);
            s.buttons.push_back(btn);
        }
    }
    if (const njson::Value* as = v.find("axes"); as && as->isArray()) {
        for (const auto& a : as->arr) {
            if (!a.isObject()) continue;
            PadSpec::Axis ax;
            Phys p;
            if (physFromId(a.getString("neg"), &p)) ax.neg = p;
            if (physFromId(a.getString("pos"), &p)) ax.pos = p;
            ax.evdev = jsonCode(a.find("linux"), false);
            const njson::Value* an = a.find("android");
            ax.android = an && an->isString() ? axisFromName(an->str) : jsonCode(an, false);
            ax.posLabel = a.getString("label");
            if (ax.neg == Phys::kCount && ax.pos == Phys::kCount) continue;
            s.axes.push_back(ax);
        }
    }
    if (s.buttons.empty()) return false;
    *out = std::move(s);
    return true;
}

PadSpec standardPadSpec(const DeviceId& dev) {
    PadSpec s;
    s.vendor = dev.vendor;
    s.product = dev.product;
    s.name = dev.name;
    const char* south = "Bottom";
    const char* east = "Right";
    const char* north = "Top";
    const char* west = "Left";
    std::string l1 = "L1", r1 = "R1", l2 = "L2", r2 = "R2", l3 = "L3", r3 = "R3", start = "Start", select = "Select";
    if (dev.vendor == 0x045e) {
        s.id = "xbox"; s.drawing = "xbox";
        south = "A"; east = "B"; north = "Y"; west = "X";
        l1 = "LB"; r1 = "RB"; l2 = "LT"; r2 = "RT"; l3 = "LS"; r3 = "RS"; start = "Menu"; select = "View";
    } else if (dev.vendor == 0x054c) {
        s.id = "playstation"; s.drawing = "dualshock";
        south = "Cross"; east = "Circle"; north = "Triangle"; west = "Square";
        start = "Options"; select = "Share";
    } else if (dev.vendor == 0x057e) {
        s.id = "switch_pro"; s.drawing = "switch";
        south = "B"; east = "A"; north = "X"; west = "Y";
        l1 = "L"; r1 = "R"; l2 = "ZL"; r2 = "ZR"; start = "+"; select = "-";
    } else {
        s.id = "generic";
    }
    auto btn = [&](Phys p, const std::string& label, int evdev, int android) {
        PadSpec::Button b;
        b.phys = p; b.label = label; b.evdev = evdev; b.android = android;
        s.buttons.push_back(b);
    };
    btn(Phys::South, south, BTN_A, akey::kButtonA);
    btn(Phys::East, east, BTN_B, akey::kButtonB);
    btn(Phys::North, north, BTN_X, akey::kButtonY);   // BTN_NORTH is BTN_X; Android's layouts say BUTTON_Y
    btn(Phys::West, west, BTN_Y, akey::kButtonX);
    btn(Phys::L1, l1, BTN_TL, akey::kL1);
    btn(Phys::R1, r1, BTN_TR, akey::kR1);
    btn(Phys::L3, l3, BTN_THUMBL, akey::kThumbL);
    btn(Phys::R3, r3, BTN_THUMBR, akey::kThumbR);
    btn(Phys::Start, start, BTN_START, akey::kStart);
    btn(Phys::Select, select, BTN_SELECT, akey::kSelect);
    btn(Phys::Home, "Home", BTN_MODE, akey::kMode);
    auto axis = [&](Phys neg, Phys pos, int evdev, int android, const std::string& posLabel = std::string()) {
        PadSpec::Axis a;
        a.neg = neg; a.pos = pos; a.evdev = evdev; a.android = android; a.posLabel = posLabel;
        s.axes.push_back(a);
    };
    axis(Phys::DLeft, Phys::DRight, ABS_HAT0X, aaxis::kHatX);
    axis(Phys::DUp, Phys::DDown, ABS_HAT0Y, aaxis::kHatY);
    axis(Phys::LLeft, Phys::LRight, ABS_X, aaxis::kX);
    axis(Phys::LUp, Phys::LDown, ABS_Y, aaxis::kY);
    axis(Phys::RLeft, Phys::RRight, ABS_RX, aaxis::kZ);
    axis(Phys::RUp, Phys::RDown, ABS_RY, aaxis::kRZ);
    axis(Phys::kCount, Phys::L2, ABS_Z, aaxis::kLTrigger, l2);
    axis(Phys::kCount, Phys::R2, ABS_RZ, aaxis::kRTrigger, r2);
    return s;
}

// ---- the pad model ----

namespace {

int swapAbxy(int c) {   // A<->B, X<->Y, as gammapad's InputTransformer swaps them before remap_btn
    switch (c) {
        case BTN_A: return BTN_B;
        case BTN_B: return BTN_A;
        case BTN_X: return BTN_Y;
        case BTN_Y: return BTN_X;
    }
    return c;
}

int remapCode(const std::string& remap, int c) {
    size_t i = 0;
    while (i < remap.size()) {
        size_t e = remap.find(',', i);
        if (e == std::string::npos) e = remap.size();
        const std::string pair = remap.substr(i, e - i);
        const size_t colon = pair.find(':');
        if (colon != std::string::npos) {
            const int from = atoi(pair.substr(0, colon).c_str());
            const int to = atoi(pair.substr(colon + 1).c_str());
            if (from == c && to > 0 && to < 0x300) return to;
        }
        i = e + 1;
    }
    return c;
}

// The virtual device's axes (PID 0b13 keeps its native layout, gammapad/README.md) and the Android
// axes Generic.kl gives them, by the role a source axis fills.
bool virtualAxis(Phys half, int* evdev, int* android) {
    switch (half) {
        case Phys::LLeft: case Phys::LRight: *evdev = ABS_X; *android = aaxis::kX; return true;
        case Phys::LUp: case Phys::LDown: *evdev = ABS_Y; *android = aaxis::kY; return true;
        case Phys::RLeft: case Phys::RRight: *evdev = ABS_Z; *android = aaxis::kZ; return true;
        case Phys::RUp: case Phys::RDown: *evdev = ABS_RZ; *android = aaxis::kRZ; return true;
        case Phys::DLeft: case Phys::DRight: *evdev = ABS_HAT0X; *android = aaxis::kHatX; return true;
        case Phys::DUp: case Phys::DDown: *evdev = ABS_HAT0Y; *android = aaxis::kHatY; return true;
        case Phys::L2: *evdev = ABS_BRAKE; *android = aaxis::kLTrigger; return true;
        case Phys::R2: *evdev = ABS_GAS; *android = aaxis::kRTrigger; return true;
        default: return false;
    }
}

}  // namespace

std::vector<Phys> PadModel::physFor(const Input& in) const {
    std::vector<Phys> out;
    for (const auto& s : sends)
        if (s.first == in && std::find(out.begin(), out.end(), s.second) == out.end()) out.push_back(s.second);
    return out;
}

Input PadModel::input(Phys p, Input::Kind space) const {
    for (const auto& s : sends)
        if (s.second == p && s.first.kind == space) return s.first;
    return Input();
}

std::string PadModel::labelOf(Phys p) const {
    if ((int)p >= kPhysCount) return std::string();
    if (!label[(int)p].empty()) return label[(int)p];
    return kPhysGeneric[(int)p];
}

PadModel padModel(const PadSpec& src, const GamepadState& gp) {
    PadModel m;
    m.id = src.id;
    m.name = src.name;
    m.drawing = src.drawing;
    m.viaGammapad = gp.running;
    m.swapped = gp.running && !gp.swap;
    for (const auto& b : src.buttons) {
        const int i = (int)b.phys;
        if (i >= kPhysCount) continue;
        m.present[i] = true;
        if (!b.label.empty()) m.label[i] = b.label;
        if (!gp.running) {
            if (b.android > 0) addSend(&m, Input::key(b.android), b.phys);
            if (b.evdev >= 0) addSend(&m, Input::evKey(b.evdev), b.phys);
            continue;
        }
        int c = b.gammapadLinux >= 0 ? b.gammapadLinux : b.evdev;
        if (c < 0) continue;
        // gammapad gives the built-in (Nintendo-layout) pad's face buttons by position, and ABXY Swap
        // swaps them back; Nano and drastic-nano undo both as they read them (NanoAbxy.h).
        if (!gp.swap) c = swapAbxy(c);
        c = remapCode(gp.remap, c);
        addSend(&m, Input::evKey(gp.swap ? c : swapAbxy(c)), b.phys);
        if (const int k = genericKl(c)) addSend(&m, Input::key(k), b.phys);
    }
    for (const auto& a : src.axes) {
        for (int half = 0; half < 2; half++) {
            const Phys p = half ? a.pos : a.neg;
            const int dir = half ? 1 : -1;
            if ((int)p >= kPhysCount) continue;
            m.present[(int)p] = true;
            if (half && !a.posLabel.empty() && m.label[(int)p].empty()) m.label[(int)p] = a.posLabel;
            int evdev = a.evdev, android = a.android;
            if (gp.running && !virtualAxis(p, &evdev, &android)) continue;
            // A trigger reads 0 at rest: only its positive half exists on the device.
            if (android >= 0) addSend(&m, Input::axis(android, dir), p);
            if (evdev >= 0) addSend(&m, Input::evAbs(evdev, dir), p);
        }
    }
    // Android turns an unhandled hat into D-pad keys, and Flycast, ARMSX2 and Vita3K do so
    // themselves, so a bind on 19-22 is the D-pad.
    static const std::pair<Phys, int> kDpadKeys[] = {
        {Phys::DUp, akey::kDpadUp}, {Phys::DDown, akey::kDpadDown},
        {Phys::DLeft, akey::kDpadLeft}, {Phys::DRight, akey::kDpadRight}};
    for (const auto& d : kDpadKeys)
        if (m.present[(int)d.first]) addSend(&m, Input::key(d.second), d.first);
    return m;
}

int southKeycode(const PadModel& m) {
    const Input in = m.input(Phys::South, Input::kKey);
    return in.kind == Input::kKey ? in.code : 0;
}

int southEvdev(const PadModel& m) {
    const Input in = m.input(Phys::South, Input::kEvKey);
    return in.kind == Input::kEvKey ? in.code : 0;
}

Phys settingButton(const std::string& value, const PadModel& m) {
    static const std::pair<const char*, Phys> kFace[] = {
        {"FACE_SOUTH", Phys::South}, {"FACE_EAST", Phys::East}, {"FACE_NORTH", Phys::North}, {"FACE_WEST", Phys::West}};
    for (const auto& f : kFace)
        if (value == f.first) return m.has(f.second) ? f.second : Phys::kCount;
    const int k = keycodeFromName(value);
    if (!k) return Phys::kCount;
    const std::vector<Phys> p = m.physFor(Input::key(k));
    return p.empty() ? Phys::kCount : p[0];
}

bool sameButton(const std::string& a, const std::string& b, const PadModel& m) {
    const Phys pa = settingButton(a, m);
    return pa != Phys::kCount && pa == settingButton(b, m);
}

std::string migrateButtonMap(const std::string& current, const std::vector<std::string>& hotkeys,
                             const PadModel& m, std::string* log) {
    if (log) log->clear();
    const Phys cur = settingButton(current, m);
    if (cur == Phys::kCount) return current;
    auto taken = [&](Phys p, std::string* by) {
        for (const auto& h : hotkeys)
            if (settingButton(h, m) == p) { if (by) *by = h; return true; }
        return false;
    };
    std::string holder;
    if (!taken(cur, &holder)) return current;
    static const std::pair<Phys, const char*> kOrder[] = {
        {Phys::South, "FACE_SOUTH"}, {Phys::East, "FACE_EAST"}, {Phys::North, "FACE_NORTH"}, {Phys::West, "FACE_WEST"}};
    for (const auto& o : kOrder) {
        if (!m.has(o.first) || taken(o.first, nullptr)) continue;
        if (log) *log = "buttonmap: " + current + " is held by hotkey " + holder + ", using " + o.second;
        return o.second;
    }
    if (log) *log = "buttonmap: every face button is a hotkey, Button Map Button off";
    return "NONE";
}

// ---- when the chord opens ----

bool triggerOpens(Trigger t, Where w) {
    switch (t) {
        case Trigger::kFunctionSouth: return true;
        case Trigger::kStartSelectSouth: return w == Where::kNanoMenus;
    }
    return false;
}

BrowserState browserState(bool gammapadRunning, bool bindingsEnabled) {
    if (!gammapadRunning) return BrowserState::kNeedsControllerEnable;
    if (!bindingsEnabled) return BrowserState::kBindingsOff;
    return BrowserState::kActive;
}

const char* browserHint(BrowserState s) {
    switch (s) {
        case BrowserState::kNeedsControllerEnable:
            return "Browser controls need Controller Enable (Settings > Gamepad Settings)";
        case BrowserState::kBindingsOff:
            return "Controller Bindings is off (Quick Menu)";
        case BrowserState::kActive:
            break;
    }
    return "";
}

// ---- the catalogue ----

const Anchor* Drawing::find(const std::string& id) const {
    for (const auto& a : anchors) if (a.id == id) return &a;
    auto it = aliases.find(id);
    if (it != aliases.end())
        for (const auto& a : anchors) if (a.id == it->second) return &a;
    return nullptr;
}

const Drawing* Catalog::drawing(const std::string& id) const {
    auto it = drawings.find(id);
    return it == drawings.end() ? nullptr : &it->second;
}

const CoreMap* Catalog::core(const std::string& c) const {
    auto it = cores.find(c);
    return it == cores.end() ? nullptr : &it->second;
}

namespace {

void readBox(const njson::Value* v, float out[4]) {
    if (!v || !v->isArray() || v->arr.size() != 4) return;
    for (int i = 0; i < 4; i++) out[i] = (float)v->arr[i].asNumber(out[i]);
}

void readStringMap(const njson::Value* v, std::map<std::string, std::string>* out) {
    if (!v || !v->isObject()) return;
    for (const auto& kv : v->obj) if (kv.second.isString()) (*out)[kv.first] = kv.second.str;
}

void readSystems(const njson::Value* v, std::map<std::string, SystemArt>* out) {
    if (!v || !v->isObject()) return;
    for (const auto& kv : v->obj) {
        if (!kv.second.isObject() || out->count(kv.first)) continue;   // Nano's own systems first
        SystemArt s;
        s.name = kv.second.getString("name");
        s.drawing = kv.second.getString("drawing");
        s.alt = kv.second.getString("alt");
        readStringMap(kv.second.find("names"), &s.names);
        (*out)[kv.first] = s;
    }
}

bool readSmallFile(const std::string& path, std::string* out, size_t max) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (size_t)st.st_size <= max;
    if (ok) {
        out->resize((size_t)st.st_size);
        size_t got = 0;
        while (got < out->size()) {
            const ssize_t n = read(fd, &(*out)[got], out->size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        ok = got == out->size();
    }
    close(fd);
    return ok;
}

}  // namespace

bool loadCatalog(const std::string& controllersJson, const std::string& retropadJson,
                 const std::string& browserJson, Catalog* out) {
    Catalog cat;
    njson::Value art;
    if (!njson::parse(controllersJson, &art) || !art.isObject()) return false;
    if (const njson::Value* ds = art.find("drawings"); ds && ds->isObject()) {
        for (const auto& kv : ds->obj) {
            const njson::Value& d = kv.second;
            if (!d.isObject()) continue;
            Drawing dr;
            dr.id = kv.first;
            dr.file = d.getString("file");
            readBox(d.find("viewBox"), dr.vb);
            dr.labelBounds[0] = dr.vb[0]; dr.labelBounds[1] = dr.vb[1];
            dr.labelBounds[2] = dr.vb[0] + dr.vb[2]; dr.labelBounds[3] = dr.vb[1] + dr.vb[3];
            readBox(d.find("labelBounds"), dr.labelBounds);
            const njson::Value* asp = d.find("aspect");
            dr.aspect = asp ? (float)asp->asNumber(1.0) : 1.0f;
            dr.dynamicLabels = d.getString("labelMode") == "dynamic";
            readStringMap(d.find("aliases"), &dr.aliases);
            if (const njson::Value* as = d.find("anchors"); as && as->isArray()) {
                for (const auto& a : as->arr) {
                    if (!a.isObject()) continue;
                    Anchor an;
                    an.id = a.getString("id");
                    an.name = a.getString("name");
                    an.kind = a.getString("kind");
                    an.side = a.getString("side");
                    if (const njson::Value* x = a.find("x")) an.x = (float)x->asNumber();
                    if (const njson::Value* y = a.find("y")) an.y = (float)y->asNumber();
                    if (const njson::Value* r = a.find("r")) an.r = (float)r->asNumber();
                    const njson::Value* l = a.find("label");
                    if (l && l->isArray() && l->arr.size() == 2) {
                        an.lx = (float)l->arr[0].asNumber(an.x);
                        an.ly = (float)l->arr[1].asNumber(an.y);
                    } else {
                        an.lx = an.x; an.ly = an.y;
                    }
                    if (!an.id.empty()) dr.anchors.push_back(an);
                }
            }
            if (dr.vb[2] > 0 && dr.vb[3] > 0) cat.drawings[dr.id] = std::move(dr);
        }
    }
    readSystems(art.find("systems"), &cat.systems);
    readSystems(art.find("platforms"), &cat.systems);

    njson::Value rp;
    if (!retropadJson.empty() && njson::parse(retropadJson, &rp)) {
        if (const njson::Value* cs = rp.find("cores"); cs && cs->isObject()) {
            for (const auto& kv : cs->obj) {
                const njson::Value& c = kv.second;
                if (!c.isObject()) continue;
                CoreMap cm;
                cm.core = kv.first;
                cm.library = c.getString("library", kv.first);
                cm.drawing = c.getString("drawing");
                cm.perGame = c.getBool("perGame");
                if (const njson::Value* s = c.find("systems"); s && s->isArray())
                    for (const auto& x : s->arr) if (x.isString()) cm.systems.push_back(x.str);
                readStringMap(c.find("map"), &cm.map);
                readStringMap(c.find("notes"), &cm.notes);
                if (const njson::Value* ok = c.find("optionKeys"); ok && ok->isArray())
                    for (const auto& x : ok->arr) if (x.isString()) cm.optionKeys.push_back(x.str);
                if (const njson::Value* vs = c.find("variants"); vs && vs->isArray()) {
                    for (const auto& v : vs->arr) {
                        if (!v.isObject()) continue;
                        CoreMap::Variant var;
                        var.device = v.getInt("device", -1);
                        var.option = v.getString("option");
                        var.value = v.getString("value");
                        var.drawing = v.getString("drawing");
                        var.note = v.getString("note");
                        readStringMap(v.find("map"), &var.map);
                        if (const njson::Value* sy = v.find("system"); sy && sy->isArray()) {
                            // A system-scoped variant is one variant per system.
                            for (const auto& x : sy->arr) {
                                if (!x.isString()) continue;
                                CoreMap::Variant one = var;
                                one.option = "@system";
                                one.value = x.str;
                                cm.variants.push_back(one);
                            }
                            continue;
                        }
                        cm.variants.push_back(var);
                    }
                }
                cat.cores[cm.core] = std::move(cm);
            }
        }
    }

    njson::Value br;
    if (!browserJson.empty() && njson::parse(browserJson, &br)) {
        cat.browserBookmarkCtrlB = br.getString("bookmarkCtrlB");
        if (const njson::Value* rs = br.find("rows"); rs && rs->isArray()) {
            for (const auto& r : rs->arr) {
                if (!r.isObject()) continue;
                BrowserRow row;
                row.control = r.getString("control");
                row.action = r.getString("action");
                row.cursorAction = r.getString("cursor");
                if (!row.control.empty() && !row.action.empty()) cat.browser.push_back(row);
            }
        }
    }
    *out = std::move(cat);
    return true;
}

bool loadCatalogFrom(const std::string& artDir, const std::string& dataDir, Catalog* out) {
    std::string art, rp, br;
    if (!readSmallFile(artDir + "/controllers.json", &art, 4u << 20)) return false;
    readSmallFile(dataDir + "/retropad.json", &rp, 1u << 20);
    readSmallFile(dataDir + "/browser.json", &br, 256u << 10);
    return loadCatalog(art, rp, br, out);
}

// ---- context ----

std::string coreFromSo(const std::string& soPath) {
    std::string b = soPath.substr(soPath.find_last_of('/') + 1);
    for (const char* suf : {"_libretro_android.so", "_libretro.so", ".so"}) {
        const size_t n = strlen(suf);
        if (b.size() > n && b.compare(b.size() - n, n, suf) == 0) { b.resize(b.size() - n); break; }
    }
    return b;
}

bool packageMatches(const std::string& entry, const std::string& pkg) {
    if (!entry.empty() && entry.back() == '*') return pkg.compare(0, entry.size() - 1, entry, 0, entry.size() - 1) == 0;
    return !entry.empty() && entry == pkg;
}

bool isBrowserPackage(const std::string& list, const std::string& pkg) {
    if (pkg.empty()) return false;
    const std::string l = list.empty() ? "com.brave.browser,net.slions.fulguris.*" : list;
    size_t i = 0;
    while (i <= l.size()) {
        size_t e = l.find(',', i);
        if (e == std::string::npos) e = l.size();
        std::string entry = l.substr(i, e - i);
        while (!entry.empty() && isspace((unsigned char)entry.back())) entry.pop_back();
        while (!entry.empty() && isspace((unsigned char)entry[0])) entry.erase(0, 1);
        if (packageMatches(entry, pkg)) return true;
        i = e + 1;
    }
    return false;
}

std::string defaultSystemFor(const std::string& emulator) {
    static const std::pair<const char*, const char*> kFirst[] = {
        {"ppsspp", "psp"}, {"dolphin", "gc"}, {"azahar", "n3ds"}, {"flycast", "dreamcast"}, {"armsx2", "ps2"},
        {"vita3k", "vita"}, {"watermelonds", "nds"}, {"seedlessds", "nds"}, {"drastic", "nds"},
        {"drastic-nano", "nds"}, {"mupen64plus-fz", "n64"}, {"scummvm", "scummvm"}, {"hakux", "xbox"},
    };
    for (const auto& f : kFirst) if (emulator == f.first) return f.second;
    return std::string();
}

namespace {

std::string emulatorForPackage(const std::vector<ProfileRef>& profiles, const std::string& pkg) {
    if (pkg.empty()) return std::string();
    if (pkg == "com.gammaos.drasticsf") return "drastic-nano";   // drastic-nano's host activity
    for (const auto& p : profiles) {
        if (p.id == "fallback") continue;
        for (const auto& e : p.packages)
            if (e != "*" && packageMatches(e, pkg)) return p.id;
    }
    return std::string();
}

}  // namespace

std::string knownSystem(const Catalog& cat, const std::string& systemId) {
    if (systemId.empty()) return std::string();
    for (const auto& a : nanosys::romFolderAliases(systemId))
        if (cat.systems.count(a)) return a;
    return std::string();
}

std::string coreSystem(const CoreMap& cm, const std::string& systemId) {
    if (systemId.empty()) return std::string();
    for (const auto& a : nanosys::romFolderAliases(systemId))
        if (std::find(cm.systems.begin(), cm.systems.end(), a) != cm.systems.end()) return a;
    return std::string();
}

Context resolveContext(const ContextInputs& in, const Catalog& cat) {
    Context c;
    if (!triggerOpens(in.trigger, in.where)) return c;
    if (in.where == Where::kNanoMenus) {
        if (in.focus == ContextInputs::kFocusAppRow) {
            if (isBrowserPackage(in.browserPkgs, in.appPkg)) {
                c.kind = Context::kBrowser;
                c.browserPkg = c.package = in.appPkg;
            }
            return c;
        }
        if (in.focus != ContextInputs::kFocusSystem && in.focus != ContextInputs::kFocusGame &&
            in.focus != ContextInputs::kFocusRecent)
            return c;
        if (in.systemId == "androidgames" || in.launchPkg == "org.thimsy.androidgames") return c;
        if (!in.coreSo.empty()) {
            c.emulator = "retroarch";
            c.package = "com.retroarch.aarch64";
            c.core = coreFromSo(in.coreSo);
        } else {
            c.emulator = emulatorForPackage(in.profiles, in.launchPkg);
            c.package = in.launchPkg;
            if (in.drasticNano && (c.emulator == "drastic" || in.launchPkg == "com.dsemu.drastic")) c.emulator = "drastic-nano";
        }
        // An app with no profile (an emulator catalogue player Thimsy doesn't ship) still plays a
        // console the catalogue draws.
        if (c.emulator.empty() && (c.package.empty() || knownSystem(cat, in.systemId).empty())) return Context();
        c.kind = Context::kGame;
        c.systemId = in.systemId;
        if (c.systemId.empty() && !c.core.empty())
            if (const CoreMap* cm = cat.core(c.core); cm && !cm->systems.empty()) c.systemId = cm->systems[0];
        if (c.systemId.empty()) c.systemId = defaultSystemFor(c.emulator);
        if (in.focus != ContextInputs::kFocusSystem) c.romPath = in.romPath;
        return c;
    }

    if (in.fgPkg.empty()) return c;
    if (isBrowserPackage(in.browserPkgs, in.fgPkg)) {
        c.kind = Context::kBrowser;
        c.browserPkg = c.package = in.fgPkg;
        return c;
    }
    c.emulator = emulatorForPackage(in.profiles, in.fgPkg);
    c.kind = Context::kGame;
    c.package = in.fgPkg;
    if (c.emulator.empty()) {
        // Any other app gets nothing, unless it is the one Nano launched for a console it draws.
        if (in.fgPkg != in.launchApp || in.launchSystem == "androidgames" || knownSystem(cat, in.launchSystem).empty())
            return Context();
        c.systemId = in.launchSystem;
        c.romPath = in.launchGame;
        return c;
    }
    if (c.emulator == "retroarch") {
        const bool hist = in.historyNewer && !in.historyCore.empty();
        c.core = coreFromSo(hist ? in.historyCore : in.launchCore);
        c.romPath = hist ? in.historyPath : in.launchRom;
        const CoreMap* cm = c.core.empty() ? nullptr : cat.core(c.core);
        if (!hist && !in.launchSystem.empty() && (!cm || !coreSystem(*cm, in.launchSystem).empty()))
            c.systemId = in.launchSystem;
        else if (cm && !cm->systems.empty())
            c.systemId = cm->systems[0];
        return c;
    }
    c.systemId = in.launchSystem.empty() ? defaultSystemFor(c.emulator) : in.launchSystem;
    c.romPath = in.launchGame;
    return c;
}

bool parseContentHistory(const std::string& json, HistoryEntry* newest) {
    njson::Value v;
    if (json.size() > (8u << 20) || !njson::parse(json, &v) || !v.isObject()) return false;
    const njson::Value* items = v.find("items");
    if (!items || !items->isArray() || items->arr.empty()) return false;
    const njson::Value& e = items->arr[0];   // RetroArch puts a new entry first
    if (!e.isObject()) return false;
    HistoryEntry h;
    h.path = e.getString("path");
    h.corePath = e.getString("core_path");
    h.coreName = e.getString("core_name");
    if (h.corePath.empty() || h.corePath == "DETECT") return false;
    *newest = h;
    return true;
}

// ---- rows ----

namespace {

struct DirOf { int group; int dir; };   // group 0 D-pad, 1 left stick, 2 right stick; dir 0 up 1 down 2 left 3 right
bool dirOf(Phys p, DirOf* out) {
    const int i = (int)p;
    if (physIsDpad(p)) { *out = {0, i - (int)Phys::DUp}; return true; }
    if (physIsLStick(p)) { *out = {1, i - (int)Phys::LUp}; return true; }
    if (physIsRStick(p)) { *out = {2, i - (int)Phys::RUp}; return true; }
    return false;
}

const char* const kGroupNames[3] = {"D-pad", "Left stick", "Right stick"};
const char* const kDirNames[4] = {"Up", "Down", "Left", "Right"};

}  // namespace

std::string rowKeys(const Row& row, const PadModel& m) {
    if (!row.keys.empty()) return row.keys;
    if (!row.phys.empty()) return physLabel(row.phys, m);
    if (!row.offPad.empty()) return row.offPad;
    return "\xE2\x80\x94";
}

std::string physLabel(const std::vector<Phys>& phys, const PadModel& m) {
    if (phys.empty()) return "\xE2\x80\x94";   // an em dash
    std::vector<Phys> sorted(phys);
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    bool whole[3] = {false, false, false};
    for (int g = 0; g < 3; g++) {
        int n = 0;
        for (Phys p : sorted) { DirOf d; if (dirOf(p, &d) && d.group == g) n++; }
        whole[g] = n == 4;
    }
    std::string out;
    bool done[3] = {false, false, false};
    for (Phys p : sorted) {
        std::string part;
        DirOf d;
        if (dirOf(p, &d) && whole[d.group]) {
            if (done[d.group]) continue;
            done[d.group] = true;
            part = kGroupNames[d.group];
        } else {
            part = m.labelOf(p);
        }
        if (!out.empty()) out += " / ";
        out += part;
    }
    return out;
}

namespace {

// Text from an app's own file, cut to what a panel line can show (the layout's wrap is not linear
// in the length), at a character boundary, with an ellipsis.
std::string clipText(const std::string& t) {
    constexpr size_t kMax = 120;
    if (t.size() <= kMax) return t;
    size_t n = kMax;
    while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
    return t.substr(0, n) + "\xE2\x80\xA6";
}

std::string inputOffPadText(const Input& in) {
    switch (in.kind) {
        case Input::kOther: return in.text;
        case Input::kKey: {
            const std::string kb = keyboardKeyText(in.code);
            if (!kb.empty()) return "Keyboard " + kb;
            const std::string n = keycodeName(in.code);
            return n.empty() ? "Key " + std::to_string(in.code) : n;
        }
        case Input::kAxis:
            return "Axis " + std::to_string(in.code) + (in.dir < 0 ? "-" : "+");
        case Input::kEvKey:
            return "Key " + std::to_string(in.code);
        case Input::kEvAbs:
            return "Axis " + std::to_string(in.code) + (in.dir < 0 ? "-" : "+");
        case Input::kNone:
            break;
    }
    return std::string();
}

std::string anchorName(const std::string& id, const Drawing* dr, const SystemArt* sa, const std::string& fallback) {
    if (sa) {
        auto it = sa->names.find(id);
        if (it != sa->names.end()) return it->second;
    }
    if (dr)
        if (const Anchor* a = dr->find(id)) return a->name;
    return fallback.empty() ? id : fallback;
}

// The four rows of one d-pad or stick, when each direction is pressed by the same group(s) of
// physical directions: one row on the whole control.
void collapseGroup(std::vector<Row>* rows, const std::string dirIds[4], const std::string& whole,
                   const std::string& wholeName) {
    int at[4];
    for (int d = 0; d < 4; d++) {
        at[d] = -1;
        for (size_t i = 0; i < rows->size(); i++)
            if ((*rows)[i].consoleId == dirIds[d]) { at[d] = (int)i; break; }
        if (at[d] < 0) return;
    }
    int groups = -1;
    std::vector<Phys> all;
    for (int d = 0; d < 4; d++) {
        const Row& r = (*rows)[at[d]];
        if (r.phys.empty() || !r.offPad.empty()) return;
        int g = 0;
        for (Phys p : r.phys) {
            DirOf o;
            if (!dirOf(p, &o) || o.dir != d) return;
            g |= 1 << o.group;
        }
        if (groups >= 0 && g != groups) return;
        groups = g;
        all.insert(all.end(), r.phys.begin(), r.phys.end());
    }
    Row merged = (*rows)[at[0]];
    merged.consoleId = whole;
    merged.consoleName = wholeName;
    merged.phys = all;
    std::sort(merged.phys.begin(), merged.phys.end());
    std::vector<int> drop(at, at + 4);
    std::sort(drop.begin(), drop.end());
    (*rows)[drop[0]] = merged;
    for (int i = 3; i >= 1; i--) rows->erase(rows->begin() + drop[i]);
}

}  // namespace

MapResult build(const ReaderEnv& env, const Context& ctx, const PadModel& pad, const Catalog& cat) {
    MapResult r;
    r.ctx = ctx;
    r.padName = pad.name;
    if (ctx.kind == Context::kNone) return r;

    if (ctx.kind == Context::kBrowser) {
        r.console = "browser";
        r.consoleName = "Browser Controls";
        r.emulatorName = ctx.browserPkg.rfind("net.slions.fulguris", 0) == 0 ? "Fulguris"
                         : ctx.browserPkg == "com.brave.browser"           ? "Brave"
                                                                           : ctx.browserPkg;
        r.source = Source::kConfig;
        auto codePhys = [&](const std::string& c, std::vector<Phys>* out) {
            static const std::pair<const char*, int> kEv[] = {
                {"BTN_A", BTN_A}, {"BTN_B", BTN_B}, {"BTN_X", BTN_X}, {"BTN_Y", BTN_Y}, {"BTN_TL", BTN_TL},
                {"BTN_TR", BTN_TR}, {"BTN_TL2", BTN_TL2}, {"BTN_TR2", BTN_TR2}, {"BTN_THUMBL", BTN_THUMBL},
                {"BTN_THUMBR", BTN_THUMBR}, {"BTN_START", BTN_START}, {"BTN_SELECT", BTN_SELECT}};
            if (c == "LSTICK") { for (Phys p : {Phys::LUp, Phys::LDown, Phys::LLeft, Phys::LRight}) out->push_back(p); return true; }
            if (c == "RSTICK") { for (Phys p : {Phys::RUp, Phys::RDown, Phys::RLeft, Phys::RRight}) out->push_back(p); return true; }
            if (c == "DPAD") { for (Phys p : {Phys::DUp, Phys::DDown, Phys::DLeft, Phys::DRight}) out->push_back(p); return true; }
            for (const auto& e : kEv) {
                if (c != e.first) continue;
                // BrowserMode reads gammapad's codes, before Nano swaps them back.
                std::vector<Phys> ps = pad.physFor(Input::evKey(pad.swapped ? swapAbxy(e.second) : e.second));
                // The triggers reach BrowserMode as an axis or a button, whichever the pad has.
                if (ps.empty() && e.second == BTN_TL2) ps = {Phys::L2};
                if (ps.empty() && e.second == BTN_TR2) ps = {Phys::R2};
                out->insert(out->end(), ps.begin(), ps.end());
                return !ps.empty();
            }
            return false;
        };
        for (const auto& b : cat.browser) {
            Row row;
            row.consoleName = b.action;
            if (!b.cursorAction.empty()) row.note = "Cursor mode: " + b.cursorAction;
            // "BTN_SELECT+BTN_TL": a chord; the row's buttons are its parts, in order.
            std::string rest = b.control;
            bool ok = true;
            std::vector<std::string> keys;
            while (!rest.empty()) {
                const size_t plus = rest.find('+');
                const std::string part = rest.substr(0, plus);
                std::vector<Phys> ps;
                if (!codePhys(part, &ps)) ok = false;
                keys.push_back(physLabel(ps, pad));
                row.phys.insert(row.phys.end(), ps.begin(), ps.end());
                rest = plus == std::string::npos ? std::string() : rest.substr(plus + 1);
            }
            if (!ok) continue;
            if (keys.size() > 1) {
                for (size_t i = 0; i < keys.size(); i++) row.keys += (i ? " + " : "") + keys[i];
            }
            r.rows.push_back(row);
        }
        return r;
    }

    const ReadResult rr = readEmulator(env, ctx, cat);
    r.console = rr.console.empty() ? ctx.systemId : rr.console;
    if (const std::string k = knownSystem(cat, r.console); !k.empty()) r.console = k;
    auto sit = cat.systems.find(r.console);
    const SystemArt* sa = sit == cat.systems.end() ? nullptr : &sit->second;
    r.consoleName = sa && !sa->name.empty() ? sa->name : r.console;
    r.drawing = !rr.drawing.empty() ? rr.drawing : sa ? sa->drawing : std::string();
    const Drawing* dr = cat.drawing(r.drawing);
    if (!dr) r.drawing.clear();
    r.emulatorName = clipText(rr.emulatorName);
    r.source = rr.source;
    r.notMappedWhere = rr.notMappedWhere;
    for (const auto& n : rr.notes) r.notes.push_back(clipText(n));

    for (const auto& b : rr.binds) {
        Row row;
        row.consoleId = b.id;
        row.note = b.note;
        if (b.id.empty()) {
            row.consoleName = b.name;
        } else {
            const size_t colon = b.id.find(':');
            if (colon == std::string::npos) {
                row.consoleName = anchorName(b.id, dr, sa, b.name);
            } else {
                const std::string base = b.id.substr(0, colon), dir = b.id.substr(colon + 1);
                std::string dn = dir;
                if (dir == "up") dn = kDirNames[0];
                else if (dir == "down") dn = kDirNames[1];
                else if (dir == "left") dn = kDirNames[2];
                else if (dir == "right") dn = kDirNames[3];
                row.consoleName = b.name.empty() ? anchorName(base, dr, sa, base) + " " + dn : b.name;
            }
        }
        for (const auto& in : b.in) {
            const std::vector<Phys> ps = pad.physFor(in);
            for (Phys p : ps)
                if (std::find(row.phys.begin(), row.phys.end(), p) == row.phys.end()) row.phys.push_back(p);
            if (ps.empty() && !in.none() && row.offPad.empty()) row.offPad = clipText(inputOffPadText(in));
        }
        std::sort(row.phys.begin(), row.phys.end());
        if (!row.phys.empty()) row.offPad.clear();
        for (const auto& chord : b.chords) {
            std::string k;
            for (const auto& in : chord) {
                const std::vector<Phys> ps = pad.physFor(in);
                if (ps.empty()) { k.clear(); break; }
                k += (k.empty() ? "" : " + ") + pad.labelOf(ps[0]);
            }
            if (k.empty()) continue;
            if (!row.phys.empty() || !row.keys.empty()) k = (row.keys.empty() ? physLabel(row.phys, pad) : row.keys) + " / " + k;
            row.keys = k;
            row.offPad.clear();
        }
        r.rows.push_back(row);
    }

    static const std::string kDpad[4] = {"dpad-up", "dpad-down", "dpad-left", "dpad-right"};
    static const std::string kLs[4] = {"stick-l:up", "stick-l:down", "stick-l:left", "stick-l:right"};
    static const std::string kRs[4] = {"stick-r:up", "stick-r:down", "stick-r:left", "stick-r:right"};
    if (!dr || dr->find("dpad")) collapseGroup(&r.rows, kDpad, "dpad", anchorName("dpad", dr, sa, "D-pad"));
    collapseGroup(&r.rows, kLs, "stick-l", anchorName("stick-l", dr, sa, "Left stick"));
    collapseGroup(&r.rows, kRs, "stick-r", anchorName("stick-r", dr, sa, "Right stick"));

    // The drawing's own order (its anchors run around the controller), text rows after.
    auto order = [&](const Row& row) {
        if (!dr || row.consoleId.empty()) return 100000;
        const std::string base = row.consoleId.substr(0, row.consoleId.find(':'));
        for (size_t i = 0; i < dr->anchors.size(); i++)
            if (dr->anchors[i].id == base) return (int)i;
        if (const Anchor* a = dr->find(base))
            for (size_t i = 0; i < dr->anchors.size(); i++)
                if (&dr->anchors[i] == a) return (int)i;
        return 100000;
    };
    std::stable_sort(r.rows.begin(), r.rows.end(), [&](const Row& a, const Row& b) { return order(a) < order(b); });

    for (const auto& h : rr.hotkeys) {
        std::string keys;
        bool ok = !h.chord.empty();
        for (const auto& in : h.chord) {
            const std::vector<Phys> ps = pad.physFor(in);
            if (ps.empty()) { ok = false; break; }   // a key the pad can't press is not shown
            keys += (keys.empty() ? "" : " + ") + pad.labelOf(ps[0]);
        }
        if (!ok) continue;
        // Another input for the same action shares its row, as alternatives do in the controls.
        auto same = std::find_if(r.hotkeys.begin(), r.hotkeys.end(), [&](const HotkeyRow& x) { return x.action == h.action; });
        if (same == r.hotkeys.end()) r.hotkeys.push_back({keys, h.action});
        else if ((" / " + same->keys + " / ").find(" / " + keys + " / ") == std::string::npos) same->keys += " / " + keys;
    }
    return r;
}

std::map<std::string, std::string> profileActions(const std::string& profileJson) {
    std::map<std::string, std::string> out;
    njson::Value v;
    if (profileJson.size() > (1u << 20) || !njson::parse(profileJson, &v)) return out;
    if (const njson::Value* a = v.find("actions"); a && a->isObject())
        for (const auto& kv : a->obj)
            if (kv.second.isObject()) out[kv.first] = kv.second.getString("driver", "none");
    return out;
}

std::vector<HotkeyRow> chordRows(const std::map<std::string, std::string>& actions, const ReaderEnv& env,
                                 const PadModel& pad, const std::string& fn, bool tapIsBack) {
    std::vector<HotkeyRow> rows;
    auto prop = [&](const char* k, const char* def) {
        const std::string v = env.prop_(k);
        return v.empty() ? std::string(def) : v;
    };
    auto active = [&](const char* a) {
        auto it = actions.find(a);
        return it != actions.end() && it->second != "none";
    };
    if (prop("persist.gammaos.hotkeys.enable", "1") != "0") {
        static const struct { const char* action; const char* prop; const char* def; const char* text; } kHot[] = {
            {"save_state", "persist.gammaos.hotkeys.save", "BUTTON_R1", "Save state"},
            {"load_state", "persist.gammaos.hotkeys.load", "BUTTON_L1", "Load state"},
            {"fast_forward", "persist.gammaos.hotkeys.fastforward", "BUTTON_R2", "Fast forward"},
            {"rewind", "persist.gammaos.hotkeys.rewind", "BUTTON_L2", "Rewind"},
            {"quit", "persist.gammaos.hotkeys.quit", "NONE", "Quit game"},
        };
        bool any = false;
        for (const auto& h : kHot) {
            if (!active(h.action)) continue;
            const Phys p = settingButton(prop(h.prop, h.def), pad);
            if (p == Phys::kCount) continue;
            rows.push_back({fn + " + " + pad.labelOf(p), h.text});
            any = true;
        }
        if (active("quit") && prop("persist.gammaos.hotkeys.quithold", "1") != "0") {
            char buf[160];
            snprintf(buf, sizeof(buf), "Hold %s for 3 s", fn.c_str());
            rows.push_back({buf, "Quit game"});
            any = true;
        }
        if (any && prop("persist.gammaos.hotkeys.startselect", "1") != "0")
            rows.push_back({"Start + Select", "Works in place of the function button for these"});
    }
    if (tapIsBack) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Hold %s for 1 s", fn.c_str());
        rows.push_back({buf, "Quick Menu"});
    } else {
        rows.push_back({fn, "Quick Menu"});
    }
    rows.push_back({fn + " + " + pad.labelOf(Phys::Start), "Applications"});
    rows.push_back({fn + " + " + pad.labelOf(Phys::Select), "Shader on/off"});
    rows.push_back({fn + " + Volume", "Brightness"});
    const Phys bm = settingButton(prop("persist.gammaos.hotkeys.buttonmap", "FACE_SOUTH"), pad);
    if (bm != Phys::kCount) rows.push_back({fn + " + " + pad.labelOf(bm), "Button map"});
    return rows;
}

// ---- what Nano, PhoneWindowManager and drastic-nano feed in ----

std::vector<PadSpec> padSpecsFromJson(const std::string& json) {
    std::vector<PadSpec> out;
    njson::Value v;
    if (json.size() > (1u << 20) || !njson::parse(json, &v) || !v.isObject()) return out;
    const njson::Value* pads = v.find("pads");
    if (!pads || !pads->isArray()) return out;
    for (const auto& p : pads->arr) {
        PadSpec s;
        if (p.isObject() && padSpecFromJson(njson::serialize(p), &s)) out.push_back(std::move(s));
    }
    return out;
}

std::vector<PadSpec> loadPadSpecsFrom(const std::string& dataDir) {
    std::string json;
    if (!readSmallFile(dataDir + "/pads.json", &json, 1u << 20)) return {};
    return padSpecsFromJson(json);
}

GamepadState gamepadStateFrom(const std::function<std::string(const std::string&)>& prop) {
    GamepadState g;
    auto num = [&](const char* k, int def) {
        const std::string v = prop(k);
        if (v.empty()) return def;
        char* end = nullptr;
        const long n = strtol(v.c_str(), &end, 0);
        return end && *end == '\0' ? (int)n : def;
    };
    g.running = prop("init.svc.gammapad") == "running";
    const std::string swap = prop("persist.gammaos.gamepad.abxy_swap");
    g.swap = swap == "1" || swap == "true" || swap == "y" || swap == "yes" || swap == "on";
    g.remap = prop("persist.gammaos.gamepad.remap_btn");
    g.vid = num("persist.gammaos.gamepad.device_vid", 0x045e);
    g.pid = num("persist.gammaos.gamepad.device_pid", 0x0b13);
    const std::string n = prop("persist.gammaos.gamepad.device_name");
    if (!n.empty()) g.name = n;
    g.merge = num("persist.gammaos.gamepad.merge", 1) != 0;
    g.hideSource = num("persist.gammaos.gamepad.hide_source", 1) != 0;
    return g;
}

PadModel padModelFor(const DeviceId& dev, const std::vector<PadSpec>& specs, const GamepadState& gp) {
    GamepadState off = gp;
    off.running = false;
    const bool known = dev.vendor || dev.product;
    if (known && gp.running && dev.vendor == gp.vid && dev.product == gp.pid && !specs.empty())
        return padModel(specs[0], gp);
    if (known) {
        for (const auto& s : specs)
            if (s.vendor == dev.vendor && s.product == dev.product) return padModel(s, off);
        PadSpec spec = standardPadSpec(dev);
        if (!dev.name.empty()) spec.name = dev.name;
        return padModel(spec, off);
    }
    if (specs.empty()) return padModel(standardPadSpec(dev), off);
    return padModel(specs[0], gp);
}

int buttonMapKeycode(const std::string& setting, const PadModel& m) {
    if (setting.rfind("FACE_", 0) != 0) return setting == "NONE" ? 0 : keycodeFromName(setting);
    const Phys p = settingButton(setting, m);
    if (p == Phys::kCount) return 0;
    const Input in = m.input(p, Input::kKey);
    return in.kind == Input::kKey ? in.code : 0;
}

int buttonMapEvdev(const std::string& setting, const PadModel& m) {
    const Phys p = settingButton(setting, m);
    if (p == Phys::kCount) return 0;
    const Input in = m.input(p, Input::kEvKey);
    return in.kind == Input::kEvKey ? in.code : 0;
}

namespace {

const std::pair<Phys, const char*> kFaceSettings[] = {
    {Phys::South, "FACE_SOUTH"}, {Phys::East, "FACE_EAST"}, {Phys::North, "FACE_NORTH"}, {Phys::West, "FACE_WEST"}};

// The Button Map row's options besides FACE_* (button_map_overlay_plan.md §1.2).
bool buttonMapTakesKeycode(const std::string& v) {
    static const char* const kOk[] = {"BUTTON_L1", "BUTTON_R1", "BUTTON_L2", "BUTTON_R2", "BUTTON_THUMBL", "BUTTON_THUMBR"};
    for (const char* k : kOk) if (v == k) return true;
    return false;
}

// value, which another row held, in the words of a row of the other kind; "" when it has none.
std::string inRowWords(const std::string& value, bool toButtonMap, const PadModel& m) {
    if (value == "NONE") return value;
    const Phys p = settingButton(value, m);
    if (toButtonMap) {
        if (value.rfind("FACE_", 0) == 0 || buttonMapTakesKeycode(value)) return value;
        for (const auto& f : kFaceSettings) if (f.first == p) return f.second;
        return std::string();
    }
    if (value.rfind("FACE_", 0) != 0) return value;
    if (p == Phys::kCount) return std::string();
    const Input in = m.input(p, Input::kKey);
    return in.kind == Input::kKey ? keycodeName(in.code) : std::string();
}

}  // namespace

std::vector<std::pair<int, std::string>> takeButton(const std::vector<ButtonRow>& rows, int row,
                                                    const std::string& value, const PadModel& m) {
    std::vector<std::pair<int, std::string>> out;
    if (row < 0 || row >= (int)rows.size() || value == "NONE") return out;
    const std::string old = rows[row].value;
    for (int i = 0; i < (int)rows.size(); i++) {
        if (i == row || !sameButton(rows[i].value, value, m)) continue;
        std::string give = inRowWords(old, rows[i].buttonMap, m);
        if (give.empty() || (give != "NONE" && sameButton(give, value, m))) give = "NONE";
        if (give == "NONE" && rows[i].buttonMap) {
            // The first face button no other row holds once the change is made.
            for (const auto& f : kFaceSettings) {
                if (!m.has(f.first)) continue;
                bool held = false;
                for (int j = 0; j < (int)rows.size() && !held; j++) {
                    if (j == i) continue;
                    held = settingButton(j == row ? value : rows[j].value, m) == f.first;
                }
                if (!held) { give = f.second; break; }
            }
        }
        out.push_back({i, give});
    }
    return out;
}

ContextInputs menuContextInputs(const MenuFocus& f, Trigger trigger) {
    ContextInputs in;
    in.where = Where::kNanoMenus;
    in.trigger = trigger;
    if (f.modal || f.atCategories) return in;
    switch (f.item) {
        case MenuFocus::kSystem: in.focus = ContextInputs::kFocusSystem; break;
        case MenuFocus::kGame: in.focus = ContextInputs::kFocusGame; break;
        case MenuFocus::kRecent: in.focus = ContextInputs::kFocusRecent; break;
        case MenuFocus::kApp: in.focus = ContextInputs::kFocusAppRow; in.appPkg = f.appPkg; return in;
        case MenuFocus::kOther: in.focus = ContextInputs::kFocusOther; return in;
        case MenuFocus::kNoItem: return in;
    }
    in.systemId = f.systemId;
    in.coreSo = f.coreSo;
    in.launchPkg = f.launchPkg;
    if (f.item == MenuFocus::kGame && f.gameEmu) {
        in.coreSo = f.gameCoreSo;
        in.launchPkg = f.gamePkg;
    }
    if (f.item != MenuFocus::kSystem) in.romPath = f.romPath;
    return in;
}

void launchStateFrom(const std::function<std::string(const std::string&)>& prop, OverAppState* s,
                     const std::string& gameFile) {
    const std::string t = prop("sys.gammaos.nano.launch_time");
    s->launchSystem = prop("sys.gammaos.nano.launch_system");
    s->launchGame = prop("sys.gammaos.nano.launch_game");
    const size_t nl = gameFile.find('\n');
    if (s->launchGame.empty() && !t.empty() && nl != std::string::npos && gameFile.compare(0, nl, t) == 0)
        s->launchGame = gameFile.substr(nl + 1);
    s->launchCore = prop("sys.gammaos.nano.launch_game_core");
    s->launchApp = prop("sys.gammaos.nano.launch_app");
    s->launchRom = s->launchGame;
    if (s->launchSystem.empty() && s->launchGame.empty()) {
        s->launchCore = prop("sys.gammaos.nano.launch_core");
        s->launchRom = prop("sys.gammaos.nano.launch_rom");
    }
    char* end = nullptr;
    const long long v = t.empty() ? 0 : strtoll(t.c_str(), &end, 10);
    s->launchTime = end && *end == '\0' && v > 0 ? (int64_t)v : 0;
}

ContextInputs overAppContextInputs(const OverAppState& s) {
    ContextInputs in;
    in.where = Where::kOverGame;
    in.trigger = Trigger::kFunctionSouth;
    in.fgPkg = s.fgPkg;
    in.launchCore = s.launchCore;
    in.launchRom = s.launchRom;
    in.launchSystem = s.launchSystem;
    in.launchGame = s.launchGame;
    in.launchApp = s.launchApp;
    HistoryEntry h;
    if (!s.historyJson.empty() && parseContentHistory(s.historyJson, &h)) {
        const bool outsideNano = s.launchCore.empty();
        const bool newer = s.launchTime > 0 && s.historyMtime >= s.launchTime;
        if (outsideNano || newer) {
            in.historyNewer = true;
            in.historyCore = h.corePath;
            in.historyPath = h.path;
        }
    }
    return in;
}

// ---- translation ----

namespace {

bool isUiKey(const std::string& s) {
    static const std::set<std::string> keys = [] {
        std::set<std::string> k;
        for (const char* u : uiStrings()) k.insert(u);
        return k;
    }();
    return keys.count(s) != 0;
}

std::string trKey(const std::string& english, const Translator& tr) {
    if (!tr || !isUiKey(english)) return english;
    const char* t = tr(english.c_str());
    return t && *t ? std::string(t) : english;
}

// english matches a "...%s..." key: the key's translation with the argument (itself translated).
bool trFormat(const std::string& english, const Translator& tr, std::string* out) {
    size_t bestFixed = 0;
    std::string best;
    for (const char* u : uiStrings()) {
        const std::string key = u;
        const size_t at = key.find("%s");
        if (at == std::string::npos || key.find('%', at + 2) != std::string::npos) continue;
        const std::string pre = key.substr(0, at), post = key.substr(at + 2);
        if (english.size() <= pre.size() + post.size()) continue;
        if (english.compare(0, pre.size(), pre) || english.compare(english.size() - post.size(), post.size(), post)) continue;
        if (pre.size() + post.size() < bestFixed || (best.size() && pre.size() + post.size() == bestFixed)) continue;
        bestFixed = pre.size() + post.size();
        best = key;
    }
    if (best.empty()) return false;
    const size_t at = best.find("%s");
    const std::string arg = english.substr(at, english.size() - (best.size() - 2));
    const char* t = tr ? tr(best.c_str()) : nullptr;
    std::string fmt = t && *t ? std::string(t) : best;
    const size_t slot = fmt.find("%s");
    if (slot == std::string::npos) return false;
    *out = fmt.substr(0, slot) + translate(arg, tr) + fmt.substr(slot + 2);
    return true;
}

}  // namespace

std::string translate(const std::string& english, const Translator& tr) {
    if (english.empty() || !tr) return english;
    if (isUiKey(english)) return trKey(english, tr);
    // Composed text: alternatives, then chords, then the badge's parts.
    for (const char* sep : {" \xC2\xB7 ", " / ", " + "}) {
        if (english.find(sep) == std::string::npos) continue;
        std::string out;
        size_t i = 0;
        const size_t n = strlen(sep);
        while (true) {
            const size_t e = english.find(sep, i);
            out += translate(english.substr(i, e == std::string::npos ? std::string::npos : e - i), tr);
            if (e == std::string::npos) break;
            out += sep;
            i = e + n;
        }
        return out;
    }
    std::string out;
    if (trFormat(english, tr, &out)) return out;
    return english;
}

const std::vector<const char*>& uiStrings() {
    static const std::vector<const char*> k = {
        // pad and rows
        "Bottom", "Right", "Top", "Left", "D-pad", "Left stick", "Right stick", "Up", "Down",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Left stick Up", "Left stick Down",
        "Left stick Left", "Left stick Right", "Right stick Up", "Right stick Down", "Right stick Left",
        "Right stick Right", "Home", "Keyboard %s", "Cursor mode: %s",
        // panel
        "Button Map", "Controls", "Hotkeys", "Browser Controls", "Console button", "Your controller",
        "Not mapped in %s", "Default mapping", "From your %s settings", "Per-game layout", "Close",
        "Table", "Drawing", "Page", "More", "Reading settings...",
        // the browser
        "Browser controls need Controller Enable (Settings > Gamepad Settings)",
        "Controller Bindings is off (Quick Menu)",
        "Enter", "Left click (hold to drag)", "Back", "Refresh", "Right click", "Bookmark this page", "Previous tab",
        "Next tab", "Right click (hold to drag)", "Close tab", "New tab", "Scroll", "Mouse cursor", "Arrow keys",
        "Move the cursor", "Address bar", "Bookmarks", "Cursor mode on/off",
        "Passes through to the system (hotkeys, hold to quit)",
        // hotkeys
        "Save state", "Load state", "Fast forward", "Rewind", "Quit game", "Hold %s for 3 s", "Hold %s for 1 s",
        "Works in place of the function button for these", "Quick Menu", "Applications", "Shader on/off",
        "Brightness", "Button map",
        // the badge's notes, from the readers
        "Bound to %s", "Classic Controller binds not shown", "Core override: %s", "Folder override: %s",
        "Game override: %s", "Game profile: %s", "Game settings: %s", "Key map profile %s", "Macros not shown",
        "Mapping read from DraStic profile 1 (assumed)", "Mapping: %s", "No reader for %s",
        "Older mapping file (%s), defaults shown", "Pad profile: %s", "Profile: %s",
        "Profile %s not known, default profile shown", "Profile for this controller", "Profile for this model",
        "Portrait controls turn the D-pad and buttons with the screen", "RetroPad layout (core not in the table)",
        "Settings file not readable by WatermelonDS, defaults shown", "Left stick: %s mode",
        "Right stick: %s mode", "This game may add its own keys", "Game remap: %s", "Folder remap: %s",
        "Core remap: %s", "Analog controller", "DualShock", "Buttons shifted clockwise (core option)", "Action",
        // the generic button names, and the readers' action and row names
        "Start", "Select", "Volume", "Hinge", "Another controller", "Move the pointer", "Left click", "Skip line",
        "Skip", "Game menu", "Pause", "Virtual keyboard", "Slow mouse", "Fast forward (hold)", "Pause menu",
        "Next state slot", "Previous state slot", "Reset", "Quit RetroArch", "Quit PPSSPP", "Quit Flycast",
        "RetroArch menu", "Flycast menu", "DraStic menu", "M64Plus menu", "hakuX menu", "ScummVM menu",
        "Close game", "Close lid", "Swap screens", "Touch cursor",
        // retropad.json's console controls and notes (printed letters and symbols stay as they are)
        "Previous audio mode", "Next audio mode", "(FDS) Next disk", "(FDS) Switch side", "(FDS) Disk side",
        "(FDS) Insert/eject", "(VS) Coin", "(VS) Coin 1", "(VS) Coin 2", "(Famicom) Microphone",
        "With RetroArch's fast-forward override only", "With the core's Turbo Enable option", "C-button mode (hold)",
        "Low battery", "6-button games", "6-button pad", "(ST-V) Test", "(ST-V) Service", "(ST-V) Coin",
        "Mode 2/6 switch", "Trigger", "Booster", "Left difficulty", "Right difficulty", "Left difficulty A",
        "Right difficulty A", "Left difficulty B", "Right difficulty B", "Color", "Black and white", "Keypad", "Mouse",
        "Rotate", "Turbo %s", "Solar sensor %s", "%s (hold R2)", "Fire %s", "Button %s",
    };
    return k;
}

}  // namespace bmap
}  // namespace android
