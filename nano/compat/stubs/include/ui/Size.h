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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/Size.h>: a width and a height.
#pragma once

#include <cstdint>

namespace android {
namespace ui {

struct Size {
    int32_t width = -1;
    int32_t height = -1;

    Size() = default;
    Size(int32_t w, int32_t h) : width(w), height(h) {}
    int32_t getWidth() const { return width; }
    int32_t getHeight() const { return height; }
};

}  // namespace ui
}  // namespace android
