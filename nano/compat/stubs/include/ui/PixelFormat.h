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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/PixelFormat.h>: libui's integer pixel
// formats (android::PixelFormat) and the HAL's enum (android::ui::PixelFormat), the values Nano
// passes to SurfaceFlinger.
#pragma once

#include <cstdint>

namespace android {

typedef int32_t PixelFormat;

enum {
    PIXEL_FORMAT_RGBA_8888 = 1,
    PIXEL_FORMAT_RGBX_8888 = 2,
};

namespace ui {
enum class PixelFormat : int32_t { RGBA_8888 = 1 };
}  // namespace ui

}  // namespace android
