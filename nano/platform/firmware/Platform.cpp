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

// The firmware platform's createPlatform() (docs/linux/IMPLEMENTATION.md B.6, B.7 P4): what
// gammaos-nano's main.cpp installs. Each service is today's Android code, moved into
// platform/firmware/<Area>.cpp by the G7 step of its area, which wires it in below.

#include <memory>

#include "NanoPlatform.h"
#include "Display.h"  // G7-display
#include "Launcher.h"  // G7-launch-b
#include "Volumes.h"
#include "Power.h"  // G7-power
#include "SystemActions.h"  // G7-power
#include "InputPolicy.h"  // G7-input
#include "Network.h"  // G7-net
#include "Boot.h"  // W6-boot

namespace nanofirmware {
std::unique_ptr<nanoplat::Audio> makeAudio();  // platform/firmware/Audio.cpp (G7-audio)
}  // namespace nanofirmware

std::unique_ptr<nanoplat::Platform> nanoplat::createPlatform(const PlatformArgs& /*args*/) {
    std::unique_ptr<Platform> platform(new Platform);
    // The services, one line each (G7 steps).
    platform->display = nanofirmware::createDisplay();  // G7-display
    platform->launcher = nanofirmware::createLauncher();  // G7-launch-b
    platform->audio = nanofirmware::makeAudio();
    platform->volumes = firmware::makeVolumes();
    platform->power = nanofirmware::createPower();  // G7-power
    platform->actions = nanofirmware::createSystemActions();  // G7-power
    platform->input = nanofirmware::createInputPolicy();  // G7-input
    platform->net = nanofirmware::createNetwork();  // G7-net
    platform->boot = nanofirmware::createBoot();  // W6-boot
    return platform;
}
