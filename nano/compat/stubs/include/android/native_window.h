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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <android/native_window.h>: the window
// type and the frame-rate constants Nano passes to SurfaceComposerClient::Transaction::setFrameRate.
// The struct itself is defined by <system/window.h> (gui::Surface derives from it).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ANativeWindow;
typedef struct ANativeWindow ANativeWindow;

enum ANativeWindow_FrameRateCompatibility {
    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT = 0,
};

enum ANativeWindow_ChangeFrameRateStrategy {
    ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS = 0,
};

#ifdef __cplusplus
}
#endif
