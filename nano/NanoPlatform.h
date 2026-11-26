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
// NanoMenu thread; its EGL context is current from readyToRun() on). The firmware's is readyToRun's
// DRM splash decision, moved (platform/firmware/Display.cpp); Linux's always takes the panel and
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
    // releaseForChild: waits for the flip in flight, disables Nano's overlay planes, frees the
    // scanout ring and drops master; false when the panel was not Nano's.
    // reacquire: takes master back (retrying while the child's file is still being closed),
    // modesets and rebuilds the ring; false when the panel could not be taken back.
    virtual bool releaseForChild() = 0;
    virtual bool reacquire() = 0;
};
// --- end Display ---------------------------------------------------------------------------------

// The platform's services. Empty until the first G7 step declares one. Each platform library
// derives from it to keep its own state; services added later are members here.
struct Platform {
    virtual ~Platform();
    std::unique_ptr<Display> display;  // G7-display: never null once createPlatform() returns
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
