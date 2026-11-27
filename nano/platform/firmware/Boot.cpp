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

// The firmware's nanoplat::Boot (NanoPlatform.h; docs/linux/ARCHITECTURE-AUDIT.md step 2): moved
// verbatim from NanoMenu.cpp (readyToRun's boot-owner decision and its hand-over to the boot
// animation, readyToRun's and threadLoop's waits for persistent properties), NanoMenuRender.cpp
// (startupPreloadStart's look at them) and NanoMenuPS3Boot.cpp (the boot sound's look for the audio
// service), with NanoMenu.cpp's LOG_TAG so the firmware's log is unchanged. handOverBootScreen's
// parameters carry the names the moved lines read in readyToRun (its local earlyDrmFd and the member
// mOverlayMode), so the lines are the same; each case of awaitSavedSettings is one call site.

#define LOG_TAG "GammaOSNano"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <memory>

#include <drm.h>

#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/String16.h>

#include "Boot.h"
#include "NanoPlatform.h"

namespace android {
namespace {

class FirmwareBoot : public nanoplat::Boot {
 public:
    bool nanoOwnsBootScreen() override {
        // GammaOS Nano: decide whether to run NanoMenu or bail to stock
        // bootanim. The durable source of truth is
        // persist.bootanim.skip_nano (loaded from /data/property/
        // persistent_properties by init's load_persist_props), but that
        // load completes after /data is decrypted and after this readyToRun
        // call. To avoid a long wait on ro.persistent_properties.ready
        // every boot, we cache the same value in /data/misc/bootanim/
        // nano_skip whenever it changes through the on-device action
        // dispatchers in init.rc. /data/misc/bootanim is in DE storage so
        // it is readable as soon as /data is mounted, no FBE unlock
        // required.
        //
        // Fast path: read the file. "0" = nano, "1" = bootanim. Anything
        // else (file missing on first boot, malformed content) drops to
        // the property fallback with a wait, mirroring the original
        // behavior.
        bool decided = false;
        bool fileBootanim = false;
        {
            int fd = open("/data/misc/bootanim/nano_skip",
                          O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                char buf[8] = {};
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (n > 0) {
                    // Strip trailing newline/whitespace from init's
                    // `write` directive output.
                    for (ssize_t i = 0; i < n; i++) {
                        if (buf[i] == '\n' || buf[i] == '\r' ||
                            buf[i] == ' ') { buf[i] = 0; break; }
                    }
                    if (strcmp(buf, "0") == 0) {
                        ALOGI("GammaOS Nano: DE state file = '0', "
                              "running nano");
                        decided = true;
                    } else if (strcmp(buf, "1") == 0) {
                        ALOGI("GammaOS Nano: DE state file = '1', "
                              "starting bootanim");
                        decided = true;
                        fileBootanim = true;
                    } else {
                        ALOGW("GammaOS Nano: DE state file has "
                              "unexpected content '%s', falling back to "
                              "persist property", buf);
                    }
                }
            }
        }

        if (!decided) {
            // Slow path: wait up to 10 s for persist props, then read.
            char ready[PROPERTY_VALUE_MAX] = {};
            property_get("ro.persistent_properties.ready", ready, "");
            if (strcmp(ready, "true") != 0) {
                ALOGI("GammaOS Nano: persist props not ready, waiting...");
                for (int i = 0; i < 1000; i++) {
                    usleep(10000);
                    property_get("ro.persistent_properties.ready", ready, "");
                    if (strcmp(ready, "true") == 0) break;
                }
            }
            char skip[PROPERTY_VALUE_MAX] = {};
            property_get("persist.bootanim.skip_nano", skip, "");
            if (strcmp(skip, "0") == 0) {
                ALOGI("GammaOS Nano: skip_nano='0', running nano");
            } else {
                ALOGI("GammaOS Nano: skip_nano='%s' (not '0'), "
                      "starting bootanim", skip);
                fileBootanim = true;
            }
        }
        return !fileBootanim;
    }

