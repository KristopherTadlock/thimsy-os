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

// The Linux platform's nanoplat::Launcher (docs/linux/IMPLEMENTATION.md B.6's Launch row, G7-launch-b;
// linux_handheld_plan.md 5.3 and 6.4).
//
// - commit: the handshake's writes, as the firmware makes them (the property store keeps every key,
//   which Nano reads back as state, and the control keys observe them).
// - The trigger: the control keys (ControlKeys.h) turn do_launch=1 or service.bootanim.nano_retroarch=1
//   into one request, debounced; main_linux.cpp gives them launchFromHandshake() as Handlers.launch,
//   which reads the request (requestFromHandshake) and calls the platform's launch().
// - launch: the argv (argvFor), then Display::releaseForChild, then fork; the child calls setsid (its
//   own session and process group), puts the signals back to their defaults, closes every descriptor
//   but 0, 1 and 2, and execve's the argv with Nano's environment. A close-on-exec pipe tells the
//   parent whether execve worked; when it did not, or fork failed, the child is reaped, the panel is
//   taken back (Display::reacquire) and launch() returns false.
// - pollChild: waitpid(WNOHANG) on the child. requestChildQuit: SIGTERM to its process group.
//
// Nano's launch sites still exit after a launch (nanoExitsOnLaunch() is false, but nothing reads it
// yet), so main_linux.cpp waits for the child (waitForChild) before Nano exits and the session starts
// it again; the panel goes back to Nano by that restart, not by reacquire().
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ControlKeys.h"
#include "NanoPlatform.h"

namespace nanolinux {
namespace launcher {

// Where the image keeps what a launch runs. TODO(plan 6.4): the emulator definitions' Linux block
// (a player's emulator and core from the image's list, its launch template's argv and environment)
// and the per-launch append config; Nano has neither yet, so a core launch runs RetroArch on the
// matching Linux core and a package launch only knows RetroArch's own package.
struct Options {
    std::string retroarch = "/usr/bin/retroarch";
    std::string coreDir = "/usr/lib/libretro";                                 // <name>_libretro.so
    std::string retroarchConfig = "/storage/.config/retroarch/retroarch.cfg";  // plan 6.2's main config
};

// RetroArch's Android package: a package launch of it starts RetroArch with no content.
extern const char kRetroarchPackage[];

// The core's library name from Nano's Android core path:
// "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so" -> "snes9x". "_libretro_android.so",
// "_libretro.so" or ".so" is stripped (plan 6.4). "" when the path names no usable core.
std::string coreName(const std::string& corePath);

// The argv for `request`: for a core, {retroarch, "-L", <coreDir>/<name>_libretro.so, "--config",
// <retroarchConfig>, rom} (rom left out when ""); for RetroArch's package, {retroarch, "--config",
// <retroarchConfig>}. False with *error for another package, or a core path that names no core.
// Pure: launch() checks that the program and the core are there.
bool argvFor(const nanoplat::LaunchRequest& request, const Options& options, std::vector<std::string>* argv,
             std::string* error);

// The request in the handshake's keys, as the control keys read them when the trigger fired.
nanoplat::LaunchRequest requestFromHandshake(const controlkeys::LaunchRequest& handshake);

// `display` is the platform's (may be null: no hand-off); it must outlive the launcher.
std::unique_ptr<nanoplat::Launcher> create(nanoplat::Display* display, const Options& options = Options());

// controlkeys::Handlers.launch: requestFromHandshake, then nanoplat::get().launcher->launch(); a
// launch that can't start is logged.
void launchFromHandshake(const controlkeys::LaunchRequest& handshake);

// Polls `launcher` every pollMs until its child has ended; the child's end, or nullopt when there
// was no child.
std::optional<nanoplat::ChildExit> waitForChild(nanoplat::Launcher& launcher, int pollMs = 100);

}  // namespace launcher
}  // namespace nanolinux
