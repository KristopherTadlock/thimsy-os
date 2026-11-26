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

// The host's nanoplat::InputPolicy (docs/linux/IMPLEMENTATION.md B.6, D.3). It grabs as the
// firmware's does (EVIOCGRAB on each device Nano opens when persist.gammaos.nano.grab_input is set,
// released before Nano closes it), so the fake devices (FakeEvdev.h, through the wrapped ioctl) see
// the same grabs and the scenarios' {"ev":"grab"} events stay what they were before the move. There
// is no padd on the host: silenceGamePad only records, and tells the runner with
// {"ev":"input","op":"silence","on":true|false}.
#pragma once

#include <memory>

#include "NanoPlatform.h"

namespace nanohost {

// What the fake input policy has done, for tests.
struct InputRecord {
    int opened = 0, closing = 0, grabs = 0;  // grabs: EVIOCGRAB(1) calls made by deviceOpened
    int silenceCalls = 0;
    bool silenced = false;
};

std::unique_ptr<nanoplat::InputPolicy> createFakeInput();

// The record of the most recently made fake input policy (zeros before one is made).
InputRecord inputRecord();

}  // namespace nanohost
