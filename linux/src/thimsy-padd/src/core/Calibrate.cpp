// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Calibrate.h"

#include <algorithm>

namespace padd {
namespace {

int32_t clampTo(int64_t v, int32_t lo, int32_t hi) {
    return static_cast<int32_t>(std::max<int64_t>(lo, std::min<int64_t>(hi, v)));
}

// (num * scale) / den rounded to nearest, for num >= 0 and den > 0.
int64_t scaleRound(int64_t num, int64_t scale, int64_t den) {
    return (num * scale + den / 2) / den;
}

}  // namespace

AxisRange resolveRange(const Calibration& cal, const AbsInfo* node, bool invert, bool trigger) {
    AxisRange r;
    r.invert = invert;
    bool haveMin = cal.min.has_value() || node != nullptr;
    bool haveMax = cal.max.has_value() || node != nullptr;
    r.min = cal.min ? *cal.min : (node ? node->min : 0);
    r.max = cal.max ? *cal.max : (node ? node->max : 0);
    r.valid = haveMin && haveMax && r.max > r.min;
    if (cal.center) {
        r.center = *cal.center;
    } else if (trigger) {
        r.center = r.min;
    } else {
        r.center = static_cast<int32_t>((static_cast<int64_t>(r.min) + r.max) / 2);
    }
    if (cal.deadzone) {
        r.deadzone = *cal.deadzone;
    } else if (cal.flat) {
        r.deadzone = *cal.flat;
    } else {
        r.deadzone = node ? node->flat : 0;
    }
    if (r.deadzone < 0) r.deadzone = 0;
    if (r.valid) r.center = clampTo(r.center, r.min, r.max);
    return r;
}

int32_t scaleStick(const AxisRange& r, int32_t raw) {
    if (!r.valid) {
        int64_t v = r.invert ? -static_cast<int64_t>(raw) : raw;
        return clampTo(v, kStickMin, kStickMax);
    }
    int64_t d = static_cast<int64_t>(clampTo(raw, r.min, r.max)) - r.center;
    if (r.invert) d = -d;
    int64_t dz = r.deadzone;
    if (d >= -dz && d <= dz) return 0;
    // After inversion the positive side is the side towards min.
    int64_t posSpan = (r.invert ? static_cast<int64_t>(r.center) - r.min
                                : static_cast<int64_t>(r.max) - r.center) - dz;
    int64_t negSpan = (r.invert ? static_cast<int64_t>(r.max) - r.center
                                : static_cast<int64_t>(r.center) - r.min) - dz;
    if (d > 0) {
        if (posSpan <= 0) return kStickMax;
        return clampTo(scaleRound(d - dz, kStickMax, posSpan), 0, kStickMax);
    }
    if (negSpan <= 0) return kStickMin;
    return clampTo(-scaleRound(-d - dz, -static_cast<int64_t>(kStickMin), negSpan), kStickMin, 0);
}

int32_t scaleTrigger(const AxisRange& r, int32_t raw) {
    if (!r.valid) return clampTo(raw, kTriggerMin, kTriggerMax);
    int64_t v = r.invert ? static_cast<int64_t>(r.max) - clampTo(raw, r.min, r.max)
                         : static_cast<int64_t>(clampTo(raw, r.min, r.max)) - r.min;
    int64_t dz = r.deadzone;
    if (v <= dz) return kTriggerMin;
    int64_t span = static_cast<int64_t>(r.max) - r.min - dz;
    if (span <= 0) return kTriggerMax;
    return clampTo(kTriggerMin + scaleRound(v - dz, kTriggerMax - kTriggerMin, span), kTriggerMin,
                   kTriggerMax);
}

}  // namespace padd
