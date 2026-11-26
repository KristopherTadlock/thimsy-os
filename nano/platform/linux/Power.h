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

// The Linux platform's nanoplat::Power (docs/linux/IMPLEMENTATION.md B.6, G7-power):
//
// - readBattery: /sys/class/power_supply. The first supply (by name) of type "Battery" that is
//   present and gives a charge: `capacity`, else charge_now/charge_full, else energy_now/energy_full,
//   clamped to 0-100. Charging when its `status` is "Charging" or "Full"; with no status, when a
//   supply of another type (Mains, USB, ...) is online. Nothing found: the values stay as they were,
//   as the firmware's do when no health source answers.
// - setBrightnessViaHal: false. Linux has no lights HAL; NanoBacklight.h, which Nano calls first,
//   already drove every /sys/class/backlight node.
// - mirrorBrightness: nothing. The level's record is persist.gammaos.nano.brightness, which
//   syncBrightnessToAndroid wrote just before (Android keeps a second copy in Settings.System).
// - savedBrightness: that record, persist.gammaos.nano.brightness, when it is a level (1-255), else
//   -1; so on Linux the brightness Nano boots with is the one it last set, as on Android.
// - powerAction: a thimsy-session request (linux/docs/session.md): "reboot" -> reboot, "shutdown"
//   -> poweroff, through controlkeys::sessionRequest, the sequence every request of Nano's shares.
//   "recovery", "safemode" and "android" have no Linux meaning: logged and ignored.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "NanoPlatform.h"

namespace nanolinux {
namespace power {

struct BatteryReading {
    bool found = false;
    int percent = -1;
    bool charging = false;
    std::string supply;  // the power_supply entry read ("battery", "BAT0", ...)
};

// The battery under `root` + /sys/class/power_supply ("" on the device; a fake tree in tests).
BatteryReading readBattery(const std::string& root);

// The thimsy-session verb for prepareShutdown's `action`, or "" when Linux has none.
std::string sessionVerbFor(const std::string& action);

// persist.gammaos.nano.brightness's text as a level: 1-255, else -1.
int parseLevel(const std::string& text);

struct Options {
    std::string root;  // prefix for /sys ("" on the device)
    // Writes one thimsy-session request; empty: controlkeys::sessionRequest.
    std::function<void(const std::string& verb)> sessionRequest;
};

std::unique_ptr<nanoplat::Power> create(Options options = Options());

}  // namespace power
}  // namespace nanolinux