    void handOverBootScreen(int earlyDrmFd, bool mOverlayMode) override {
        if (earlyDrmFd >= 0) {
            ioctl(earlyDrmFd, DRM_IOCTL_DROP_MASTER, 0);
            close(earlyDrmFd);
        }
        // The overlay process is NEVER the boot-animation owner. In normal Android
        // (skip_nano != '0') persist.gammaos.nano.overlay stays 1 so the rc trigger
        // can still start the overlay service; if it reaches here, starting bootanim
        // is exactly what made the boot animation reappear at random. Just exit.
        if (!mOverlayMode) {
            property_set("ctl.start", "bootanim");
        } else {
            ALOGI("GammaOS Nano overlay: not nano mode (skip_nano!='0'), exiting without bootanim");
        }
        _exit(0);
    }

    void awaitSavedSettings(SettingsReader reader) override {
        switch (reader) {
        case SettingsReader::QuickResumeCheck:
            // Persist props can race with our early start. gammaos-nano begins
            // around T+7 s on the Brick while persistent_properties loads at
            // ~T+9 to 10 s. Without a brief wait here, the qr_prepared read
            // below comes back "0" on every cold boot and the drastic fast
            // path is silently skipped (NanoMenu falls into the XMB render
            // loop instead). Wait up to 2 s, breaking out as soon as the
            // property service publishes ro.persistent_properties.ready=true.
            {
                char ready[PROPERTY_VALUE_MAX] = {};
                property_get("ro.persistent_properties.ready", ready, "");
                if (strcmp(ready, "true") != 0) {
                    int waited = 0;
                    for (int i = 0; i < 200; i++) {
                        usleep(10000);
                        waited += 10;
                        property_get("ro.persistent_properties.ready",
                                     ready, "");
                        if (!strcmp(ready, "true")) break;
                    }
                    if (strcmp(ready, "true") == 0) {
                        ALOGI("NanoMenu: persist props ready after %d ms "
                              "(drastic QR pre-check)", waited);
                    } else {
                        ALOGW("NanoMenu: persist props still not ready after "
                              "%d ms (drastic QR pre-check), continuing",
                              waited);
                    }
                }
            }
            break;
        case SettingsReader::MenuSetup:
            // Wait for persistent properties to load before reading any persist.*
            // values. readyToRun() used to wait for ro.persistent_properties.ready
            // up top, but the 2026-05-13 boot-speed work moved that wait out of
            // the EGL hot path so first paint is not delayed. We still need the
            // wait here, otherwise the re-reads below (and the Quick Resume gate
            // further down at "if (mQuickResumeEnabled)") fall back to defaults
            // when persist props have not loaded yet, silently disabling QR auto
            // resume on every cold boot. Cap at 3 s, which is plenty given persist
            // props normally settle within ~200 ms of /data being mounted.
            {
                char ready[PROPERTY_VALUE_MAX] = {};
                property_get("ro.persistent_properties.ready", ready, "");
                if (strcmp(ready, "true") != 0) {
                    int waited = 0;
                    for (int i = 0; i < 300; i++) {
                        usleep(10000);
                        waited += 10;
                        property_get("ro.persistent_properties.ready", ready, "");
                        if (!strcmp(ready, "true")) break;
                    }
                    if (strcmp(ready, "true") == 0) {
                        ALOGI("NanoMenu: persist props ready after %d ms", waited);
                    } else {
                        ALOGW("NanoMenu: persist props still not ready after "
                              "%d ms, continuing with possibly stale defaults",
                              waited);
                    }
                }
            }
            break;
        }
    }

    bool savedSettingsLoaded() override {
        char ready[PROPERTY_VALUE_MAX] = {};
        property_get("ro.persistent_properties.ready", ready, "");
        return strcmp(ready, "true") == 0;
    }

    bool soundSystemReady() override {
        auto audioUp = []() { return defaultServiceManager()->checkService(String16("audio")) != nullptr; };
        return audioUp();
    }
};

}  // namespace
}  // namespace android

std::unique_ptr<nanoplat::Boot> nanofirmware::createBoot() {
    return std::unique_ptr<nanoplat::Boot>(new android::FirmwareBoot);
}
