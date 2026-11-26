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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkMediaCodec.h>: no codec exists
// (both creates return null); every other call fails or returns nothing. Video, music and radio
// decoding on Linux is a later platform service (Cap::MediaPlayers off, IMPLEMENTATION.md B.4).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <android/native_window.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AMediaCodec;
typedef struct AMediaCodec AMediaCodec;
struct AMediaCrypto;
typedef struct AMediaCrypto AMediaCrypto;

typedef struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

enum {
    AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2,
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1,
};

AMediaCodec* AMediaCodec_createCodecByName(const char* name);
AMediaCodec* AMediaCodec_createDecoderByType(const char* mimeType);
media_status_t AMediaCodec_delete(AMediaCodec* codec);
media_status_t AMediaCodec_configure(AMediaCodec* codec, const AMediaFormat* format, ANativeWindow* surface,
                                     AMediaCrypto* crypto, uint32_t flags);
media_status_t AMediaCodec_start(AMediaCodec* codec);
media_status_t AMediaCodec_stop(AMediaCodec* codec);
media_status_t AMediaCodec_flush(AMediaCodec* codec);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* codec, size_t idx, size_t* outSize);
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t idx, size_t* outSize);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* codec, int64_t timeoutUs);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* codec, size_t idx, off_t offset, size_t size,
                                            uint64_t time, uint32_t flags);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec, AMediaCodecBufferInfo* info, int64_t timeoutUs);
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* codec);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec, size_t idx, bool render);

#ifdef __cplusplus
}
#endif
