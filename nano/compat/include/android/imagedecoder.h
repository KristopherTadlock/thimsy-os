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
 * Thimsy's <android/imagedecoder.h> for the Linux and host builds of Nano: the NDK's AImageDecoder
 * with Android's names, values and signatures, implemented by nano/compat/src/imagedecoder.cpp on
 * the system's image libraries (IMPLEMENTATION.md B.2, decision I30). The ES-DE theme decodes every
 * image through it, so it is a real decoder, not a stub.
 *
 *   PNG    libpng 1.6 (its simplified API)          JPEG  libjpeg (libjpeg-turbo), EXIF orientation
 *   WebP   libwebp, animated through libwebpdemux    GIF   giflib 5, animated, composited
 *   HEIF, AVIF, BMP and anything else: ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT.
 *
 * A format whose library was not found when the shim was compiled (__has_include) is unsupported
 * too: create returns ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT and the caller falls back, as it does
 * for a broken file.
 *
 * Differences from Android that Nano can't rely on either way:
 *   - Only ANDROID_BITMAP_FORMAT_RGBA_8888 output (others: INVALID_CONVERSION). Nano asks for nothing
 *     else.
 *   - A still image scales to any target size, with straight or premultiplied alpha (filtered in
 *     premultiplied space). An animated image can't be scaled (INVALID_SCALE), as Nano found on
 *     Android.
 *   - setDataSpace accepts only ADATASPACE_UNKNOWN (0) and sRGB; no colour management is done.
 */
#ifndef NANOCOMPAT_ANDROID_IMAGEDECODER_H
#define NANOCOMPAT_ANDROID_IMAGEDECODER_H

#include <android/bitmap.h>
#include <android/rect.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ANDROID_IMAGE_DECODER_SUCCESS = 0,
    ANDROID_IMAGE_DECODER_INCOMPLETE = -1,
    ANDROID_IMAGE_DECODER_ERROR = -2,
    ANDROID_IMAGE_DECODER_INVALID_CONVERSION = -3,
    ANDROID_IMAGE_DECODER_INVALID_SCALE = -4,
    ANDROID_IMAGE_DECODER_BAD_PARAMETER = -5,
    ANDROID_IMAGE_DECODER_INVALID_INPUT = -6,
    ANDROID_IMAGE_DECODER_SEEK_ERROR = -7,
    ANDROID_IMAGE_DECODER_INTERNAL_ERROR = -8,
    ANDROID_IMAGE_DECODER_UNSUPPORTED_FORMAT = -9,
    ANDROID_IMAGE_DECODER_FINISHED = -10,
    ANDROID_IMAGE_DECODER_INVALID_STATE = -11,
};

typedef struct AImageDecoder AImageDecoder;
typedef struct AImageDecoderHeaderInfo AImageDecoderHeaderInfo;
typedef struct AImageDecoderFrameInfo AImageDecoderFrameInfo;

/* A string for a result code ("ANDROID_IMAGE_DECODER_SUCCESS" and so on); NULL for an unknown one. */
const char* AImageDecoder_resultToString(int result);

/*
 * Reads the image from fd's current offset to its end. The decoder does not take ownership of fd,
 * and fd may be closed as soon as this returns.
 */
int AImageDecoder_createFromFd(int fd, AImageDecoder** outDecoder);
/* The buffer is not copied: it must stay valid until AImageDecoder_delete, as on Android. */
int AImageDecoder_createFromBuffer(const void* buffer, size_t length, AImageDecoder** outDecoder);
void AImageDecoder_delete(AImageDecoder* decoder);

int AImageDecoder_setAndroidBitmapFormat(AImageDecoder* decoder, int32_t format);
int AImageDecoder_setUnpremultipliedRequired(AImageDecoder* decoder, bool unpremultipliedRequired);
int AImageDecoder_setDataSpace(AImageDecoder* decoder, int32_t dataspace);
/* The output size. Any positive size for a still image; INVALID_SCALE for an animated one. A crop
 * set earlier must fit inside the new size (else INVALID_SCALE). */
