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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/DisplayId.h>: SurfaceFlinger's display
// ids. With no SurfaceFlinger the list of physical displays is always empty.
#pragma once

#include <cstdint>

namespace android {

struct DisplayId {
    uint64_t value = 0;
};

struct PhysicalDisplayId : DisplayId {
    uint8_t getPort() const { return static_cast<uint8_t>(value); }
};

}  // namespace android
