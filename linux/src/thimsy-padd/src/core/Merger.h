// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The merger: source events in, the virtual pad's events out. Pure: no I/O, no clock.
//
// Each source's events are held until its SYN_REPORT, then applied together, and the output is
// the difference between what the pad should now show and what it last showed, in a fixed order
// (keys by code, then axes by code), followed by one SYN_REPORT per device that changed.
//
// Intercept (sys.thimsy.padd.intercept): turning it on releases every held output and centres
// every axis on the pad within one batch; from then on the pad gets nothing and the same events
// go to the tap (Dev::Tap) instead, for Nano's menu. Turning it off does the same the other way.
// On either switch the inputs held at that moment are suppressed until they are released, so a
// hotkey held while the menu opens never reaches the menu as a press, and a button held when it
// closes never reaches the game; axes resume at their current position.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/Calibrate.h"
#include "core/PadSpec.h"

namespace padd {

struct SourceEvent {
    int source = -1;  // index into PadSpec::sources; anything else is ignored
    uint16_t type = 0;
    uint16_t code = 0;
    int32_t value = 0;
    int64_t timeNs = 0;
};

enum class Dev : uint8_t { Pad, Keys, Tap };

struct OutEvent {
    Dev dev = Dev::Pad;
    uint16_t type = 0;
    uint16_t code = 0;
    int32_t value = 0;
    bool operator==(const OutEvent& o) const {
        return dev == o.dev && type == o.type && code == o.code && value == o.value;
    }
};

// What the virtual devices declare. The pad always has ABS_X and ABS_Y (centred and still when
// the device has no stick), because libudev-zero and SDL's evdev fallback only call a device
// with both a joystick. keysKeys empty means there is no "Thimsy Keys" device.
struct Layout {
    std::set<uint16_t> padKeys;
    std::map<uint16_t, AbsInfo> padAbs;  // min, max, fuzz, flat of each output axis
    std::set<uint16_t> keysKeys;
    bool ff = false;
};

// The layout for a spec; caps[i] is source i's capabilities when known (pass-through uses them;
// when unknown, the standard gamepad buttons and the hat).
Layout computeLayout(const PadSpec& spec, const std::vector<std::optional<SourceCaps>>& caps);

class Merger {
 public:
    Merger(const PadSpec& spec, const std::vector<std::optional<SourceCaps>>& caps);

    const Layout& layout() const { return layout_; }

    // One source event. Output appears on its source's SYN_REPORT.
    void process(const SourceEvent& ev, std::vector<OutEvent>* out);

    // After SYN_DROPPED the source's events are ignored up to its next SYN_REPORT; then
    // needsSync() is true until syncSource() gives its whole state (EVIOCGKEY, EVIOCGABS).
    bool needsSync(int source) const;
    // Replaces the source's state: keysDown are the codes pressed, abs the axes' values. Also
    // used when a node is (re)opened.
    void syncSource(int source, const std::set<uint16_t>& keysDown,
                    const std::map<uint16_t, int32_t>& abs, std::vector<OutEvent>* out);
    // Gives the axis ranges of a node opened after the merger was made.
    void attachSource(int source, const SourceCaps& caps);
    // The node went away: everything it held is released, its axes return to rest.
    void dropSource(int source, std::vector<OutEvent>* out);

    void setIntercept(bool on, std::vector<OutEvent>* out);
    bool intercept() const { return intercept_; }

    // The value the active stream last showed for an output (tests).
    int32_t shown(uint16_t type, uint16_t code) const;

 private:
    struct Slot {
        Dev dev;  // Pad or Keys
        uint16_t type;
        uint16_t code;
        int32_t neutral;
    };
    struct Axis {
        AxisMap map;
        AxisRange range;
        std::optional<int32_t> raw;
    };
    using Input = std::pair<int, uint16_t>;  // source, code

    void apply(const SourceEvent& ev);
    bool routed(const Input& in) const;
    std::vector<Target> routesOf(const Input& in) const;
    std::vector<int32_t> desired() const;
    void flush(std::vector<OutEvent>* out);
    void emitTo(const std::vector<int32_t>& target, bool tap, std::vector<OutEvent>* out);
    void suppressHeld();
    std::vector<int32_t> neutral() const;

    PadSpec spec_;
    Layout layout_;
    std::vector<Slot> slots_;
    std::map<std::pair<uint16_t, uint16_t>, size_t> slotIndex_;  // (type, code) -> slot
    std::map<Input, std::vector<Target>> buttonRoutes_;
    std::vector<Axis> axes_;
    std::map<std::pair<int, uint16_t>, std::vector<size_t>> axisRoutes_;  // (source, ABS_) -> axes_
    int passSource_ = -1;  // the kernel-joypad source when passing through

    std::set<Input> held_;
    std::set<Input> suppressed_;
    std::map<uint16_t, int32_t> passHat_;  // ABS_HAT0X/Y passed through from passSource_

    std::vector<std::vector<SourceEvent>> pending_;
    std::vector<bool> dropping_;
    std::vector<bool> needsSync_;

    bool intercept_ = false;
    std::vector<int32_t> shownLive_;
    std::vector<int32_t> shownTap_;
};

}  // namespace padd
