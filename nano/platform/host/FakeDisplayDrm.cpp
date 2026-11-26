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

// The fake display's HostDrm (FakeDisplay.h), bound to NanoMenuDrm.h's API, which DrmHost.cpp
// defines on the host. Built into nano-headless only: NanoMenuDrm.h needs the EGL, GLES and libdrm
// headers, so check-headless-0's programs link their own HostDrm.

#define LOG_TAG "NanoHost"

#include "FakeDisplay.h"

#include <memory>

#include <log/log.h>

#include "NanoMenuDrm.h"

namespace nanohost {

namespace {

class DrmHostApi : public HostDrm {
 public:
    void earlySplash(int fd) override { android::drmEarlySplash(fd); }
    bool active() const override { return android::sDrmActive; }
    int width() const override { return primary() ? (int)primary()->w : 0; }
    int height() const override { return primary() ? (int)primary()->h : 0; }
    int rotationDeg() const override { return android::sDrmRotationDeg; }
    void waitForFlips() override { android::drmDrainPageFlipEvents(); }
    void rememberInstallMatrix() override { android::drmRememberInstallMatrix(); }
    void stop() override { android::drmStop(); }
    bool takeBack(int timeoutMs) override {
        EGLDisplay dpy = eglGetCurrentDisplay();
        if (dpy == EGL_NO_DISPLAY) {
            ALOGE("fake display: no current EGL display (not the render thread?)");
            return false;
        }
        return android::drmReacquireForHome(dpy, timeoutMs);
    }

 private:
    static const android::DrmDisplay* primary() {
        if (android::sDrmDisplays.empty()) return nullptr;
        int idx = android::sDrmPrimaryIdx;
        if (idx < 0 || idx >= (int)android::sDrmDisplays.size()) idx = 0;
        return &android::sDrmDisplays[idx];
    }
};

}  // namespace

std::unique_ptr<HostDrm> createHostDrm() { return std::unique_ptr<HostDrm>(new DrmHostApi); }

}  // namespace nanohost
