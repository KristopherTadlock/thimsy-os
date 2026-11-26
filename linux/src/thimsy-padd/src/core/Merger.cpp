// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/Merger.h"

#include <linux/input.h>

#include <algorithm>
#include <climits>

namespace padd {
namespace {

AbsInfo outRange(int32_t min, int32_t max) {
    AbsInfo a;
    a.min = min;
    a.max = max;
    return a;
}

bool isTriggerOut(uint16_t code) { return code == ABS_Z || code == ABS_RZ; }

const uint16_t kStandardButtons[] = {BTN_SOUTH,  BTN_EAST,  BTN_NORTH, BTN_WEST,   BTN_TL,
                                     BTN_TR,     BTN_TL2,   BTN_TR2,   BTN_SELECT, BTN_START,
                                     BTN_MODE,   BTN_THUMBL, BTN_THUMBR};

int inputSourceOf(const PadSpec& spec) {
    for (size_t i = 0; i < spec.sources.size(); ++i) {
        if (spec.sources[i].role != Role::Rumble) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

Layout computeLayout(const PadSpec& spec, const std::vector<std::optional<SourceCaps>>& caps) {
    Layout l;
    l.padAbs[ABS_X] = outRange(kStickMin, kStickMax);
    l.padAbs[ABS_Y] = outRange(kStickMin, kStickMax);
    bool hat = false;
    auto addTarget = [&](const Target& t) {
        switch (t.kind) {
            case TargetKind::PadKey: l.padKeys.insert(t.code); break;
            case TargetKind::KeysKey: l.keysKeys.insert(t.code); break;
            case TargetKind::Hat: hat = true; break;
        }
    };
    for (const ButtonMap& b : spec.buttons) addTarget(b.to);
    if (spec.passThrough) {
        int s = inputSourceOf(spec);
        const SourceCaps* c = (s >= 0 && s < static_cast<int>(caps.size()) && caps[s]) ? &*caps[s] : nullptr;
        if (c) {
            for (uint16_t k : c->keys) addTarget(targetFor(k));
            if (c->abs.count(ABS_HAT0X) || c->abs.count(ABS_HAT0Y)) hat = true;
        } else {
            for (uint16_t k : kStandardButtons) l.padKeys.insert(k);
            hat = true;
        }
    }
    if (hat) {
        l.padAbs[ABS_HAT0X] = outRange(kHatMin, kHatMax);
        l.padAbs[ABS_HAT0Y] = outRange(kHatMin, kHatMax);
    }
    for (const AxisMap& a : spec.axes) {
        l.padAbs[a.to] = a.trigger ? outRange(kTriggerMin, kTriggerMax) : outRange(kStickMin, kStickMax);
    }
    l.ff = spec.rumbleSource >= 0;
    return l;
}

Merger::Merger(const PadSpec& spec, const std::vector<std::optional<SourceCaps>>& caps)
    : spec_(spec), layout_(computeLayout(spec, caps)) {
    for (uint16_t k : layout_.padKeys) slots_.push_back({Dev::Pad, EV_KEY, k, 0});
    for (const auto& a : layout_.padAbs) {
        int32_t rest = isTriggerOut(a.first) ? kTriggerMin : 0;
        slots_.push_back({Dev::Pad, EV_ABS, a.first, rest});
    }
    for (uint16_t k : layout_.keysKeys) slots_.push_back({Dev::Keys, EV_KEY, k, 0});
    for (size_t i = 0; i < slots_.size(); ++i) slotIndex_[{slots_[i].type, slots_[i].code}] = i;

    for (const ButtonMap& b : spec_.buttons) buttonRoutes_[{b.source, b.code}].push_back(b.to);
    for (const AxisMap& a : spec_.axes) {
        Axis ax;
        ax.map = a;
        const AbsInfo* node = nullptr;
        if (a.source < static_cast<int>(caps.size()) && caps[a.source]) {
            auto it = caps[a.source]->abs.find(a.code);
            if (it != caps[a.source]->abs.end()) node = &it->second;
        }
        ax.range = resolveRange(a.cal, node, a.invert, a.trigger);
        axisRoutes_[{a.source, a.code}].push_back(axes_.size());
        axes_.push_back(ax);
    }
    if (spec_.passThrough) passSource_ = inputSourceOf(spec_);

    pending_.resize(spec_.sources.size());
    dropping_.assign(spec_.sources.size(), false);
    needsSync_.assign(spec_.sources.size(), false);
    shownLive_ = neutral();
    shownTap_ = neutral();
}

std::vector<int32_t> Merger::neutral() const {
    std::vector<int32_t> v;
    v.reserve(slots_.size());
    for (const Slot& s : slots_) v.push_back(s.neutral);
    return v;
}

std::vector<Target> Merger::routesOf(const Input& in) const {
    auto it = buttonRoutes_.find(in);
    if (it != buttonRoutes_.end()) return it->second;
    if (passSource_ >= 0 && in.first == passSource_) {
        Target t = targetFor(in.second);
        bool present = t.kind == TargetKind::Hat ? slotIndex_.count({EV_ABS, ABS_HAT0X}) > 0
                                                 : slotIndex_.count({EV_KEY, t.code}) > 0;
        if (present) return {t};
    }
    return {};
}

bool Merger::routed(const Input& in) const { return !routesOf(in).empty(); }

void Merger::apply(const SourceEvent& ev) {
    if (ev.type == EV_KEY) {
        Input in{ev.source, ev.code};
        if (!routed(in)) return;
        if (ev.value == 1) {
            held_.insert(in);
        } else if (ev.value == 0) {
            held_.erase(in);
            suppressed_.erase(in);
        }
        // 2 is autorepeat: the held state already says it.
    } else if (ev.type == EV_ABS) {
        auto it = axisRoutes_.find({ev.source, ev.code});
        if (it != axisRoutes_.end()) {
            for (size_t i : it->second) axes_[i].raw = ev.value;
        }
        if (ev.source == passSource_ && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) &&
            slotIndex_.count({EV_ABS, ev.code})) {
            passHat_[ev.code] = std::max(-1, std::min(1, ev.value));
        }
    }
}

std::vector<int32_t> Merger::desired() const {
    std::vector<int32_t> v = neutral();
    auto set = [&](uint16_t type, uint16_t code, int32_t value) {
        auto it = slotIndex_.find({type, code});
        if (it != slotIndex_.end()) v[it->second] = value;
    };
    bool up = false, down = false, left = false, right = false;
    for (const Input& in : held_) {
        if (suppressed_.count(in)) continue;
        for (const Target& t : routesOf(in)) {
            if (t.kind == TargetKind::Hat) {
                up |= t.code == BTN_DPAD_UP;
                down |= t.code == BTN_DPAD_DOWN;
                left |= t.code == BTN_DPAD_LEFT;
                right |= t.code == BTN_DPAD_RIGHT;
            } else {
                set(EV_KEY, t.code, 1);
            }
        }
    }
    int32_t hx = (right ? 1 : 0) - (left ? 1 : 0);
    int32_t hy = (down ? 1 : 0) - (up ? 1 : 0);
    if (hx == 0 && passHat_.count(ABS_HAT0X)) hx = passHat_.at(ABS_HAT0X);
    if (hy == 0 && passHat_.count(ABS_HAT0Y)) hy = passHat_.at(ABS_HAT0Y);
    set(EV_ABS, ABS_HAT0X, hx);
    set(EV_ABS, ABS_HAT0Y, hy);
    for (const Axis& a : axes_) {
        if (!a.raw) continue;
        set(EV_ABS, a.map.to, a.map.trigger ? scaleTrigger(a.range, *a.raw) : scaleStick(a.range, *a.raw));
    }
    return v;
}

void Merger::emitTo(const std::vector<int32_t>& target, bool tap, std::vector<OutEvent>* out) {
    std::vector<int32_t>& shown = tap ? shownTap_ : shownLive_;
    bool padChanged = false, keysChanged = false;
    // Pad slots come before keyboard slots, so one pass keeps each device's events together.
    for (Dev dev : {Dev::Pad, Dev::Keys}) {
        for (size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].dev != dev || shown[i] == target[i]) continue;
            shown[i] = target[i];
            out->push_back({tap ? Dev::Tap : dev, slots_[i].type, slots_[i].code, target[i]});
            (dev == Dev::Pad ? padChanged : keysChanged) = true;
        }
        if (!tap && dev == Dev::Pad && padChanged) out->push_back({Dev::Pad, EV_SYN, SYN_REPORT, 0});
        if (!tap && dev == Dev::Keys && keysChanged) out->push_back({Dev::Keys, EV_SYN, SYN_REPORT, 0});
    }
    if (tap && (padChanged || keysChanged)) out->push_back({Dev::Tap, EV_SYN, SYN_REPORT, 0});
}

void Merger::flush(std::vector<OutEvent>* out) { emitTo(desired(), intercept_, out); }

void Merger::suppressHeld() {
    for (const Input& in : held_) suppressed_.insert(in);
}

void Merger::process(const SourceEvent& ev, std::vector<OutEvent>* out) {
    if (ev.source < 0 || ev.source >= static_cast<int>(spec_.sources.size())) return;
    size_t s = static_cast<size_t>(ev.source);
    if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
        pending_[s].clear();
        dropping_[s] = true;
        return;
    }
    if (dropping_[s]) {
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            dropping_[s] = false;
            needsSync_[s] = true;
        }
        return;
    }
    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        for (const SourceEvent& e : pending_[s]) apply(e);
        pending_[s].clear();
        flush(out);
        return;
    }
    if (ev.type == EV_KEY || ev.type == EV_ABS) pending_[s].push_back(ev);
}

