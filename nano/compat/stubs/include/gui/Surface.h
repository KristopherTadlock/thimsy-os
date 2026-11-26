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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/Surface.h>: a window no producer ever
// feeds. It derives from ANativeWindow, as Android's does, so Nano can pass one where a window is
// wanted. The Linux and host builds compile with EGL_NO_PLATFORM_SPECIFIC_TYPES (stubs.mk), so
// EGLNativeWindowType is void* and Nano's eglCreateWindowSurface(..., surface.get(), ...) compiles;
// those calls are on the SurfaceFlinger path only and never run there.
#pragma once

#include <gui/IGraphicBufferProducer.h>
#include <nanostubs/stub.h>
#include <system/window.h>
#include <utils/Errors.h>

namespace android {

class Surface : public ANativeWindow, public virtual RefBase {
 public:
    explicit Surface(const sp<IGraphicBufferProducer>& /*producer*/, bool /*controlledByApp*/ = false) {}

    int setMaxDequeuedBufferCount(int /*count*/) {
        NANOSTUB_ONCE();
        return NO_INIT;
    }
};

}  // namespace android
