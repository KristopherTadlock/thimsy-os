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

// nano-headless: Nano on the host with no GPU and no window (docs/linux/IMPLEMENTATION.md D.3).
// It does what main.cpp does, on the host platform: the platform first (createPlatform, install),
// then NanoMenu, run through the compat android::Thread on real time, exactly as on a device.
// NanoMenu::threadLoop holds the whole session (set-up, its own frame loop, teardown); when it
// returns, the process ends. On the host, readyToRun takes the DRM path because DrmHost.cpp's
// drmEarlySplash sets sDrmActive.
//
// The steps of main.cpp it mirrors (main.cpp:944-1220), and the ones it leaves out:
//   mirrors   property_set("sys.gammaos.nano.drm_active", "0") before NanoMenu starts;
//             new NanoMenu(); run("GammaOSNano", PRIORITY_DISPLAY)
//   leaves out the --overlay instance and its guards (an SF window: [firmware]); the drastic-nano
//             relaunch, audio patch, library preload, init and QR watcher (DraStic is [firmware]);
//             the stray-instance guard of overlay_home (no second instance on the host); the early
//             DRM master grab (no DRM device: gEarlyDrmFd stays -1); the binder thread pool
//             (main() joins NanoMenu's thread instead, since IPCThreadState::stopProcess is inert)
//
// Usage: nano-headless [--host-config PATH] (or THIMSY_HEADLESS_CONFIG; HostConfig.h). Exit status:
// 0 when Nano's thread ended, 2 when the platform could not start, 3 when the thread did not start.

#define LOG_TAG "NanoHeadless"

#include <cutils/properties.h>
#include <log/log.h>
#include <utils/StrongPointer.h>
#include <utils/ThreadDefs.h>

#include "NanoMenu.h"
#include "NanoPlatform.h"
#include "platform/host/HostControl.h"

// main.cpp's early DRM master fd, which NanoMenu::readyToRun reads (extern in NanoMenu.cpp). The
// host has no DRM device.
int gEarlyDrmFd = -1;

using android::NanoMenu;
using android::sp;
using android::wp;

int main(int argc, char** argv) {
    nanoplat::PlatformArgs args;
    args.argc = argc;
    args.argv = argv;
    std::unique_ptr<nanoplat::Platform> platform = nanoplat::createPlatform(args);
    if (!platform) return 2;
    nanoplat::install(std::move(platform));

    property_set("sys.gammaos.nano.drm_active", "0");

    sp<NanoMenu> nano = new NanoMenu();
    wp<NanoMenu> weak(nano);
    nanohost::setQuitHandler([weak]() {
        sp<NanoMenu> strong = weak.promote();
        if (strong != nullptr) strong->requestExit();
    });

    if (nano->run("GammaOSNano", android::PRIORITY_DISPLAY) != android::OK) {
        ALOGE("nano-headless: NanoMenu's thread did not start");
        nanohost::event("exit", "\"code\":3");
        return 3;
    }
    ALOGI("nano-headless: NanoMenu running");
    const android::status_t st = nano->join();
    nanohost::event("exit", "\"code\":0,\"thread_status\":" + std::to_string((int)st));
    return 0;
}
