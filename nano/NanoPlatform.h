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

// Nano's platform seam, layer 4 (docs/linux/IMPLEMENTATION.md B.6). C++17; no Android or Linux
// system header is included, and none may be.
//
// Declared when used, not ahead: this header holds only the platform object and how it is made,
// installed and reached. Each interface (Display, Launcher, Audio, Volumes, Power, SystemActions,
// InputPolicy, and the Cap set) is declared by the step that moves the code behind it, in the same
// commit as the move, sized to that code.
//
// Who may use it. drastic-nano, gammaos-nano-scrape, the host tests and the other binaries built
// from Nano's files never call install(). No file they compile may include this header or name
// nanoplat:: (tools/nano-tree/tests/test_nano_ratchets.py holds that rule); they reach platform
// behaviour through the shims and link-time substitution (layers 1 to 3).
//
// Ownership and threading. main() calls createPlatform(), then install(), before any Nano thread
// starts. The platform then lives until the process exits and is never destroyed, so a reference
// from get() stays valid on every thread, including during exit. Which platform is linked is a
// link-time choice: each platform library (platform/firmware, platform/linux, platform/host) defines
// createPlatform() once.
#pragma once

#include <memory>

namespace nanoplat {

// --- Display (G7-display) ------------------------------------------------------------------------
// The panel: taken at start-up, and handed to a game's process and back. Render thread only (the
// NanoMenu thread). startScanout runs in readyToRun() before its EGL set-up, so no EGL context is
// current then; releaseForChild and reacquire need Nano's context current. The firmware's is
// readyToRun's DRM splash decision, moved (platform/firmware/Display.cpp); Linux's always takes the panel and
// applies the rotation contract (platform/linux/display/); the host's records what it did
// (platform/host/FakeDisplay.cpp).
class Display {
 public:
    virtual ~Display() = default;

    // readyToRun(): takes the panel for Nano's own scanout (DRM master and the splash,
    // drmEarlySplash) unless this process presents through SurfaceFlinger. `earlyDrmFd` is main()'s
    // early DRM master fd, or -1; `overlayMode` is the --overlay instance; `forceSfPath` is
    // readyToRun's rule for the SoCs whose composer must keep master. Returns the early fd still
    // held, or -1; sDrmActive (NanoMenuDrm.h) tells whether the panel is Nano's.
    virtual int startScanout(int earlyDrmFd, bool overlayMode, bool (*forceSfPath)()) = 0;

    // The hand-off to a child process that becomes DRM master itself (a game), and back, for a
    // platform where Nano keeps running while the child runs (Linux). The firmware's home exits on
    // a launch instead, so its versions do nothing and return false.
    // releaseForChild: finishes the GPU's work, waits for the flip in flight, disables Nano's
    // overlay planes, frees the scanout ring and drops master; false when the panel was not Nano's.
    // reacquire: takes master back (retrying while the child's file is still being closed),
    // modesets and rebuilds the ring; false when the panel could not be taken back.
    virtual bool releaseForChild() = 0;
    virtual bool reacquire() = 0;
};
// --- end Display ---------------------------------------------------------------------------------

// --- InputPolicy (G7-input) ----------------------------------------------------------------------
// Who else gets the pads' events while Nano reads them. Nano's own evdev code (opening
// /dev/input/event*, reading, hotplug) is the same on every platform; what differs is silencing.
// The firmware's home grabs each device node as it opens it (EVIOCGRAB when
// persist.gammaos.nano.grab_input is set) and releases the grab before closing it: that code, moved
// from NanoMenuInput.cpp (platform/firmware/InputPolicy.cpp). On Linux the built-in pad is
// thimsy-padd's, and silencing it is padd's intercept (sys.thimsy.padd.intercept), so Linux leaves
// the device nodes alone (platform/linux/InputPolicy.cpp). The host's grabs as the firmware's does,
// so its fake devices see the same grabs, and records each call (platform/host/FakeInput.cpp).
class InputPolicy {
 public:
    virtual ~InputPolicy() = default;

    // Where Nano opened a device node: openInputDevices' first scan, checkInputHotplug's IN_CREATE,
    // or checkInputHotplug's once-a-second sweep for missed devices. The firmware's log lines name it.
    enum class Opened { AtStart, Hotplugged, Swept };

    // Render thread only (the NanoMenu thread: openInputDevices and checkInputHotplug).
    // deviceOpened: Nano has just opened the node `path` as `fd` (O_RDONLY | O_NONBLOCK; fd >= 0),
    // before it keeps the fd. deviceClosing: Nano is about to close `fd`, a device it opened whose
    // node went away (IN_DELETE, or the sweep found it deleted).
    virtual void deviceOpened(int fd, const char* path, Opened where) = 0;
    virtual void deviceClosing(int fd) = 0;

    // Any thread. Stops the built-in pad's events reaching anyone but Nano's menu (true), and gives
    // them back (false), for a platform where the menu can be up while a game runs (Linux: padd's
    // intercept; the menu then reads the pad from padd's tap). The firmware silences per device in
    // deviceOpened and its home exits on a launch, so its version does nothing.
    virtual void silenceGamePad(bool on) = 0;
};
// --- end InputPolicy -----------------------------------------------------------------------------

// The platform's services. Empty until the first G7 step declares one. Each platform library
// derives from it to keep its own state; services added later are members here.
struct Platform {
    virtual ~Platform();
    std::unique_ptr<Display> display;  // G7-display: never null once createPlatform() returns
    std::unique_ptr<InputPolicy> input;  // G7-input: never null once createPlatform() returns
};

struct PlatformArgs {
    int argc = 0;
    char** argv = nullptr;
};

// Made by the linked platform library. Returns nullptr when the platform can't start (the reason
// is logged); main() then exits with an error.
std::unique_ptr<Platform> createPlatform(const PlatformArgs& args);

// Makes `platform` the process's platform. Call it once, from main(), before any Nano thread
// starts. Aborts with a message when `platform` is null or a platform is already installed.
void install(std::unique_ptr<Platform> platform);

// The installed platform; any thread. Never null: aborts with a message if install() was never
// called (which the rule above keeps out of the binaries that don't call it).
Platform& get();

}  // namespace nanoplat
