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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkImage.h>: no image ever exists
// (AImageReader never delivers one); every accessor fails.
#pragma once

#include <stdint.h>

#include <media/NdkMediaError.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AImage;
typedef struct AImage AImage;

enum AIMAGE_FORMATS {
    AIMAGE_FORMAT_YUV_420_888 = 0x23,
};

typedef struct AImageCropRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} AImageCropRect;

void AImage_delete(AImage* image);
media_status_t AImage_getWidth(const AImage* image, int32_t* width);
media_status_t AImage_getHeight(const AImage* image, int32_t* height);
media_status_t AImage_getCropRect(const AImage* image, AImageCropRect* rect);
media_status_t AImage_getPlanePixelStride(const AImage* image, int planeIdx, int32_t* pixelStride);
media_status_t AImage_getPlaneRowStride(const AImage* image, int planeIdx, int32_t* rowStride);
media_status_t AImage_getPlaneData(const AImage* image, int planeIdx, uint8_t** data, int* dataLength);

#ifdef __cplusplus
}
#endif
