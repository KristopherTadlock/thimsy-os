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
//   which Nano reads back as state, and the control keys observe them). The launcher also keeps the
//   full value of each key it wrote: the store refuses a value of 92 bytes or more (a ROM on a card's
//   long path), and then holds the key's older value, or none (requestFor).
// - The trigger: the control keys (ControlKeys.h) turn do_launch=1 or service.bootanim.nano_retroarch=1
//   into one request, debounced; main_linux.cpp gives them launchFromHandshake() as the launch
//   handler, which builds the request (LinuxLauncher::requestFor) and calls the platform's launch().
//   Only a trigger written by Nano's own commitLaunch() is a launch: one from another process is
//   logged and refused while Nano runs. A launch Nano wrote sets sys.gammaos.nano.app_launched=1
//   (kAppLaunchedKey), which Android's framework sets, so threadLoop's exit path does not wait.
// - launch: checks the request (the argv, argvFor; the program and the core are there) and queues it.
//   Nano's launch sites still exit after a launch, and code runs after the trigger (Quick Resume
//   saves the core's state and SRAM for RetroArch to resume from, the fades, the pad grab), so the
//   game starts only once Nano's menu thread has ended and NanoMenu is gone: main_linux.cpp calls
//   leaveQueued(), as Android starts the activity as the home exits.
// - startQueued: Display::releaseForChild, then fork; the child calls setsid (its own session and
//   process group), puts the signals back to their defaults, closes every descriptor but 0, 1 and 2,
//   and execve's the argv with Nano's environment. A close-on-exec pipe tells the parent whether
//   execve worked; when it did not, or fork failed, the child is reaped, the panel is taken back
//   (Display::reacquire) and startQueued() returns false.
// - A started child's pid is sys.thimsy.game.pid (kGamePidKey, linux/docs/session.md): thimsy-session
//   stops, continues and quits that group, and holds Nano's restart until it ends. pollChild clears
//   the key when it reports the child's end.
// - pollChild: waitpid(WNOHANG) on the child. requestChildQuit: SIGTERM to its process group.
// - The launch's end (pollChild reports the child's end, or a requested launch did not start): the
//   launcher clears launch_app, launch_intent, launch_core and launch_rom, as Android's framework
//   does when it returns to Nano (nanoStopAppToHome), so the next trigger never reads a stale core
//   or ROM.
//
// - Nano exits after a launch (review findings linux-host-impls-1, -2, -6): main_linux.cpp starts the
//   queued launch with leaveQueued() and returns, as Android's home exits when it starts a game, so
//   no thread ~NanoMenu leaves behind (the render watchdog, the scan workers) outlives the process
//   for the game's length. The child is left running with sys.thimsy.game.pid set; thimsy-session
//   watches it and starts Nano again, without a back-off, when it ends. The launch is over for this
//   Nano at once, so the handshake is cleared then (the next Nano never reads a stale core or ROM),
//   and Nano exits 0 (kExitLaunchOver) after a launch, started or not, else kExitNoLaunch.
//
// nanoExitsOnLaunch() is false: the intended Linux contract (a resident Nano that hands the panel
// over and polls the child: startQueued(), pollChild(), waitForChild()). Until threadLoop does that,
// the exit above stands in for it; the panel goes back to Nano by the session's restart of Nano, not
// by reacquire().
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

// RetroArch's Android package: a package launch of it, or of no package (launch_app "", which Nano
// and the framework read as RetroArch), starts RetroArch with no content.
extern const char kRetroarchPackage[];

// The game's pid for thimsy-session (linux/docs/session.md): set when a child starts.
extern const char kGamePidKey[];

// sys.gammaos.nano.app_launched: "1" once launchFromHandshake has taken Nano's own launch (queued
// or refused), as Android's framework sets it when it starts the app. threadLoop's exit path waits
// for it (up to ~10 s of "Loading..."), so on Linux it ends at once. thimsy-session clears it (and
// app_drawn) when the game ends and before it starts Nano with no game running: a stale "1" reads
// as a game in front of Nano (no idle sleep, no ES-DE card sweep, BIOS installs refused).
extern const char kAppLaunchedKey[];

