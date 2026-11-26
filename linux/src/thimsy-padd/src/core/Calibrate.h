// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Axis calibration: a source axis's raw range, centre and deadzone, rescaled to the virtual
// pad's fixed ranges. Values device.json leaves out come from the node's EVIOCGABS.
#pragma once

#include <cstdint>
#include <optional>

namespace padd {

// What EVIOCGABS reports for one axis (struct input_absinfo without the kernel header).
struct AbsInfo {
    int32_t value = 0;
    int32_t min = 0;
    int32_t max = 0;
    int32_t fuzz = 0;
    int32_t flat = 0;
    int32_t resolution = 0;
};

// device.json's calibration object; every field optional.
struct Calibration {
    std::optional<int32_t> min, max, center, deadzone, fuzz, flat;
};

// The virtual pad's ranges (kernel gamepad.rst leaves them open; SDL rescales any range).
constexpr int32_t kStickMin = -32768;
constexpr int32_t kStickMax = 32767;
constexpr int32_t kTriggerMin = 0;
constexpr int32_t kTriggerMax = 255;
constexpr int32_t kHatMin = -1;
constexpr int32_t kHatMax = 1;

// A resolved range. valid is false when neither device.json nor the node gave min and max;
// then values pass through, clamped to the output range.
struct AxisRange {
    int32_t min = 0;
    int32_t max = 0;
    int32_t center = 0;
    int32_t deadzone = 0;
    bool invert = false;
    bool valid = false;
};

// device.json first, then the node's absinfo (nullptr when unknown). The deadzone is the
// calibration's deadzone, else its flat, else the node's flat. The centre defaults to the
// middle of the range for a stick and to min for a trigger.
AxisRange resolveRange(const Calibration& cal, const AbsInfo* node, bool invert, bool trigger);

// A stick axis: the centre maps to 0, min to kStickMin and max to kStickMax (or the reverse
// when inverted); inside the deadzone around the centre the result is 0.
int32_t scaleStick(const AxisRange& r, int32_t raw);

// A trigger: rest (min, or max when inverted) maps to kTriggerMin, the other end to kTriggerMax;
// inside the deadzone above rest the result is kTriggerMin.
int32_t scaleTrigger(const AxisRange& r, int32_t raw);

}  // namespace padd
