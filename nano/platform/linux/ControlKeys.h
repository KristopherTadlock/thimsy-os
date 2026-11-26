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

// Control keys on Linux (docs/linux/IMPLEMENTATION.md B.2, linux/docs/session.md): some of Nano's
// property writes are messages to Android's init or framework (gammaos-nano.rc's `on property:`
// triggers, PhoneWindowManager's listeners). The store keeps every write (it never lets a handler
// swallow one); these handlers observe them through nanocompat::addPropertyObserver and do the
// Linux equivalent:
//   - a thimsy-session request (sys.thimsy.session.request: dosleep -> sleep, park_restart ->
//     restart-nano, service.bootanim.nano_action=reboot|shutdown -> reboot|poweroff, the end of
//     NanoMenu::prepareShutdown);
//   - the launch handshake (do_launch, or service.bootanim.nano_retroarch) -> the launcher;
//   - fixedperf -> the family hook (sh /usr/share/thimsy/family/<family>/hook fixedperf-on|off);
//   - drm_active=1 (Nano's first frame) -> sys.thimsy.nano.first_frame and sys.boot_completed;
//   - Android-only messages (ctl.*, the overlay, the OTA flasher, drastic-nano) -> logged once.
// The rc's own resets (setprop sys.gammaos.nano.dosleep 0) are mirrored. Keys the session reads
// itself (sys.powerctl, scrape_run) and plain state need no handler.
//
// The table is kControlRows in ControlKeys.cpp; tools/nano-tree/control-keys.json gives every
// trigger and listener a disposition, and tools/nano-tree/tests/test_control_keys.py checks that the
// two agree and that every rc trigger has a row.
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace nanolinux {
namespace controlkeys {

enum class Action {
    Session,     // write arg as a thimsy-session request
    Launch,      // the launch handshake, as one unit
    Hook,        // run the family hook with arg as its action
    FirstFrame,  // Nano's first frame is on screen
    Reset,       // only the rc's reset
    Refuse,      // Android-only: logged once
};

struct Row {
    const char* key;
    const char* value;   // the value that triggers ("" or "*": any value not given its own row)
    Action action;
    const char* arg;     // Session: the verb; Hook: the hook's action; else ""
    const char* reset;   // written back first, as the rc's setprop does ("" for none)
};

extern const Row kControlRows[];
extern const size_t kControlRowCount;

// The launch handshake's keys, read when the launch fires.
struct LaunchRequest {
    std::map<std::string, std::string> keys;   // sys.gammaos.nano.launch_app and the rest
    std::string trigger;                        // the key that fired it
};

struct Handlers {
    // Defaults: the session request key with a sequence number; a log line (main_linux.cpp gives
    // the Linux launcher's, launcher::launchFromHandshake in Launcher.h, through setLaunchHandler);
    // the family hook, run in the background.
    std::function<void(const std::string& verb)> sessionRequest;
    std::function<void(const LaunchRequest&)> launch;
    std::function<void(const std::string& action)> runHook;
    std::function<long long()> nowMs;           // CLOCK_MONOTONIC in ms by default
};

// Registers the observers. Call once, after nanocompat::init and before any Nano thread starts.
// Calling it again replaces the handlers (tests).
void install(Handlers handlers = Handlers());
void uninstall();

// Replaces the launch handler (nullptr: the default's log line). thimsy-nano's main registers the
// observers before the platform exists and gives the launcher's handler once the platform is
// installed, so no launch reaches nanoplat::get() before install() (G7-launch-b).
void setLaunchHandler(std::function<void(const LaunchRequest&)> launch);

// The request the default sessionRequest writes: "<seq>:<verb>", seq one past the number in the
// key's current value (Nano is its only writer; the session ignores a repeated seq).
std::string nextRequest(const std::string& current, const std::string& verb);

// The keys refused so far (each logged once), for tests.
std::size_t refusedCount();

}  // namespace controlkeys
}  // namespace nanolinux
