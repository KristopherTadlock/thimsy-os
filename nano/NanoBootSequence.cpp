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

// nanoBootSequence() (NanoBootSequence.h): main.cpp's boot steps, moved verbatim, with main.cpp's
// LOG_TAG so the firmware's log is unchanged.

#define LOG_TAG "GammaOSNano"

#include <sys/resource.h>

#include <cutils/properties.h>
#include <utils/Log.h>

#include "NanoBootSequence.h"
#include "NanoMenu.h"

using namespace android;

// The early DRM master fd: defined by each main (main.cpp grabs it on non-Qualcomm SoCs).
extern int gEarlyDrmFd;

sp<NanoMenu> nanoBootSequence(void (*platformSteps)()) {
    setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_DISPLAY);

    ALOGI("GammaOS Nano starting... (earlyDrmFd=%d)", gEarlyDrmFd);

    property_set("sys.gammaos.nano.drm_active", "0");

    if (platformSteps) platformSteps();

    sp<NanoMenu> nano = new NanoMenu();

    // waitForSurfaceFlinger() removed: on the DRM boot path, SF is not
    // needed at all (headless EGL + DRM direct).  On restart (boot_completed=1),
    // readyToRun() creates SurfaceComposerClient which waits internally.

    nano->run("GammaOSNano", PRIORITY_DISPLAY);

    return nano;
}
