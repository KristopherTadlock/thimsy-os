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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <aaudio/AAudio.h>: the subset Nano's
// [core] files use (NanoAudio.cpp, NanoMenuPS3Menu.cpp). No stream can be created:
// AAudio_createStreamBuilder and AAudioStreamBuilder_openStream return AAUDIO_ERROR_UNAVAILABLE
// with a null result, and every other call does nothing. Linux audio is ALSA's, behind the platform
// layer (IMPLEMENTATION.md B.6), not AAudio's.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t aaudio_result_t;
enum {
    AAUDIO_OK = 0,
    AAUDIO_ERROR_DISCONNECTED = -899,
    AAUDIO_ERROR_UNAVAILABLE = -889,
};

typedef int32_t aaudio_direction_t;
enum {
    AAUDIO_DIRECTION_OUTPUT = 0,
};

typedef int32_t aaudio_format_t;
enum {
    AAUDIO_FORMAT_PCM_I16 = 1,
};

typedef int32_t aaudio_performance_mode_t;
enum {
    AAUDIO_PERFORMANCE_MODE_NONE = 10,
    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
};

typedef int32_t aaudio_usage_t;
enum {
    AAUDIO_USAGE_MEDIA = 1,
};

typedef int32_t aaudio_content_type_t;
enum {
    AAUDIO_CONTENT_TYPE_MUSIC = 2,
};

typedef int32_t aaudio_data_callback_result_t;
enum {
    AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
    AAUDIO_CALLBACK_RESULT_STOP = 1,
};

struct AAudioStreamStruct;
typedef struct AAudioStreamStruct AAudioStream;
struct AAudioStreamBuilderStruct;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;

typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream* stream, void* userData,
                                                                   void* audioData, int32_t numFrames);
typedef void (*AAudioStream_errorCallback)(AAudioStream* stream, void* userData, aaudio_result_t error);

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channelCount);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sampleRate);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder* builder, aaudio_content_type_t contentType);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder, AAudioStream_dataCallback callback,
                                         void* userData);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder, AAudioStream_errorCallback callback,
                                          void* userData);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder, AAudioStream** stream);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* builder);

aaudio_result_t AAudioStream_close(AAudioStream* stream);
aaudio_result_t AAudioStream_requestStart(AAudioStream* stream);
aaudio_result_t AAudioStream_requestPause(AAudioStream* stream);
aaudio_result_t AAudioStream_requestStop(AAudioStream* stream);
int32_t AAudioStream_getSampleRate(AAudioStream* stream);
int32_t AAudioStream_getChannelCount(AAudioStream* stream);

#ifdef __cplusplus
}
#endif