int AImageDecoder_setTargetSize(AImageDecoder* decoder, int32_t width, int32_t height);
/* The size the image would have when sampled by sampleSize (each side divided, at least 1). */
int AImageDecoder_computeSampledSize(const AImageDecoder* decoder, int sampleSize, int32_t* width,
                                     int32_t* height);
/* A crop of the (target-size) output; an all-zero rect removes it. BAD_PARAMETER if it is empty or
 * does not fit. */
int AImageDecoder_setCrop(AImageDecoder* decoder, ARect crop);

const AImageDecoderHeaderInfo* AImageDecoder_getHeaderInfo(const AImageDecoder* decoder);
int32_t AImageDecoderHeaderInfo_getWidth(const AImageDecoderHeaderInfo* info);
int32_t AImageDecoderHeaderInfo_getHeight(const AImageDecoderHeaderInfo* info);
const char* AImageDecoderHeaderInfo_getMimeType(const AImageDecoderHeaderInfo* info);
int32_t AImageDecoderHeaderInfo_getAndroidBitmapFormat(const AImageDecoderHeaderInfo* info);
/* ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE or ANDROID_BITMAP_FLAGS_ALPHA_PREMUL. */
int AImageDecoderHeaderInfo_getAlphaFlags(const AImageDecoderHeaderInfo* info);
int32_t AImageDecoderHeaderInfo_getDataSpace(const AImageDecoderHeaderInfo* info);

/* Output width (after target size and crop) times 4. */
size_t AImageDecoder_getMinimumStride(AImageDecoder* decoder);
/* Decodes the current frame. size must be at least stride * (height - 1) + minimum stride. */
int AImageDecoder_decodeImage(AImageDecoder* decoder, void* pixels, size_t stride, size_t size);

bool AImageDecoder_isAnimated(AImageDecoder* decoder);

enum {
    ANDROID_IMAGE_DECODER_INFINITE = INT32_MAX,
};

/* How many times an animation repeats after the first time; ANDROID_IMAGE_DECODER_INFINITE for a
 * loop; 0 for a still image. */
int32_t AImageDecoder_getRepeatCount(AImageDecoder* decoder);
/* Moves to the next frame; ANDROID_IMAGE_DECODER_FINISHED after the last one. BAD_PARAMETER for a
 * still image. */
int AImageDecoder_advanceFrame(AImageDecoder* decoder);
int AImageDecoder_rewind(AImageDecoder* decoder);

AImageDecoderFrameInfo* AImageDecoderFrameInfo_create(void);
void AImageDecoderFrameInfo_delete(AImageDecoderFrameInfo* info);
/* The current frame's information. */
int AImageDecoder_getFrameInfo(AImageDecoder* decoder, AImageDecoderFrameInfo* info);
/* The frame's duration in nanoseconds. */
int64_t AImageDecoderFrameInfo_getDuration(const AImageDecoderFrameInfo* info);
ARect AImageDecoderFrameInfo_getFrameRect(const AImageDecoderFrameInfo* info);
bool AImageDecoderFrameInfo_hasAlphaWithinBounds(const AImageDecoderFrameInfo* info);

enum {
    ANDROID_IMAGE_DECODER_DISPOSE_OP_NONE = 1,
    ANDROID_IMAGE_DECODER_DISPOSE_OP_BACKGROUND = 2,
    ANDROID_IMAGE_DECODER_DISPOSE_OP_PREVIOUS = 3,
};
int32_t AImageDecoderFrameInfo_getDisposeOp(const AImageDecoderFrameInfo* info);

enum {
    ANDROID_IMAGE_DECODER_BLEND_OP_SRC = 1,
    ANDROID_IMAGE_DECODER_BLEND_OP_SRC_OVER = 2,
};
int32_t AImageDecoderFrameInfo_getBlendOp(const AImageDecoderFrameInfo* info);

/* Accepted and ignored: this decoder always composites frames itself. */
void AImageDecoder_setInternallyHandleDisposePrevious(AImageDecoder* decoder, bool handle);

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_ANDROID_IMAGEDECODER_H */
