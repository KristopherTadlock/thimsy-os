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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/ScreenCaptureResults.h>: what a
// capture delivers (never, here).
#pragma once

#include <ui/Fence.h>
#include <ui/FenceResult.h>
#include <ui/GraphicBuffer.h>

namespace android {
namespace gui {

struct ScreenCaptureResults {
    sp<GraphicBuffer> buffer;
    FenceResult fenceResult = Fence::NO_FENCE;
    bool capturedSecureLayers = false;
};

}  // namespace gui
}  // namespace android
