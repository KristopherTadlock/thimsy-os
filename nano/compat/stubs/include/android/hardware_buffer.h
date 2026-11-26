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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <android/hardware_buffer.h>: the type,
// the description struct and the constants Nano's [core] and [drm] files use.
//
// AHardwareBuffer_release, _describe, _lock and _unlock are DECLARED here and defined by no stub:
// NanoMenuDrm.cpp calls them on the render targets its buffer back end allocates, so the back end
// defines them for its own handles (link-time substitution, IMPLEMENTATION.md B.5 and B.8): on
// Linux nano/platform/linux/display/DrmBuffersGbm.cpp. The host build does not compile
// NanoMenuDrm.cpp and nothing else calls them. Allocation (AHardwareBuffer_allocate) is the back
// end's business too and is not declared.
#pragma once

#include <stdint.h>

#include <android/rect.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AHardwareBuffer AHardwareBuffer;

typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

enum AHardwareBuffer_Format {
    AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1,
};

enum AHardwareBuffer_UsageFlags {
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3ULL,
};

void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
int AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect,
                         void** outVirtualAddress);
int AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence);

#ifdef __cplusplus
}
#endif