bool Merger::needsSync(int source) const {
    return source >= 0 && source < static_cast<int>(needsSync_.size()) && needsSync_[source];
}

void Merger::syncSource(int source, const std::set<uint16_t>& keysDown,
                        const std::map<uint16_t, int32_t>& abs, std::vector<OutEvent>* out) {
    if (source < 0 || source >= static_cast<int>(spec_.sources.size())) return;
    pending_[source].clear();
    dropping_[source] = false;
    needsSync_[source] = false;
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->first == source && !keysDown.count(it->second)) {
            suppressed_.erase(*it);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    for (uint16_t code : keysDown) {
        Input in{source, code};
        if (routed(in)) held_.insert(in);
    }
    for (const auto& a : abs) apply({source, EV_ABS, a.first, a.second, 0});
    flush(out);
}

void Merger::attachSource(int source, const SourceCaps& caps) {
    for (Axis& a : axes_) {
        if (a.map.source != source) continue;
        auto it = caps.abs.find(a.map.code);
        a.range = resolveRange(a.map.cal, it == caps.abs.end() ? nullptr : &it->second, a.map.invert,
                               a.map.trigger);
    }
}

void Merger::dropSource(int source, std::vector<OutEvent>* out) {
    if (source < 0 || source >= static_cast<int>(spec_.sources.size())) return;
    pending_[source].clear();
    dropping_[source] = false;
    needsSync_[source] = false;
    for (auto it = held_.begin(); it != held_.end();) {
        if (it->first == source) {
            suppressed_.erase(*it);
            it = held_.erase(it);
        } else {
            ++it;
        }
    }
    for (Axis& a : axes_) {
        if (a.map.source == source) a.raw.reset();
    }
    if (source == passSource_) passHat_.clear();
    flush(out);
}

void Merger::setIntercept(bool on, std::vector<OutEvent>* out) {
    if (on == intercept_) return;
    emitTo(neutral(), intercept_, out);  // the stream being left: all released, centred
    intercept_ = on;
    suppressHeld();
    flush(out);  // the stream taking over: axes where they are, held buttons not pressed
}

int32_t Merger::shown(uint16_t type, uint16_t code) const {
    auto it = slotIndex_.find({type, code});
    if (it == slotIndex_.end()) return INT32_MIN;
    return (intercept_ ? shownTap_ : shownLive_)[it->second];
}

}  // namespace padd
