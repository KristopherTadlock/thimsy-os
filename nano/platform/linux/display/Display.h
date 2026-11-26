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

// The Linux platform's nanoplat::Display (docs/linux/IMPLEMENTATION.md B.6, G7-display; plan 5.3),
// on NanoMenuDrm.cpp's KMS code with the GBM back end (DrmBuffersGbm.cpp):
//
// - startScanout: Linux has no SurfaceFlinger and no composer that needs master, so the panel is
//   always Nano's: drmEarlySplash takes master, modesets and shows the splash. Then the rotation
//   contract: one rotation for the panel, written to sDrmRotationDeg, so the shared code's GL path
//   (initShaders' matrix, sDrmGlRotation) turns every frame at present time.
// - releaseForChild, before a game's process starts: waits for the flip in flight, turns Nano's
//   overlay (Cluster) planes off, keeps the install matrix and calls drmStop, which frees the ring,
//   drops master and closes the fd, so the child opens card0 and becomes master itself.
// - reacquire, after the child has exited: drmReacquireForHome reopens card0 and takes master
//   (retrying while the child's file is still being closed), runs drmEarlySplash's full modeset,
//   rebuilds the ring on the same EGL context and puts the matrix back; then the rotation again,
//   since the re-splash re-read it.
//
// The rotation contract's sources, in order (device.schema.json's rotation_source): the primary
// connector's DRM "panel orientation" property, then fbcon's rotation, then device.json's
// panel.rotation (ro.thimsy.device.panel.rotation, written by thimsy-prop). Degrees are in
// device.json's convention, the device tree's `rotation`. When the store has
// ro.surface_flinger.primary_display_orientation, drmEarlySplash has applied it and it wins.
//
// The calls into NanoMenuDrm.cpp go through Kms, bound in DisplayKms.cpp, so this header and
// Display.cpp build and are tested without the EGL and libdrm headers.
#pragma once

#include <stdint.h>

#include <memory>
#include <string>

#include "NanoPlatform.h"

namespace nanolinux {
namespace display {

// What the Linux display needs of NanoMenuDrm.cpp (NanoMenuDrm.h). Render thread only.
class Kms {
 public:
    virtual ~Kms() = default;
    virtual void earlySplash(int fd) = 0;        // drmEarlySplash(fd): master, modeset, splash
    virtual bool active() const = 0;             // sDrmActive
    virtual void setRotationDeg(int deg) = 0;    // sDrmRotationDeg
    virtual std::string panelOrientation() = 0;  // the primary connector's, readPanelOrientation()
    virtual void waitForFlips() = 0;             // drmDrainPageFlipEvents
    virtual int disableOverlayPlanes() = 0;      // the Cluster planes Nano set up, off; how many
    virtual void rememberInstallMatrix() = 0;    // drmRememberInstallMatrix
    virtual void stop() = 0;                     // drmStop
    virtual bool takeBack(int timeoutMs) = 0;    // drmReacquireForHome(the current EGL display, ...)
};

// NanoMenuDrm.h's KMS code (DisplayKms.cpp).
std::unique_ptr<Kms> createKms();

struct Options {
    std::string fbconRotatePath = "/sys/class/graphics/fbcon/rotate";
    int reacquireTimeoutMs = 8000;  // as the parked home's drmReacquireForHome (NanoMenuDrasticPark.cpp)
};

std::unique_ptr<nanoplat::Display> create(std::unique_ptr<Kms> kms, const Options& options = Options());

// The rotation contract's sources, as degrees (0, 90, 180, 270) in device.json's convention, or -1
// when the text names no rotation. *(guess, checked on the ARC in G10: Nano's sDrmRotationDeg, from
// SurfaceFlinger's ORIENTATION_<n> on Android, turns the same way as the device tree's rotation)*
int rotationFromPanelOrientation(const std::string& enumName);  // the kernel's names, e.g. "Right Side Up"
int rotationFromFbcon(const std::string& text);                  // /sys/class/graphics/fbcon/rotate: 0 to 3
int rotationFromDegrees(const std::string& text);                // "0", "90", "180" or "270"

// PanelOrientation.cpp: the name of the value of DRM connector `connectorId`'s "panel orientation"
// property, read on `drmFd` through `ioctlFn` (::ioctl in production); "" when the connector has
// none or a call fails.
using IoctlFn = int (*)(int fd, unsigned long request, void* arg);
std::string readPanelOrientation(int drmFd, uint32_t connectorId, IoctlFn ioctlFn);

}  // namespace display
}  // namespace nanolinux
