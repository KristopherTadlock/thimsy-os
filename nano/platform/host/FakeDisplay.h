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

// The host's nanoplat::Display (docs/linux/IMPLEMENTATION.md B.6, D.3): the Linux display's
// behaviour (the panel is always Nano's; the hand-off drops and retakes it) over DrmHost.cpp, which
// stands in for NanoMenuDrm.cpp. It records what it was asked and the display size it was given,
// and tells the runner with a {"ev":"display"} event per call: "op" is "start" (with "w", "h" and
// "rotation"), "release" or "reacquire" (with "ok").
//
// The calls into DrmHost.cpp go through HostDrm, bound in FakeDisplayDrm.cpp, so this file and the
// host platform's small programs (check-headless-0) build without the EGL and libdrm headers.
#pragma once

#include <memory>

#include "NanoPlatform.h"

namespace nanohost {

// What the fake display needs of NanoMenuDrm.h's API (DrmHost.cpp on the host).
class HostDrm {
 public:
    virtual ~HostDrm() = default;
    virtual void earlySplash(int fd) = 0;      // drmEarlySplash
    virtual bool active() const = 0;           // sDrmActive
    virtual int width() const = 0;             // the primary display's mode size, 0 when none
    virtual int height() const = 0;
    virtual int rotationDeg() const = 0;       // sDrmRotationDeg (the host config's rotation)
    virtual void waitForFlips() = 0;           // drmDrainPageFlipEvents
    virtual void rememberInstallMatrix() = 0;  // drmRememberInstallMatrix
    virtual void stop() = 0;                   // drmStop
    virtual bool takeBack(int timeoutMs) = 0;  // drmReacquireForHome(the current EGL display, ...)
};

// DrmHost.cpp's API (FakeDisplayDrm.cpp).
std::unique_ptr<HostDrm> createHostDrm();

// What the fake display has done, for tests.
struct DisplayRecord {
    int starts = 0, releases = 0, reacquires = 0;
    int width = 0, height = 0;  // the size the last start was given
};

std::unique_ptr<nanoplat::Display> createFakeDisplay(std::unique_ptr<HostDrm> drm);

// The record of the most recently made fake display (zeros before one is made).
DisplayRecord displayRecord();

}  // namespace nanohost
