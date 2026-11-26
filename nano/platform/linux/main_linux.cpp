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

// thimsy-nano's main() (docs/linux/IMPLEMENTATION.md B.7, G9, G7-main): the Linux build links this
// file instead of nano/main.cpp. thimsy-session starts it (linux/docs/session.md) as root, in its own
// mount namespace, with stdout and stderr in /run/thimsy/log/nano.log.
//
// It sets up the platform before any thread exists, installs the Linux platform
// (LinuxPlatform.cpp), then runs main.cpp's boot steps through the shared nanoBootSequence()
// (NanoBootSequence.cpp: the display priority, drm_active=0, NanoMenu and its thread). What
// main.cpp does around that sequence is firmware-only:
//
//   main.cpp                                          here
//   --overlay (the in-game SurfaceFlinger overlay)    refused: exit 0 (no SurfaceFlinger)
//   drastic-nano relaunch (auto_relaunch)             left out: the DS player is out of scope
//   overlay_home stray-instance guard                 left out: no overlay, so no stray
//   early DRM master grab on /dev/dri/card0           left out: no HWComposer to beat; readyToRun's
//                                                     drmEarlySplash opens and takes the card itself
//                                                     (gEarlyDrmFd stays -1, as on Qualcomm)
//   firmwareBootSteps(): DraStic's patch, preload,    left out: nanoBootSequence(nullptr) (no DraStic,
//   init and QR watcher; binder's thread pool         no binder)
//   IPCThreadState::joinThreadPool()                  NanoMenu::join(): on Android the process ends
//                                                     in threadLoop's stopProcess(); here main
//                                                     returns when the thread ends
//
// tools/nano-tree/tests/test_main_linux.py fingerprints main.cpp's main() and firmwareBootSteps(),
// so a change there is reviewed against this list.

#define LOG_TAG "ThimsyNano"

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>

#include <log/log.h>
#include <nanocompat/nanocompat.h>
#include <utils/StrongPointer.h>
#include <utils/Thread.h>

#include "ControlKeys.h"
#include "NanoBootSequence.h"
#include "NanoMenu.h"
#include "NanoPlatform.h"

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

    nanoplat::PlatformArgs args;
    args.argc = argc;
    args.argv = argv;
    std::unique_ptr<nanoplat::Platform> platform = nanoplat::createPlatform(args);
    if (!platform) {
        ALOGE("the Linux platform did not start; exiting");
        nanolinux::controlkeys::uninstall();
        return 2;
    }
    nanoplat::install(std::move(platform));

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--overlay") == 0) {
            ALOGW("--overlay: the in-game overlay needs SurfaceFlinger, which Linux does not have; exiting");
            return 0;
        }
    }

    ALOGI("Thimsy Nano starting (Linux)");
    sp<NanoMenu> nano = nanoBootSequence(nullptr);
    // run()'s status is not returned (main.cpp ignores it too): when the thread did not start
    // (logged by run()), join() returns at once and main exits as after a normal end.
    nano->join();
    ALOGI("Thimsy Nano: menu thread ended, exiting");
    nanolinux::controlkeys::uninstall();
    return 0;
}
