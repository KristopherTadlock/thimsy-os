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

// Inert stub (layer 2, see nanostubs/stub.h) of libui's <ui/GraphicBuffer.h>. No graphic buffer is
// ever made (screen capture and the PSP clock's mirror never deliver one); a lock fails.
#pragma once

#include <cstdint>

#include <nanostubs/stub.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>

struct ANativeWindowBuffer;

namespace android {

class GraphicBuffer : public virtual RefBase {
 public:
    enum : uint64_t {
        USAGE_SW_READ_OFTEN = 0x3,
        USAGE_HW_TEXTURE = 0x100,
    };

    status_t lock(uint32_t /*usage*/, void** vaddr) {
        NANOSTUB_ONCE();
        if (vaddr) *vaddr = nullptr;
        return INVALID_OPERATION;
    }
    status_t unlock() { return INVALID_OPERATION; }
    uint32_t getWidth() const { return 0; }
    uint32_t getHeight() const { return 0; }
    uint32_t getStride() const { return 0; }
    uint64_t getId() const { return 0; }
    ANativeWindowBuffer* getNativeBuffer() const { return nullptr; }
};

}  // namespace android
