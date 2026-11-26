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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkMediaFormat.h>. No format can
// be created (AMediaFormat_new returns null), so no codec is ever configured; the getters find
// nothing and the setters do nothing. The key names keep Android's values.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <media/NdkMediaError.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AMediaFormat;
typedef struct AMediaFormat AMediaFormat;

AMediaFormat* AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat* format);
bool AMediaFormat_getInt32(AMediaFormat* format, const char* name, int32_t* out);
bool AMediaFormat_getInt64(AMediaFormat* format, const char* name, int64_t* out);
bool AMediaFormat_getString(AMediaFormat* format, const char* name, const char** out);
void AMediaFormat_setInt32(AMediaFormat* format, const char* name, int32_t value);
void AMediaFormat_setString(AMediaFormat* format, const char* name, const char* value);
void AMediaFormat_setBuffer(AMediaFormat* format, const char* name, const void* data, size_t size);

extern const char* AMEDIAFORMAT_KEY_ALBUM;
extern const char* AMEDIAFORMAT_KEY_ARTIST;
extern const char* AMEDIAFORMAT_KEY_BIT_RATE;
extern const char* AMEDIAFORMAT_KEY_CDTRACKNUMBER;
extern const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT;
extern const char* AMEDIAFORMAT_KEY_CSD_0;
extern const char* AMEDIAFORMAT_KEY_CSD_1;
extern const char* AMEDIAFORMAT_KEY_CSD_2;
extern const char* AMEDIAFORMAT_KEY_DURATION;
extern const char* AMEDIAFORMAT_KEY_GENRE;
extern const char* AMEDIAFORMAT_KEY_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_LANGUAGE;
extern const char* AMEDIAFORMAT_KEY_MIME;
extern const char* AMEDIAFORMAT_KEY_PCM_ENCODING;
extern const char* AMEDIAFORMAT_KEY_SAMPLE_RATE;
extern const char* AMEDIAFORMAT_KEY_TITLE;
extern const char* AMEDIAFORMAT_KEY_WIDTH;
extern const char* AMEDIAFORMAT_KEY_YEAR;

#ifdef __cplusplus
}
#endif
