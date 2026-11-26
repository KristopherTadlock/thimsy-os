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

// The Linux display (Display.h): the hand-off and the rotation contract over Kms.

#define LOG_TAG "ThimsyDisplay"

#include "Display.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>

#include <cutils/properties.h>
#include <log/log.h>

namespace nanolinux {
namespace display {

namespace {

const char kSfOrientationKey[] = "ro.surface_flinger.primary_display_orientation";
const char kDeviceRotationKey[] = "ro.thimsy.device.panel.rotation";
const char kRotationSourceKey[] = "ro.thimsy.device.panel.rotation_source";

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) e--;
    return s.substr(b, e - b);
}

// A small sysfs file's text; false when it can't be read.
bool readSmallFile(const std::string& path, std::string* out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[64];
    ssize_t n;
    do {
        n = ::read(fd, buf, sizeof(buf));
    } while (n < 0 && errno == EINTR);
    ::close(fd);
    if (n < 0) return false;
    out->assign(buf, (size_t)n);
    return true;
}

std::string prop(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "");
    return v;
}

class LinuxDisplay : public nanoplat::Display {
 public:
    LinuxDisplay(std::unique_ptr<Kms> kms, Options options) : kms_(std::move(kms)), options_(std::move(options)) {}

    int startScanout(int earlyDrmFd, bool overlayMode, bool (*)()) override {
        // No SurfaceFlinger: the panel is always Nano's (main_linux.cpp refuses --overlay).
        if (overlayMode) ALOGW("startScanout: overlay mode has no meaning on Linux; taking the panel");
        kms_->earlySplash(earlyDrmFd);  // owns earlyDrmFd from here (main_linux.cpp's is -1)
        if (!kms_->active()) {
            ALOGE("startScanout: drmEarlySplash found no display");
            return -1;
        }
        chooseRotation();
        applyRotation();
        return -1;
    }

    bool releaseForChild() override {
        if (released_) {
            ALOGW("releaseForChild: the panel is already released");
            return false;
        }
        if (!kms_->active()) {
            ALOGW("releaseForChild: the panel is not Nano's");
            return false;
        }
        kms_->finishGpu();  // before drmStop frees the ring the GPU may still be drawing into
        kms_->waitForFlips();
        const int planes = kms_->disableOverlayPlanes();
        kms_->rememberInstallMatrix();
        kms_->stop();
        released_ = true;
        ALOGI("released the panel for a child (%d overlay plane(s) off)", planes);
        return true;
    }

    bool reacquire() override {
        if (!released_) return kms_->active();
        if (!kms_->takeBack(options_.reacquireTimeoutMs)) {
            ALOGE("reacquire: could not take the panel back within %d ms", options_.reacquireTimeoutMs);
            return false;  // still released: the caller may try again
        }
        released_ = false;
        applyRotation();
        ALOGI("took the panel back");
        return true;
    }

 private:
    // The rotation contract (Display.h). Once per process: the sources don't change while it runs.
    void chooseRotation() {
        if (chosen_) return;
        chosen_ = true;
        const std::string sf = prop(kSfOrientationKey);
        if (!sf.empty()) {
            rotation_ = -1;  // drmEarlySplash applied it, and applies it again on every re-splash
            ALOGI("rotation: %s=%s (drmEarlySplash applied it)", kSfOrientationKey, sf.c_str());
            return;
        }
        // The order, from device.json's rotation_source (Display.h).
        enum Source { kDrm, kFbcon, kDeviceJson };
        const std::string wanted = prop(kRotationSourceKey);
        Source order[3] = {kDrm, kDeviceJson, kFbcon};
        if (wanted == "device-json" || wanted == "device-tree") {
            order[0] = kDeviceJson;
            order[1] = kDrm;
            order[2] = kFbcon;
        } else if (wanted == "fbcon") {
            order[0] = kFbcon;
            order[1] = kDeviceJson;
            order[2] = kDrm;
        } else if (!wanted.empty() && wanted != "drm-panel-orientation") {
            ALOGW("rotation: unknown %s=%s; using the DRM property first", kRotationSourceKey, wanted.c_str());
        }
        const char* source = "no source";
        int deg = -1;
        for (int i = 0; i < 3 && deg < 0; i++) {
            switch (order[i]) {
                case kDrm:
                    source = "the DRM panel orientation";
                    deg = rotationFromPanelOrientation(kms_->panelOrientation());
                    break;
                case kFbcon: {
                    source = "fbcon";
                    std::string text;
                    if (readSmallFile(options_.fbconRotatePath, &text)) deg = rotationFromFbcon(text);
                    break;
                }
                case kDeviceJson:
                    source = "device.json";
                    deg = rotationFromDegrees(prop(kDeviceRotationKey));
                    break;
            }
        }
        if (deg < 0) {
            source = "no source";
            deg = 0;
        }
        rotation_ = deg;
        ALOGI("rotation: %d degrees (from %s)", deg, source);
    }

    void applyRotation() {
        if (rotation_ >= 0) kms_->setRotationDeg(rotation_);
    }

    std::unique_ptr<Kms> kms_;
    const Options options_;
    bool released_ = false;
    bool chosen_ = false;
    int rotation_ = -1;  // degrees, or -1: leave what drmEarlySplash set
};

}  // namespace

int rotationFromPanelOrientation(const std::string& enumName) {
    // The kernel's names (drm_connector.c), from the device tree's rotation by
    // of_drm_get_panel_orientation: 0 Normal, 90 Right Side Up, 180 Upside Down, 270 Left Side Up.
    if (enumName == "Normal") return 0;
    if (enumName == "Right Side Up") return 90;
    if (enumName == "Upside Down") return 180;
    if (enumName == "Left Side Up") return 270;
    return -1;
}

int rotationFromFbcon(const std::string& text) {
    // FB_ROTATE_UR, _CW, _UD, _CCW. The DRM fbdev helper turns the console against the panel's
    // orientation: Right Side Up (90) gives CW, Left Side Up (270) gives CCW.
    const std::string t = trimmed(text);
    if (t == "0") return 0;
    if (t == "1") return 90;
    if (t == "2") return 180;
    if (t == "3") return 270;
    return -1;
}

int rotationFromDegrees(const std::string& text) {
    const std::string t = trimmed(text);
    if (t == "0") return 0;
    if (t == "90") return 90;
    if (t == "180") return 180;
    if (t == "270") return 270;
    return -1;
}

std::unique_ptr<nanoplat::Display> create(std::unique_ptr<Kms> kms, const Options& options) {
    if (!kms) return nullptr;
    return std::unique_ptr<nanoplat::Display>(new LinuxDisplay(std::move(kms), options));
}

}  // namespace display
}  // namespace nanolinux