// thimsy-nano's exit status after its menu ended (thimsy-session restarts Nano at once, its back-off
// reset, after kExitLaunchOver when no game runs; any other end counts as a crash).
constexpr int kExitLaunchOver = 0;  // a launch was asked for: its game left running, or it did not start
constexpr int kExitNoLaunch = 1;    // the menu ended with no launch

// The core's library name from Nano's Android core path:
// "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so" -> "snes9x". "_libretro_android.so",
// "_libretro.so" or ".so" is stripped (plan 6.4). "" when the path names no usable core.
std::string coreName(const std::string& corePath);

// The argv for `request`: for a core, {retroarch, "-L", <coreDir>/<name>_libretro.so, "--config",
// <retroarchConfig>, rom} (rom left out when ""); for RetroArch's package (or ""), {retroarch,
// "--config", <retroarchConfig>}. False with *error for another package, or a core path that names
// no core. Pure: launch() checks that the program and the core are there.
bool argvFor(const nanoplat::LaunchRequest& request, const Options& options, std::vector<std::string>* argv,
             std::string* error);

// The request in the handshake's keys, as the control keys read them when the trigger fired.
nanoplat::LaunchRequest requestFromHandshake(const controlkeys::LaunchRequest& handshake);

// The Linux launcher's calls beyond nanoplat::Launcher's (linuxLauncher() finds the installed one).
class LinuxLauncher : public nanoplat::Launcher {
 public:
    // requestFromHandshake(handshake), with the value this launcher's commit() last wrote to a key
    // wherever the store still holds what it held right after that write (the store refused a value
    // too long for a property, so it kept the older value, or "").
    virtual nanoplat::LaunchRequest requestFor(const controlkeys::LaunchRequest& handshake) const = 0;
    // Whether the trigger being handled now was written by this launcher's commit() on this thread
    // (Nano's own launch), not by another process.
    virtual bool inCommit() const = 0;
    // A launch is queued (launch() accepted it and startQueued() has not run).
    virtual bool queued() const = 0;
    // Starts the queued launch: once Nano's menu has ended (main_linux.cpp, through runQueued()). True
    // when the child started. False when nothing was queued (*error "") or it could not start
    // (*error says why; the handshake is cleared, as at a launch's end).
    virtual bool startQueued(std::string* error) = 0;

    // What startAndLeave() did.
    enum class Leave {
        NoLaunch,    // no launch was asked for since the last one ended
        Started,     // the child started and is left running
        NotStarted,  // a launch was asked for and refused, or it could not start (*error says why)
    };
    // startQueued() for a Nano that exits after the launch (main_linux.cpp, through leaveQueued()):
    // a started child is left running (pollChild() no longer reports it; sys.thimsy.game.pid stays
    // set for the session), *pid is its pid (else -1), and the launch is over for Nano: the handshake
    // is cleared at once, as at a launch's end.
    virtual Leave startAndLeave(int* pid, std::string* error) = 0;
};

// `display` is the platform's (may be null: no hand-off); it must outlive the launcher.
std::unique_ptr<LinuxLauncher> create(nanoplat::Display* display, const Options& options = Options());

// The installed platform's launcher when it is a LinuxLauncher, else nullptr.
LinuxLauncher* linuxLauncher();

// controlkeys::Handlers.launch: refused (logged) unless the trigger came from the launcher's own
// commit(); else LinuxLauncher::requestFor, then launch(); a launch that can't be queued is logged.
// Either way kAppLaunchedKey is then "1" (Nano's launch site ends the menu, and Nano exits).
void launchFromHandshake(const controlkeys::LaunchRequest& handshake);

// Polls `launcher` every pollMs until its child has ended; the child's end, or nullopt when there
// was no child.
std::optional<nanoplat::ChildExit> waitForChild(nanoplat::Launcher& launcher, int pollMs = 100);

// A resident Nano's end of a launch: starts the queued launch of the installed launcher (a failure
// is logged) and waits for the child (waitForChild). The child's end, or nullopt when no child ran.
std::optional<nanoplat::ChildExit> runQueued(int pollMs = 100);

// main_linux.cpp, after the menu thread has ended and NanoMenu is gone: the installed launcher's
// startAndLeave() (a launch that does not start is logged); *pid is the child left running, or -1.
LinuxLauncher::Leave leaveQueued(int* pid);

}  // namespace launcher
}  // namespace nanolinux
