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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/SurfaceControl.h>: a SurfaceFlinger
// layer. SurfaceComposerClient never makes one, so these members are never reached; they keep
// their Android shapes so the calls compile.
#pragma once

#include <cstdint>

#include <binder/IBinder.h>
#include <gui/Surface.h>

namespace android {

class SurfaceControl : public virtual RefBase {
 public:
    sp<Surface> getSurface() { return nullptr; }
    sp<IBinder> getHandle() const { return nullptr; }
    bool isValid() const { return false; }
    void updateDefaultBufferSize(uint32_t /*width*/, uint32_t /*height*/) {}
};

}  // namespace android
