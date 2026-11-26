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

// The host's nanoplat::Launcher (docs/linux/IMPLEMENTATION.md B.6, D.3): the Linux launcher's
// behaviour with nothing started.
//
// - commit writes the handshake as the firmware and Linux do (the store keeps the keys; the host
//   platform's {"ev":"prop"} events show every write in order).
// - A write of the trigger (do_launch=1, service.bootanim.nano_retroarch=1) in a commit is a launch,
//   as the control keys make it on Linux: the request is read from the store's handshake keys at
//   that write (with the full value commit() wrote to a key wherever the store kept an older one:
//   it refuses a value too long for a property), and a second trigger with the same request within
//   3 s is the same launch.
// - launch() records the request and tells the runner with a {"ev":"launch"} event ("kind" Core or
//   Package, "package", "core", "rom", "system", "intent", "trigger"), which the scenarios'
//   expect_launch reads (tests/e2e/nano_scenario.py). The display is not handed over: Nano's launch
//   sites still exit after a launch, and the runner starts Nano again (child_exit).
// - The "child" ends by itself after a scripted delay (pollChild), or on requestChildQuit (SIGTERM).
//   When pollChild reports its end, launch_app, launch_intent, launch_core and launch_rom are
//   cleared, as Android's framework and the Linux launcher do at a launch's end.
#pragma once

#include <memory>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

struct FakeLauncherOptions {
    bool exitsOnLaunch = false;   // nanoExitsOnLaunch(): as Linux's
    int childExitAfterMs = 0;     // pollChild reports the child's end this long after the launch
    int childExitStatus = 0;
};

std::unique_ptr<nanoplat::Launcher> createFakeLauncher(const FakeLauncherOptions& options = FakeLauncherOptions());

// What the most recently made fake launcher has done, for tests.
struct LaunchRecord {
    std::vector<nanoplat::LaunchRequest> requests;   // every launch, in order
    int quits = 0;                                    // requestChildQuit calls with a child running
};
LaunchRecord launchRecord();

}  // namespace nanohost
