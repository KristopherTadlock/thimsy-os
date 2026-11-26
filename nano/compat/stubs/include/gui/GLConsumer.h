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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/GLConsumer.h>: a texture consumer that
// never receives a frame (updateTexImage always fails; the transform stays the identity).
#pragma once

#include <cstdint>

#include <gui/IGraphicBufferConsumer.h>
#include <nanostubs/stub.h>
#include <utils/Errors.h>
#include <utils/String8.h>

namespace android {

class GLConsumer : public virtual RefBase {
 public:
    GLConsumer(const sp<IGraphicBufferConsumer>& /*bq*/, uint32_t /*tex*/, uint32_t /*textureTarget*/,
               bool /*useFenceSync*/, bool /*isControlledByApp*/) {}

    void setName(const String8& /*name*/) {}
    status_t setDefaultBufferSize(uint32_t /*w*/, uint32_t /*h*/) { return NO_INIT; }
    status_t updateTexImage() {
        NANOSTUB_ONCE();
        return NO_INIT;
    }
    void getTransformMatrix(float mtx[16]) {
        for (int i = 0; i < 16; i++) mtx[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    void abandon() {}
};

}  // namespace android
