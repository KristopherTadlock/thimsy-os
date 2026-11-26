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

// The firmware's nanoplat::InputPolicy (NanoPlatform.h; docs/linux/IMPLEMENTATION.md B.6): the
// home's EVIOCGRAB silencing, moved verbatim from NanoMenuInput.cpp (openInputDevices and
// checkInputHotplug's IN_CREATE, IN_DELETE and sweep), with NanoMenuInput.cpp's LOG_TAG so the
// firmware's log is unchanged. deviceOpened's parameters carry the names the moved lines read at
// their call sites (the opened fd and its path), so the lines are the same; each case is one site.

#define LOG_TAG "GammaOSNano"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include <memory>

#include <linux/input.h>

#include <android-base/properties.h>
#include <utils/Log.h>

#include "InputPolicy.h"
#include "NanoPlatform.h"

namespace android {
namespace {

class FirmwareInputPolicy : public nanoplat::InputPolicy {
 public:
    void deviceOpened(int fd, const char* path, Opened where) override {
        switch (where) {
        case Opened::AtStart:
            // Exclusive grab: prevent Android InputReader from stealing events.
            // Gated by property — disable when preload is off to avoid input
            // ownership issues during app transitions.
            if (android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                    ALOGW("EVIOCGRAB failed for %s: %s", path, strerror(errno));
                }
                ALOGD("Opened + grabbed input device: %s", path);
            } else {
                ALOGD("Opened input device (no grab): %s", path);
            }
            break;
        case Opened::Hotplugged:
            // Home-mode grab only (persist.gammaos.nano.grab_input).
            // The overlay does NOT grab: it isolates the app's input
            // via the framework drop_input prop, not EVIOCGRAB.
            if (android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                    ALOGW("EVIOCGRAB failed for hotplugged %s: %s", path, strerror(errno));
                }
            }
            break;
        case Opened::Swept:
            // Home-mode grab only; the overlay isolates via drop_input, not grab.
            if (android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                    ALOGW("EVIOCGRAB failed for swept %s: %s", path, strerror(errno));
                }
            }
            break;
        }
    }

    void deviceClosing(int fd) override {
        ioctl(fd, EVIOCGRAB, 0);
    }

    // The firmware silences each device as it opens it (deviceOpened), and its home exits on a
    // launch, so the menu is never up over a running game here.
    void silenceGamePad(bool) override {}
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::InputPolicy> nanofirmware::createInputPolicy() {
    return std::unique_ptr<nanoplat::InputPolicy>(new android::FirmwareInputPolicy);
}
