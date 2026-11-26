// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// device.json's pad section (linux/schema/device.schema.json, "pad"), read into the form the
// merger uses: sources by index, codes by number. Also the pure parts of finding a source node:
// matching a node's identity and checking its capabilities.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/Calibrate.h"

namespace thimsy {
namespace json {
class Value;
}
}  // namespace thimsy

namespace padd {

// The virtual pad's identity (decision I13).
constexpr const char* kPadName = "Thimsy Gamepad";
constexpr const char* kKeysName = "Thimsy Keys";
constexpr uint16_t kBusVirtual = 0x06;
constexpr uint16_t kVendor = 0x5448;
constexpr uint16_t kPadProduct = 0x0001;
constexpr uint16_t kKeysProduct = 0x0002;
constexpr uint16_t kVersion = 1;

// How an evdev node is recognised: every field given must equal the node's.
struct Match {
    std::string name;
    std::optional<std::string> phys;
    std::optional<std::string> uniq;
    int bus = -1, vendor = -1, product = -1;
};

// What EVIOCGNAME, EVIOCGPHYS, EVIOCGUNIQ and EVIOCGID report.
struct NodeInfo {
    std::string name, phys, uniq;
    int bus = 0, vendor = 0, product = 0, version = 0;
};

bool matches(const Match& m, const NodeInfo& node);

// A node's capabilities (EVIOCGBIT for EV_KEY, EV_ABS with EVIOCGABS, EV_FF).
struct SourceCaps {
    std::set<uint16_t> keys;
    std::map<uint16_t, AbsInfo> abs;
    std::set<uint16_t> ff;  // FF_* bits
};

enum class Role { Buttons, Sticks, ButtonsSticks, Rumble };
enum class Producer { Padd, KernelJoypad };

struct Source {
    std::string id;
    Role role = Role::Buttons;
    Match match;
    bool required = true;
    bool grab = true;
    bool hide = true;
};

// Where a button goes: a key on the pad, a d-pad direction (folded into ABS_HAT0X/HAT0Y), or a
// KEY_* on padd's keyboard ("Thimsy Keys").
enum class TargetKind { PadKey, Hat, KeysKey };
struct Target {
    TargetKind kind = TargetKind::PadKey;
    uint16_t code = 0;  // BTN_* (PadKey), BTN_DPAD_* (Hat), KEY_* (KeysKey)
    bool operator<(const Target& o) const {
        return kind != o.kind ? kind < o.kind : code < o.code;
    }
    bool operator==(const Target& o) const { return kind == o.kind && code == o.code; }
};

// Classifies a normalised button name's code (BTN_DPAD_* -> Hat, KEY_* -> KeysKey, else PadKey).
Target targetFor(uint16_t code);

struct ButtonMap {
    int source = -1;
    uint16_t code = 0;
    Target to;
    std::string label;
};

struct AxisMap {
    int source = -1;
    uint16_t code = 0;  // the source's ABS_*
    uint16_t to = 0;    // ABS_X, ABS_Y, ABS_RX, ABS_RY (sticks), ABS_Z, ABS_RZ (triggers)
    bool trigger = false;
    bool invert = false;
    Calibration cal;
};

struct PadSpec {
    Producer producer = Producer::Padd;
    std::vector<Source> sources;
    std::vector<ButtonMap> buttons;
    // kernel-joypad with an empty button list: the node's EV_KEY codes pass through unchanged
    // (BTN_DPAD_* still become the hat), and so do its ABS_HAT0X/ABS_HAT0Y.
    bool passThrough = false;
    std::vector<AxisMap> axes;
    int rumbleSource = -1;
    int faceButtons = 0;
    std::string faceLayout;
    int hotkey = -1;  // a normalised code, or -1
    std::vector<uint16_t> quitCombo;

    int sourceIndex(std::string_view id) const;
};

// Reads the "pad" member of a whole device.json document. Returns false with *err on anything
// that would make the pad wrong (unknown source ids or codes, a target used twice by axes, a
// kernel-joypad pad with more than one input source); *warnings gets what is only suspicious.
bool parsePadSpec(const thimsy::json::Value& device, PadSpec* out, std::string* err,
                  std::vector<std::string>* warnings = nullptr);
bool loadPadSpec(const std::string& path, PadSpec* out, std::string* err,
                 std::vector<std::string>* warnings = nullptr);

// Whether a node with these capabilities can serve source i: "" if it can, else why not.
// Buttons need EV_KEY with at least one mapped code (any key when passing through); sticks need
// every mapped axis; rumble needs FF_RUMBLE.
std::string checkCaps(const PadSpec& spec, int source, const SourceCaps& caps);

// The first source, not yet taken (taken[i]), whose match and capabilities fit; -1 if none.
// An external pad, or any node device.json does not name, gets -1 and is left alone.
int pickSource(const PadSpec& spec, const NodeInfo& node, const SourceCaps& caps,
               const std::vector<bool>& taken, std::string* why = nullptr);

}  // namespace padd
