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

// The host's nanoplat::Boot (NanoPlatform.h; W6-boot). nanoOwnsBootScreen() answers the config's
// "nano_owns_boot_screen" (HostConfig.h; true by default) at once, reading no property and no file.
// When it is false, handOverBootScreen() plays the firmware's contract: it tells the runner with
// {"ev":"boot","op":"hand_over","overlay":true|false}, closes the fd it is given and ends the process
// with status 0. Nothing waits: the store is opened before Nano starts (HostPlatform.cpp), so the
// settings are loaded, and the fake audio needs no service. Every call is recorded.
#pragma once

#include <memory>

#include "NanoPlatform.h"

namespace nanohost {

// What the fake boot has been asked, for tests.
struct BootRecord {
    int ownerChecks = 0;       // nanoOwnsBootScreen()
    int handOvers = 0;         // handOverBootScreen() (in the process that made the call)
    int quickResumeWaits = 0;  // awaitSavedSettings(QuickResumeCheck)
    int menuSetupWaits = 0;    // awaitSavedSettings(MenuSetup)
    int loadedChecks = 0;      // savedSettingsLoaded()
    int soundChecks = 0;       // soundSystemReady()
};

std::unique_ptr<nanoplat::Boot> createFakeBoot(bool nanoOwnsBootScreen);

// The record of the most recently made fake boot (zeros before one is made).
BootRecord bootRecord();

}  // namespace nanohost
