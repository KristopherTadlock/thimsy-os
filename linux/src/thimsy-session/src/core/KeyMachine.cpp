// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/KeyMachine.h"

#include <linux/input-event-codes.h>

namespace thimsy {
namespace session {

const char* toString(KeyAction a) {
    switch (a) {
        case KeyAction::Sleep: return "sleep";
        case KeyAction::PowerOff: return "poweroff";
        case KeyAction::PowerLongPress: return "power-long-press";
        case KeyAction::VolumeUp: return "volume-up";
        case KeyAction::VolumeDown: return "volume-down";
        case KeyAction::QuitGame: return "quit-game";
    }
    return "?";
}

KeyMachine::KeyMachine(KeyConfig config) : config_(std::move(config)) {
    if (config_.quitCombo.empty()) {
        config_.quitCombo = {{EV_KEY, BTN_MODE}, {EV_KEY, BTN_START}};
    }
}

KeyAction KeyMachine::longAction(const KeyContext& ctx) const {
    return ctx.nanoRunning ? KeyAction::PowerLongPress : KeyAction::PowerOff;
}

std::vector<KeyAction> KeyMachine::onEvent(const KeyEvent& ev, const KeyContext& ctx) {
    std::vector<KeyAction> out;
    if (ev.source == KeyEvent::Source::Pad) {
        if (ev.type != EV_KEY) return out;
        bool inCombo = false;
        for (const InputCode& c : config_.quitCombo) {
            if (c.type == EV_KEY && c.code == ev.code) inCombo = true;
        }
        if (!inCombo) return out;
        if (ev.value == 0) {
            padHeld_.erase(ev.code);
            comboLatched_ = false;
            return out;
        }
        if (ev.value != 1) return out;
        padHeld_.insert(ev.code);
        bool all = true;
        for (const InputCode& c : config_.quitCombo) {
            if (!padHeld_.count(c.code)) all = false;
        }
        if (all && !comboLatched_) {
            comboLatched_ = true;
            if (ctx.gameRunning) out.push_back(KeyAction::QuitGame);
        }
        return out;
    }

    if (ev.type == EV_SW && ev.code == SW_LID) {
        bool closed = ev.value != 0;
        if (closed && !lidClosed_) out.push_back(KeyAction::Sleep);
        lidClosed_ = closed;
        return out;
    }
    if (ev.type != EV_KEY) return out;

    switch (ev.code) {
        case KEY_POWER:
            if (ev.value == 1) {
                if (resumedAt_ >= 0 && ev.time <= resumedAt_ + config_.resumeIgnoreMs) {
                    powerDown_ = false;  // the wake press: its release is ignored too
                    return out;
                }
                powerDown_ = true;
                longFired_ = false;
                pressAt_ = ev.time;
            } else if (ev.value == 0) {
                if (!powerDown_) return out;  // a release whose press was not seen
                powerDown_ = false;
                if (longFired_) return out;
                if (ev.time - pressAt_ >= config_.longPressMs) {
                    // The loop did not reach the deadline before the release arrived (a busy loop,
                    // or stamped events replayed at once): the press still was a long one.
                    longFired_ = true;
                    out.push_back(longAction(ctx));
                } else {
                    out.push_back(KeyAction::Sleep);
                }
            }
            return out;
        case KEY_VOLUMEUP:
            if (ev.value == 1 || ev.value == 2) out.push_back(KeyAction::VolumeUp);
            return out;
        case KEY_VOLUMEDOWN:
            if (ev.value == 1 || ev.value == 2) out.push_back(KeyAction::VolumeDown);
            return out;
        default:
            return out;
    }
}

std::vector<KeyAction> KeyMachine::onTick(Ms now, const KeyContext& ctx) {
    std::vector<KeyAction> out;
    if (powerDown_ && !longFired_ && now >= pressAt_ + config_.longPressMs) {
        longFired_ = true;
        out.push_back(longAction(ctx));
    }
    return out;
}

Ms KeyMachine::nextDeadline() const {
    if (powerDown_ && !longFired_) return pressAt_ + config_.longPressMs;
    return kNever;
}

void KeyMachine::noteResumed(Ms now) {
    resumedAt_ = now;
    powerDown_ = false;
    longFired_ = false;
}

}  // namespace session
}  // namespace thimsy
