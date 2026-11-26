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

/*
 * Thimsy's <android/bitmap.h> for the Linux and host builds of Nano: the NDK's bitmap formats and
 * flags, which AImageDecoder uses. The JNI calls (AndroidBitmap_lockPixels and the rest) are not
 * provided: there is no Java VM.
 */
#ifndef NANOCOMPAT_ANDROID_BITMAP_H
#define NANOCOMPAT_ANDROID_BITMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ANDROID_BITMAP_RESULT_SUCCESS = 0,
    ANDROID_BITMAP_RESULT_BAD_PARAMETER = -1,
    ANDROID_BITMAP_RESULT_JNI_EXCEPTION = -2,
    ANDROID_BITMAP_RESULT_ALLOCATION_FAILED = -3,
};

#define ANDROID_BITMAP_RESUT_SUCCESS ANDROID_BITMAP_RESULT_SUCCESS

enum AndroidBitmapFormat {
    ANDROID_BITMAP_FORMAT_NONE = 0,
    ANDROID_BITMAP_FORMAT_RGBA_8888 = 1,
    ANDROID_BITMAP_FORMAT_RGB_565 = 4,
    ANDROID_BITMAP_FORMAT_RGBA_4444 = 7,
    ANDROID_BITMAP_FORMAT_A_8 = 8,
    ANDROID_BITMAP_FORMAT_RGBA_F16 = 9,
    ANDROID_BITMAP_FORMAT_RGBA_1010102 = 10,
};

enum {
    ANDROID_BITMAP_FLAGS_ALPHA_PREMUL = 0,
    ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE = 1,
    ANDROID_BITMAP_FLAGS_ALPHA_UNPREMUL = 2,
    ANDROID_BITMAP_FLAGS_ALPHA_MASK = 0x3,
    ANDROID_BITMAP_FLAGS_ALPHA_SHIFT = 0,
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    int32_t format;
    uint32_t flags;
} AndroidBitmapInfo;

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_ANDROID_BITMAP_H */
