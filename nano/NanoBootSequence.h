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

// main()'s portable boot steps, shared by every main that runs the home menu
// (docs/linux/IMPLEMENTATION.md B.7): main.cpp on the firmware, platform/linux/main_linux.cpp on
// Linux. Moved from main.cpp's main() as they were.
#pragma once

#include <utils/StrongPointer.h>

namespace android {
class NanoMenu;
}

// Raises the process to the display priority, logs the start, clears sys.gammaos.nano.drm_active,
// runs `platformSteps` (may be null: on the firmware, DraStic's boot work and binder's thread pool),
// then makes NanoMenu and starts its thread. Returns the menu; the caller waits for it.
//
// Call it from main(), on the main thread, after nanoplat::install() and after the early DRM
// section: NanoMenu::readyToRun takes gEarlyDrmFd (defined by each main) once, on its own thread.
android::sp<android::NanoMenu> nanoBootSequence(void (*platformSteps)());
