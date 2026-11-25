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

// A monitor's output mode as the framework reports it (DisplayManagerService.gammaos*MonitorMode):
// sys.gammaos.display.monitor_mode ("WxH@HZ;auto|user|trial", empty with no monitor) and the
// output of `cmd display gammaos-monitor-mode list` and `try`. What Settings > Display Settings >
// External Display offers under Output Resolution and Refresh Rate (NanoMenuMonitorMode.cpp).
// Pure, so host tests can use it.
#pragma once

#include <string>
#include <vector>

namespace android {
namespace monmode {

// The rate below which a mode is not offered in Dual Screen: the monitor paces the built-in
// screen's composition there (GammaosScreenArrangement.offered does the same).
constexpr float kDualMinHz = 50.0f;

// A mode, WxH@HZ. The rate stays as the framework wrote it ("60", "59.94"), so `try` gets the
// same number back.
struct Mode {
    int width = 0;
    int height = 0;
    std::string rate;

    bool valid() const { return width > 0 && height > 0 && !rate.empty(); }
    float hz() const;
    std::string text() const;   // WxH@HZ, "" when not valid
};

// The mode a WxH@HZ value names; false (and out untouched) for any other value. Only digits and one
// dot are accepted in the rate, so a parsed mode can go on a command line as it is.
bool parseMode(const std::string& value, Mode* out);

bool sameSize(const Mode& a, const Mode& b);
bool sameMode(const Mode& a, const Mode& b);   // the same size, the rates within 0.5 Hz

// sys.gammaos.display.monitor_mode: the mode the monitor is held at and why ("auto", "user" or
// "trial"). False with no monitor (an empty or garbled value).
bool parseProp(const std::string& value, Mode* held, std::string* why);

// `gammaos-monitor-mode list`.
struct List {
    Mode held;
    std::string why;            // auto, user or trial
    Mode active;                // invalid when the framework has none
    Mode autoMode;              // what Auto runs the monitor at
    Mode stored;                // invalid when none is stored
    std::vector<Mode> modes;    // the modes offered, largest first, then higher rate
};
// False when the text has no held mode (no monitor, or an error).
bool parseList(const std::string& text, List* out);

// `gammaos-monitor-mode try W H HZ` once the mode is active: "trial MODE SECONDS REVERT_MODE".
struct Trial {
    Mode mode;
    int seconds = 0;   // until the framework reverts it
    Mode revert;       // what it goes back to
};
bool parseTrial(const std::string& text, Trial* out);

// The modes a chooser may offer: all of them, except those below kDualMinHz in Dual Screen.
std::vector<Mode> offered(const std::vector<Mode>& modes, bool dualScreen);

// Output Resolution's choices: one mode per size, in list order (largest first). Each has the
// current mode's rate where its size has that rate, else the rate nearest 60 Hz (the higher one
// of two as near).
std::vector<Mode> sizeChoices(const std::vector<Mode>& modes, const Mode& current);

// Refresh Rate's choices: the modes at the current mode's size, in list order (higher rate first).
std::vector<Mode> rateChoices(const std::vector<Mode>& modes, const Mode& current);

// The index in choices of the mode matching current (sameMode, or with sizesOnly sameSize), -1 when
// none does.
int indexOf(const std::vector<Mode>& choices, const Mode& current, bool sizesOnly);

std::string sizeText(const Mode& m);   // "2560 x 1440"

}  // namespace monmode
}  // namespace android
