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

// thimsy-nano's main() (docs/linux/IMPLEMENTATION.md B.7, G9): the Linux build links this file
// instead of nano/main.cpp. thimsy-session starts it (linux/docs/session.md) as root, in its own
// mount namespace, with stdout and stderr in /run/thimsy/log/nano.log.
//
// It sets up the platform before any thread exists, then runs Nano's existing startup: the parts of
// main.cpp's non-overlay path that apply on Linux, in the same order. Until G7-main moves those steps
// into a shared nanoBootSequence(), this file mirrors them by hand:
//
//   main.cpp step                                     here
//   --overlay (the in-game SurfaceFlinger overlay)    refused: exit 0 (no SurfaceFlinger)
//   drastic-nano relaunch (auto_relaunch)             left out: the DS player is out of scope
//   overlay_home stray-instance guard                 left out: no overlay, so no stray
//   early DRM master grab on /dev/dri/card0           left out: no HWComposer to beat; readyToRun's
//                                                     drmEarlySplash opens and takes the card itself
//                                                     (gEarlyDrmFd stays -1, as on Qualcomm)
//   setpriority(ANDROID_PRIORITY_DISPLAY)             kept
//   sys.gammaos.nano.drm_active=0                     kept
//   drastic patch, preload, init, QR watcher          left out (drasticNanoAvailable() is false)
//   ProcessState thread pool                          left out: no binder
//   new NanoMenu, run("GammaOSNano", PRIORITY_DISPLAY) kept
//   IPCThreadState::joinThreadPool()                  NanoMenu::join(): on Android the process ends
//                                                     in threadLoop's stopProcess(); here main
//                                                     returns when the thread ends
//
// What G7-main must change here: call nanoBootSequence() instead of the kept steps (and delete this
// table), and install(createPlatform()) with the Linux platform before it, after the set-up below.

#define LOG_TAG "ThimsyNano"

#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include <string>

#include <cutils/properties.h>
#include <log/log.h>
#include <nanocompat/nanocompat.h>
#include <system/thread_defs.h>
#include <utils/StrongPointer.h>
#include <utils/Thread.h>

#include "ControlKeys.h"
#include "NanoMenu.h"

using namespace android;

// main.cpp's global, read once by NanoMenu::readyToRun (NanoMenu.cpp): -1, so it opens the card.
int gEarlyDrmFd = -1;

namespace {

constexpr const char* kPersistFile = "/storage/.config/thimsy/persist.props";
constexpr const char* kRunDir = "/run/thimsy/props";
constexpr const char* kLogFile = "/run/thimsy/log/nano.log";

}  // namespace

int main(int argc, char** argv) {
    // The platform, before any thread: the log file, the property store (watching, so other
    // processes' writes reach Nano: thimsy-session's answers, volumes serial, power state), and the
    // control-key observers. The shell gate needs no set-up: it is linked in (--wrap).
    nanocompat::setLogFile(kLogFile);
    std::string err;
    nanocompat::InitOptions opts;
    opts.persistFile = kPersistFile;
    opts.runDir = kRunDir;
    opts.watch = true;
    if (!nanocompat::init(opts, &err)) {
        // Nano still runs on the lazy default store (every get gives its default): a menu with
        // default settings is better than no menu.
        ALOGE("property store %s, %s: %s", kPersistFile, kRunDir, err.c_str());
    }
    nanolinux::controlkeys::install();

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--overlay") == 0) {
            ALOGW("--overlay: the in-game overlay needs SurfaceFlinger, which Linux does not have; exiting");
            return 0;
        }
    }

    setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_DISPLAY);
    ALOGI("Thimsy Nano starting (Linux)");
    property_set("sys.gammaos.nano.drm_active", "0");

    sp<NanoMenu> nano = new NanoMenu();
    status_t st = nano->run("GammaOSNano", PRIORITY_DISPLAY);
    if (st != OK) {
        ALOGE("NanoMenu::run failed: %d", (int)st);
        return 1;
    }
    nano->join();
    ALOGI("Thimsy Nano: menu thread ended, exiting");
    nanolinux::controlkeys::uninstall();
    return 0;
}
