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

// The host's nanoplat::Power and nanoplat::SystemActions (docs/linux/IMPLEMENTATION.md B.6, D.3,
// G7-power): fakes that record what Nano asked and change nothing.
//
// FakePower: readBattery reports the battery set with setFakeBattery (none by default, so Nano's
// values stay unknown, as with no health source); setBrightnessViaHal returns false (no HAL);
// savedBrightness returns -1 (nothing recorded: Nano boots on persist.gammaos.nano.brightness).
// Each mirrorBrightness and powerAction is also a {"ev":"power"} event: "op" is "mirror" (with
// "level") or "action" (with "action"), so a scenario can assert that a menu's Power Off asked for
// "shutdown".
//
// FakeActions: each call is one line of actionsRecord() and one {"ev":"action","call":...} event:
// "wifi on", "wifi off", "forget <id>", "bt on", "bt off", "bt connect <mac>", "time 12h",
// "time 24h", "zone <id>".
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

struct PowerRecord {
    int batteryReads = 0;
    int halCalls = 0, halLevel = -1;     // setBrightnessViaHal: calls and the last level
    int mirrors = 0, mirrorLevel = -1;   // mirrorBrightness: calls and the last level
    int savedReads = 0;
    std::vector<std::string> actions;    // powerAction's actions, in order
};

// The battery the fake power reports from now on; percent < 0: none.
void setFakeBattery(int percent, bool charging);

std::unique_ptr<nanoplat::Power> createFakePower();
std::unique_ptr<nanoplat::SystemActions> createFakeActions();

// What the most recently made fake has recorded (empty before one is made).
PowerRecord powerRecord();
std::vector<std::string> actionsRecord();

}  // namespace nanohost
