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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/LayerStack.h>: a SurfaceFlinger layer
// stack id, with Android's default (0) and invalid (UINT32_MAX) values.
#pragma once

#include <cstdint>

namespace android {
namespace ui {

struct LayerStack {
    uint32_t id = UINT32_MAX;

    static constexpr LayerStack fromValue(uint32_t v) { return LayerStack{v}; }
};

constexpr LayerStack INVALID_LAYER_STACK{};
constexpr LayerStack DEFAULT_LAYER_STACK{0u};

inline bool operator==(LayerStack a, LayerStack b) { return a.id == b.id; }
inline bool operator!=(LayerStack a, LayerStack b) { return a.id != b.id; }

}  // namespace ui
}  // namespace android
