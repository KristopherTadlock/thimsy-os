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

// The Linux platform's createPlatform() (docs/linux/IMPLEMENTATION.md B.6, G7-main): what
// main_linux.cpp installs, after its own set-up (the log file, the property store, the control
// keys). Each service is wired in here by the G7 step of its area, from platform/linux/<Area>*.cpp.

#include <memory>

#include "NanoPlatform.h"
#include "Launcher.h"  // G7-launch-b
#include "display/Display.h"  // G7-display
#include "AudioPull.h"  // G7-audio
#include "Volumes.h"
#include "Power.h"  // G7-power
#include "actions/SystemActions.h"  // G7-power
#include "InputPolicy.h"  // G7-input
#include "actions/Network.h"  // G7-net
#include "Emulators.h"  // W8-EMU

std::unique_ptr<nanoplat::Platform> nanoplat::createPlatform(const PlatformArgs& /*args*/) {
    std::unique_ptr<Platform> platform(new Platform);
    // The services, one line each (G7 steps).
    platform->display = nanolinux::display::create(nanolinux::display::createKms());  // G7-display
    platform->launcher = nanolinux::launcher::create(platform->display.get());  // G7-launch-b
    platform->audio = nanolinux::makeAudio();
    platform->volumes = nanolinux::makeVolumes();   // /run/thimsy/volumes.json
    platform->power = nanolinux::power::create();  // G7-power
    platform->actions = nanolinux::actions::create();  // G7-power
    platform->input = nanolinux::input::create();  // G7-input
    platform->net = nanolinux::net::create();  // G7-net: wpa_supplicant and bluetoothctl
    platform->emulators = nanolinux::makeEmulators();  // W8-EMU: the image's cores; no apps
    return platform;
}
