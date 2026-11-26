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

// Inert stub (layer 2, see nanostubs/stub.h) of libgui's <gui/BufferItemConsumer.h>: a consumer
// that never has a buffer (acquireBuffer always reports none).
#pragma once

#include <cstdint>

#include <gui/BufferItem.h>
#include <gui/IGraphicBufferConsumer.h>
#include <nanostubs/stub.h>
#include <ui/Fence.h>
#include <ui/PixelFormat.h>
#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/Timers.h>

namespace android {

class BufferItemConsumer : public virtual RefBase {
 public:
    BufferItemConsumer(const sp<IGraphicBufferConsumer>& /*consumer*/, uint64_t /*consumerUsage*/,
                       int /*bufferCount*/ = -1, bool /*controlledByApp*/ = false) {}

    void setName(const String8& /*name*/) {}
    status_t setDefaultBufferSize(uint32_t /*w*/, uint32_t /*h*/) { return NO_INIT; }
    status_t setDefaultBufferFormat(PixelFormat /*format*/) { return NO_INIT; }
    status_t acquireBuffer(BufferItem* /*item*/, nsecs_t /*presentWhen*/, bool /*waitForFence*/ = true) {
        NANOSTUB_ONCE();
        return NO_INIT;
    }
    status_t releaseBuffer(const BufferItem& /*item*/, const sp<Fence>& /*releaseFence*/ = Fence::NO_FENCE) {
        return NO_INIT;
    }
};

}  // namespace android
