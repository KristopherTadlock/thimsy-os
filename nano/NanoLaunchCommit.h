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

// The launch handshake's writes, gathered (docs/linux/IMPLEMENTATION.md B.2, B.6's Launch row):
// every place that writes the handshake's keys builds a LaunchTuple of its writes, in the order it
// has always written them, and hands it to commitLaunch(), which writes them in that order. The
// order is the call site's, never normalised: the sites write different subsets in different
// orders, and the framework (and on Linux the control keys' observers) sees every set.
//
//   commitLaunch(LaunchTuple().app(pkg).launchedPkg(pkg).romPath("").core(""));
//
// A tuple only holds handshake writes; anything else a site does between two of them (a file
// write, another key, a read of a key just written) stays at the site, between two commits.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace android {

enum class LaunchKey {
    App,           // sys.gammaos.nano.launch_app
    LaunchedPkg,   // sys.gammaos.nano.launched_pkg
    Intent,        // sys.gammaos.nano.launch_intent
    Core,          // sys.gammaos.nano.launch_core
    Rom,           // sys.gammaos.nano.launch_rom, the property alone
    RomPath,       // setLaunchRomPath(value) (NanoMenuUtils.h): launch_rom, its file, the game reset
    Game,          // setLaunchGame(value, rom, core) (NanoMenuUtils.h): the button map's record
    ReturnRecent,  // sys.gammaos.nano.return_recent
    DropInput,     // sys.gammaos.nano.drop_input
    HandoffFired,  // sys.gammaos.nano.handoff_fired
    PendingExit,   // sys.gammaos.nano.pending_exit
    DoLaunch,      // sys.gammaos.nano.do_launch
    Retroarch,     // service.bootanim.nano_retroarch
};

// The property a key writes; nullptr for RomPath and Game, which are calls.
const char* launchKeyProperty(LaunchKey key);

struct LaunchTuple {
    struct Write {
        LaunchKey key;
        std::string value;       // the property's value; RomPath: the ROM path; Game: the system id
        std::string rom, core;   // Game only
    };
    std::vector<Write> writes;   // in the call site's order

    LaunchTuple& app(std::string v) { return put(LaunchKey::App, std::move(v)); }
    LaunchTuple& launchedPkg(std::string v) { return put(LaunchKey::LaunchedPkg, std::move(v)); }
    LaunchTuple& intent(std::string v) { return put(LaunchKey::Intent, std::move(v)); }
    LaunchTuple& core(std::string v) { return put(LaunchKey::Core, std::move(v)); }
    LaunchTuple& rom(std::string v) { return put(LaunchKey::Rom, std::move(v)); }
    LaunchTuple& romPath(std::string v) { return put(LaunchKey::RomPath, std::move(v)); }
    LaunchTuple& game(std::string systemId, std::string romPath, std::string corePath = std::string()) {
        writes.push_back(Write{LaunchKey::Game, std::move(systemId), std::move(romPath), std::move(corePath)});
        return *this;
    }
    LaunchTuple& returnRecent(std::string v) { return put(LaunchKey::ReturnRecent, std::move(v)); }
    LaunchTuple& dropInput(std::string v) { return put(LaunchKey::DropInput, std::move(v)); }
    LaunchTuple& handoffFired(std::string v) { return put(LaunchKey::HandoffFired, std::move(v)); }
    LaunchTuple& pendingExit(std::string v) { return put(LaunchKey::PendingExit, std::move(v)); }
    LaunchTuple& doLaunch(std::string v) { return put(LaunchKey::DoLaunch, std::move(v)); }
    LaunchTuple& retroarch(std::string v) { return put(LaunchKey::Retroarch, std::move(v)); }

 private:
    LaunchTuple& put(LaunchKey key, std::string v) {
        writes.push_back(Write{key, std::move(v), std::string(), std::string()});
        return *this;
    }
};

// Writes the tuple's writes in order, on the calling thread: each key with property_set, RomPath
// and Game through setLaunchRomPath and setLaunchGame. Nothing is skipped, merged or reordered,
// and an empty value is written as the empty string, as the call sites did.
void commitLaunch(const LaunchTuple& tuple);

}  // namespace android
