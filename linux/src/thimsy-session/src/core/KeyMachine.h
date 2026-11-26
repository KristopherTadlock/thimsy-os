// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The system keys (IMPLEMENTATION F.1, plan 5.2): the power key, the volume keys, the lid switch and
// the quit hotkey. Pure: events and "now" come in, actions go out; time is the events' own
// CLOCK_MONOTONIC stamps and the loop's clock.
//
//   power  short press (released before longPressMs): sleep.
//          long press (held for longPressMs): power off when Nano is not running (FlipBase's
//          pwrkeyd idea), else the long press goes to Nano (its power menu). The action fires when
//          the time is reached, while the key is still held; the release then does nothing.
//          After a resume, power events stamped within resumeIgnoreMs of it are dropped, and a
//          release whose press was not seen is ignored: the press that woke the device must not
//          put it back to sleep.
//   volume a press or autorepeat of KEY_VOLUMEUP/DOWN is one step.
//   lid    SW_LID closing is a sleep; opening does nothing (the kernel wakes the device).
//   quit   every button of the combo (device.json pad.hotkey.quit_combo, default BTN_MODE +
//          BTN_START) held on the Thimsy Gamepad while a game runs: quit the game. It fires once
//          until one of the combo's buttons is released.
#pragma once

#include <set>
#include <vector>

#include "core/InputCodes.h"
#include "core/Types.h"

namespace thimsy {
namespace session {

struct KeyEvent {
    enum class Source { Keys, Pad };
    Source source = Source::Keys;
    int type = 0;
    int code = 0;
    int value = 0;  // 0 release, 1 press, 2 autorepeat; for EV_SW 1 closed, 0 open
    Ms time = 0;
};

enum class KeyAction {
    Sleep,
    PowerOff,
    PowerLongPress,  // Nano is running: a long press for its power menu
    VolumeUp,
    VolumeDown,
    QuitGame,
};

const char* toString(KeyAction a);

struct KeyConfig {
    Ms longPressMs = 3000;
    Ms resumeIgnoreMs = 1000;
    std::vector<InputCode> quitCombo;  // empty: BTN_MODE + BTN_START
};

struct KeyContext {
    bool nanoRunning = false;
    bool gameRunning = false;
};

class KeyMachine {
 public:
    explicit KeyMachine(KeyConfig config = {});

    std::vector<KeyAction> onEvent(const KeyEvent& ev, const KeyContext& ctx);
    std::vector<KeyAction> onTick(Ms now, const KeyContext& ctx);
    Ms nextDeadline() const;

    // The device has just resumed at `now` (the write to /sys/power/state returned).
    void noteResumed(Ms now);

    bool lidClosed() const { return lidClosed_; }
    bool powerHeld() const { return powerDown_; }

 private:
    KeyAction longAction(const KeyContext& ctx) const;

    KeyConfig config_;
    bool powerDown_ = false;
    bool longFired_ = false;
    Ms pressAt_ = 0;
    Ms resumedAt_ = -1;
    bool lidClosed_ = false;
    std::set<int> padHeld_;
    bool comboLatched_ = false;
};

}  // namespace session
}  // namespace thimsy
