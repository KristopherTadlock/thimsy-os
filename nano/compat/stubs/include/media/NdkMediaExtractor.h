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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkMediaExtractor.h>: no
// extractor can be created (AMediaExtractor_new returns null); every other call fails or finds
// nothing.
// off64_t: C++ (g++ defines _GNU_SOURCE) and Bionic always have it; a C file on glibc needs
// _LARGEFILE64_SOURCE or _GNU_SOURCE before its first include.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AMediaExtractor;
typedef struct AMediaExtractor AMediaExtractor;

typedef enum {
    AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC = 0,
    AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC = 2,
} SeekMode;

AMediaExtractor* AMediaExtractor_new(void);
media_status_t AMediaExtractor_delete(AMediaExtractor* extractor);
media_status_t AMediaExtractor_setDataSourceFd(AMediaExtractor* extractor, int fd, off64_t offset,
                                               off64_t length);
media_status_t AMediaExtractor_setDataSource(AMediaExtractor* extractor, const char* location);
media_status_t AMediaExtractor_setDataSourceCustom(AMediaExtractor* extractor, AMediaDataSource* source);
size_t AMediaExtractor_getTrackCount(AMediaExtractor* extractor);
AMediaFormat* AMediaExtractor_getTrackFormat(AMediaExtractor* extractor, size_t idx);
AMediaFormat* AMediaExtractor_getFileFormat(AMediaExtractor* extractor);
media_status_t AMediaExtractor_selectTrack(AMediaExtractor* extractor, size_t idx);
ssize_t AMediaExtractor_readSampleData(AMediaExtractor* extractor, uint8_t* buffer, size_t capacity);
int64_t AMediaExtractor_getSampleTime(AMediaExtractor* extractor);
bool AMediaExtractor_advance(AMediaExtractor* extractor);
media_status_t AMediaExtractor_seekTo(AMediaExtractor* extractor, int64_t seekPosUs, SeekMode mode);

#ifdef __cplusplus
}
#endif
