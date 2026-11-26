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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/Rect.h>: an ARect with the
// constructors and sizes Nano's SurfaceFlinger code uses.
#pragma once

#include <cstdint>

#include <android/rect.h>

namespace android {

class Rect : public ARect {
 public:
    Rect() : Rect(0, 0, 0, 0) {}
    Rect(int32_t w, int32_t h) : Rect(0, 0, w, h) {}
    Rect(int32_t l, int32_t t, int32_t r, int32_t b) {
        left = l;
        top = t;
        right = r;
        bottom = b;
    }
    int32_t getWidth() const { return right - left; }
    int32_t getHeight() const { return bottom - top; }
};

}  // namespace android
