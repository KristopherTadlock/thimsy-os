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

// The firmware's nanoplat::Display (NanoPlatform.h; docs/linux/IMPLEMENTATION.md B.6): readyToRun()'s
// DRM splash decision, moved verbatim from NanoMenu.cpp, with NanoMenu.cpp's LOG_TAG so the
// firmware's log is unchanged. startScanout's parameters carry the names of what the moved lines
// read in readyToRun (its local earlyDrmFd, the member mOverlayMode and NanoMenu.cpp's static
// nanoForceSfPath(), which readyToRun's SurfaceFlinger set-up still calls), so the lines are the same.

#define LOG_TAG "GammaOSNano"

#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <memory>

#include <drm.h>

#include <cutils/properties.h>
#include <utils/Log.h>

#include "Display.h"
#include "NanoMenuDrm.h"
#include "NanoPlatform.h"

namespace android {
namespace {

class FirmwareDisplay : public nanoplat::Display {
 public:
    int startScanout(int earlyDrmFd, bool mOverlayMode, bool (*nanoForceSfPath)()) override {
        // GammaOS: Nano mode is confirmed active (skip_nano check passed).
        // Run DRM splash now - before any SF/HWC setup. This replaces the
        // bootloader logo with NanoMenu's DRM direct rendering. Skip on
        // restarts (returning from app) since HWC is already active.
        {
            char bootDone[PROPERTY_VALUE_MAX] = {};
            property_get("sys.boot_completed", bootDone, "0");
            // force_drm=1 re-grabs DRM master post-boot for drastic nano
            // mode (XMB -> drastic nano restart path).
            char forceDrm[PROPERTY_VALUE_MAX] = {};
            property_get("sys.gammaos.nano.force_drm", forceDrm, "0");
            // Overlay mode never grabs DRM master: it coexists with SurfaceFlinger
            // and the running app as a translucent layer. Force the SF path.
            if (mOverlayMode || nanoForceSfPath()) {
                ALOGI("NanoMenu: %s, skipping DRM splash (SF path)",
                      mOverlayMode ? "overlay mode" : "force-SF home");
            } else if (strcmp(bootDone, "1") != 0 || strcmp(forceDrm, "1") == 0) {
                if (strcmp(forceDrm, "1") == 0) {
                    ALOGW("NanoMenu: force_drm=1, grabbing DRM master "
                          "post-boot for drastic nano");
                }
                drmEarlySplash(earlyDrmFd);
                earlyDrmFd = -1;
            } else {
                ALOGI("NanoMenu: skipping DRM splash (already booted)");
            }
        }
        if (earlyDrmFd >= 0 && !sDrmActive) {
            // DRM splash failed - release the fd so HWC can use it
            ioctl(earlyDrmFd, DRM_IOCTL_DROP_MASTER, 0);
            close(earlyDrmFd);
            earlyDrmFd = -1;
        }
        return earlyDrmFd;
    }

    // The firmware's home exits on every launch (init or the overlay brings it back), and the parked
    // home (NanoMenuDrasticPark.cpp) calls NanoMenuDrm.h itself, so nothing hands the panel over here.
    bool releaseForChild() override { return false; }
    bool reacquire() override { return false; }
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::Display> nanofirmware::createDisplay() {
    return std::unique_ptr<nanoplat::Display>(new android::FirmwareDisplay);
}
