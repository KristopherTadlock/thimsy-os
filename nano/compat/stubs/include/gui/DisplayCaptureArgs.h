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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/DisplayCaptureArgs.h>: the arguments
// of a screen capture (ScreenshotClient never captures).
#pragma once

#include <cstdint>
#include <unordered_set>

#include <binder/IBinder.h>
#include <gui/SpHash.h>
#include <ui/PixelFormat.h>

namespace android {
namespace gui {

struct CaptureArgs {
    ui::PixelFormat pixelFormat = ui::PixelFormat::RGBA_8888;
    bool captureSecureLayers = false;
    bool allowProtected = false;
    std::unordered_set<sp<IBinder>, SpHash<IBinder>> excludeHandles;
};

struct DisplayCaptureArgs : CaptureArgs {
    sp<IBinder> displayToken;
    uint32_t width = 0;
    uint32_t height = 0;
};

}  // namespace gui
}  // namespace android
