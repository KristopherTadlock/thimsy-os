// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Force feedback on the virtual pad, turned into commands for the device's vibrator node
// (pwm-vibrator, pm8xxx-vibrator: memless FF_RUMBLE input devices named by pad.rumble.source).
// Pure: time is passed in.
//
// The pad advertises FF_RUMBLE and FF_GAIN. Every uploaded effect keeps its own timing (delay,
// length, repeat count, as the kernel's ff-memless plays them); the magnitudes of the effects
// active at a moment are summed per motor, clamped and scaled by the gain, and each change of the
// sum is one command. A command carries how long it may run unattended (until the soonest end of
// an active effect, 0 for "until the next command"), so the motor stops by itself if padd dies.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace padd {

struct RumbleEffect {
    int id = -1;
    uint16_t strong = 0;
    uint16_t weak = 0;
    uint32_t lengthMs = 0;  // 0: until stopped
    uint32_t delayMs = 0;
};

struct VibCommand {
    bool on = false;
    uint16_t strong = 0;
    uint16_t weak = 0;
    uint32_t durationMs = 0;  // when on; 0: until the next command
    bool operator==(const VibCommand& o) const {
        return on == o.on && strong == o.strong && weak == o.weak && durationMs == o.durationMs;
    }
};

class Rumble {
 public:
    explicit Rumble(int maxEffects = 16) : maxEffects_(maxEffects) {}

    // A new effect or an update of one (same id); an update keeps it playing. 0, or -EINVAL for
    // an id outside 0..maxEffects-1.
    int upload(const RumbleEffect& e, int64_t nowNs, std::vector<VibCommand>* out);
    // Stops and forgets the effect. 0, or -EINVAL for an unknown id.
    int erase(int id, int64_t nowNs, std::vector<VibCommand>* out);
    // count > 0 plays the effect count times; 0 stops it. Unknown ids are ignored.
    void play(int id, int count, int64_t nowNs, std::vector<VibCommand>* out);
    // FF_GAIN: 0..0xffff, applied to every effect.
    void setGain(uint16_t gain, int64_t nowNs, std::vector<VibCommand>* out);
    // Moves time on: ends and starts effects whose time has come.
    void tick(int64_t nowNs, std::vector<VibCommand>* out);
    // The next time the output can change by itself; -1 when nothing is pending.
    int64_t nextDeadlineNs() const;
    // Stops everything (intercept, shutdown); effects stay uploaded.
    void stopAll(int64_t nowNs, std::vector<VibCommand>* out);

    int maxEffects() const { return maxEffects_; }

 private:
    struct Slot {
        RumbleEffect effect;
        bool playing = false;
        int64_t startNs = 0;
        int count = 0;
    };
    // Whether slot s is active at t, and when its current state ends (-1: never).
    bool activeAt(const Slot& s, int64_t t, int64_t* changeNs) const;
    void update(int64_t nowNs, std::vector<VibCommand>* out);

    int maxEffects_;
    std::map<int, Slot> slots_;
    uint16_t gain_ = 0xffff;
    int64_t now_ = 0;
    VibCommand last_;
    int64_t lastExpiryNs_ = -1;  // when last_ runs out by itself; -1: never
};

}  // namespace padd
