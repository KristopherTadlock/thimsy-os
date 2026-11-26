/*
 * Copyright (C) 2026 GammaOS
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

// commitLaunch() (NanoLaunchCommit.h): the launch handshake's writes, in the call site's order.
// The LOG_TAG is the call sites' (NanoMenu.cpp and the files split from it), for NanoMenuUtils.h's
// inline helpers; commitLaunch itself logs nothing.

#define LOG_TAG "GammaOSNano"

#include <cutils/properties.h>

#include "NanoLaunchCommit.h"
#include "NanoMenuUtils.h"   // setLaunchRomPath, setLaunchGame

namespace android {

const char* launchKeyProperty(LaunchKey key) {
    switch (key) {
        case LaunchKey::App: return "sys.gammaos.nano.launch_app";
        case LaunchKey::LaunchedPkg: return "sys.gammaos.nano.launched_pkg";
        case LaunchKey::Intent: return "sys.gammaos.nano.launch_intent";
        case LaunchKey::Core: return "sys.gammaos.nano.launch_core";
        case LaunchKey::Rom: return "sys.gammaos.nano.launch_rom";
        case LaunchKey::RomPath: return nullptr;
        case LaunchKey::Game: return nullptr;
        case LaunchKey::ReturnRecent: return "sys.gammaos.nano.return_recent";
        case LaunchKey::DropInput: return "sys.gammaos.nano.drop_input";
        case LaunchKey::HandoffFired: return "sys.gammaos.nano.handoff_fired";
        case LaunchKey::PendingExit: return "sys.gammaos.nano.pending_exit";
        case LaunchKey::DoLaunch: return "sys.gammaos.nano.do_launch";
        case LaunchKey::Retroarch: return "service.bootanim.nano_retroarch";
    }
    return nullptr;
}

void commitLaunch(const LaunchTuple& tuple) {
    for (const LaunchTuple::Write& w : tuple.writes) {
        switch (w.key) {
            case LaunchKey::RomPath:
                setLaunchRomPath(w.value);
                break;
            case LaunchKey::Game:
                setLaunchGame(w.value, w.rom, w.core);
                break;
            default:
                property_set(launchKeyProperty(w.key), w.value.c_str());
                break;
        }
    }
}

}  // namespace android
