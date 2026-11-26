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

// The Linux display's Kms (Display.h), bound to NanoMenuDrm.cpp's KMS code (NanoMenuDrm.h), which
// the Linux build compiles unchanged over the GBM back end (B.5). Every call is on the render
// thread with Nano's EGL context current, as drmStop() and drmReacquireForHome() need.
//
// Written, not compiled here (NanoMenuDrm.h needs the EGL, GLES and libdrm headers): G10 runs
// -fsyntax-only on it against them.

#define LOG_TAG "ThimsyDisplay"

#include "Display.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include <memory>
#include <string>

#include <log/log.h>

#include "NanoMenuDrm.h"

namespace nanolinux {
namespace display {

namespace {

int sysIoctl(int fd, unsigned long request, void* arg) { return ::ioctl(fd, request, arg); }

class DrmKms : public Kms {
 public:
    void earlySplash(int fd) override { android::drmEarlySplash(fd); }

    void finishGpu() override { glFinish(); }  // GLES2/gl2.h, through NanoMenuDrm.h

    bool active() const override { return android::sDrmActive; }

    void setRotationDeg(int deg) override { android::sDrmRotationDeg = deg; }

    std::string panelOrientation() override {
        if (android::sDrmFd < 0 || android::sDrmDisplays.empty()) return "";
        int idx = android::sDrmPrimaryIdx;
        if (idx < 0 || idx >= (int)android::sDrmDisplays.size()) idx = 0;
        return readPanelOrientation(android::sDrmFd, android::sDrmDisplays[idx].connId, sysIoctl);
    }

    void waitForFlips() override { android::drmDrainPageFlipEvents(); }

    int disableOverlayPlanes() override {
        // The planes Nano adds over a CRTC's primary are the Cluster (AFBC) ones of sDrmAfbcMode.
        // drmStop() then clears sDrmDisplays, and the re-splash sets them up again.
        int n = 0;
        for (const android::DrmDisplay& d : android::sDrmDisplays) {
            if (d.clPlaneId == 0 || !d.clConfigured || android::sDrmFd < 0) continue;
            struct drm_mode_set_plane sp;
            memset(&sp, 0, sizeof(sp));
            sp.plane_id = d.clPlaneId;  // fb_id 0: the plane is turned off
            if (ioctl(android::sDrmFd, DRM_IOCTL_MODE_SETPLANE, &sp) == 0) {
                n++;
            } else {
                ALOGW("could not turn Cluster plane %u off: %s", d.clPlaneId, strerror(errno));
            }
        }
        return n;
    }

    void rememberInstallMatrix() override { android::drmRememberInstallMatrix(); }

    void stop() override { android::drmStop(); }

    bool takeBack(int timeoutMs) override {
        EGLDisplay dpy = eglGetCurrentDisplay();
        if (dpy == EGL_NO_DISPLAY) {
            ALOGE("takeBack: no current EGL display (not the render thread?)");
            return false;
        }
        return android::drmReacquireForHome(dpy, timeoutMs);
    }
};

}  // namespace

std::unique_ptr<Kms> createKms() { return std::unique_ptr<Kms>(new DrmKms); }

}  // namespace display
}  // namespace nanolinux
