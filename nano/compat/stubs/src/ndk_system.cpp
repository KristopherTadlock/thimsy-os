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

// Inert definitions of the other NDK stubs: AAudio, sensors, the looper, performance hints and the
// binder service manager. Nothing can be created or found; each call logs once and fails.
#include <errno.h>

#include <aaudio/AAudio.h>
#include <android/binder_manager.h>
#include <android/looper.h>
#include <android/performance_hint.h>
#include <android/sensor.h>

#include <nanostubs/stub.h>

extern "C" {

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder) {
    NANOSTUB_ONCE();
    if (builder) *builder = nullptr;
    return AAUDIO_ERROR_UNAVAILABLE;
}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*, aaudio_direction_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, aaudio_format_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int32_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int32_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder*, aaudio_performance_mode_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder*, aaudio_usage_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder*, aaudio_content_type_t) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, AAudioStream_dataCallback, void*) { NANOSTUB_ONCE(); }
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder*, AAudioStream_errorCallback, void*) { NANOSTUB_ONCE(); }
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream** stream) {
    NANOSTUB_ONCE();
    if (stream) *stream = nullptr;
    return AAUDIO_ERROR_UNAVAILABLE;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder*) { return AAUDIO_OK; }
aaudio_result_t AAudioStream_close(AAudioStream*) { return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestStart(AAudioStream*) { NANOSTUB_ONCE(); return AAUDIO_ERROR_UNAVAILABLE; }
aaudio_result_t AAudioStream_requestPause(AAudioStream*) { NANOSTUB_ONCE(); return AAUDIO_ERROR_UNAVAILABLE; }
aaudio_result_t AAudioStream_requestStop(AAudioStream*) { NANOSTUB_ONCE(); return AAUDIO_ERROR_UNAVAILABLE; }
int32_t AAudioStream_getSampleRate(AAudioStream*) { NANOSTUB_ONCE(); return 0; }
int32_t AAudioStream_getChannelCount(AAudioStream*) { NANOSTUB_ONCE(); return 0; }

ASensorManager* ASensorManager_getInstance(void) { NANOSTUB_ONCE(); return nullptr; }
ASensorManager* ASensorManager_getInstanceForPackage(const char*) { NANOSTUB_ONCE(); return nullptr; }
const ASensor* ASensorManager_getDefaultSensor(ASensorManager*, int) { NANOSTUB_ONCE(); return nullptr; }
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager*, ALooper*, int, ALooper_callbackFunc, void*) {
    NANOSTUB_ONCE();
    return nullptr;
}
int ASensorEventQueue_enableSensor(ASensorEventQueue*, const ASensor*) { NANOSTUB_ONCE(); return -EINVAL; }
int ASensorEventQueue_disableSensor(ASensorEventQueue*, const ASensor*) { NANOSTUB_ONCE(); return -EINVAL; }
int ASensorEventQueue_setEventRate(ASensorEventQueue*, const ASensor*, int32_t) { NANOSTUB_ONCE(); return -EINVAL; }
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue*, ASensorEvent*, size_t) { NANOSTUB_ONCE(); return -EINVAL; }

ALooper* ALooper_forThread(void) { NANOSTUB_ONCE(); return nullptr; }
ALooper* ALooper_prepare(int) { NANOSTUB_ONCE(); return nullptr; }

APerformanceHintManager* APerformanceHint_getManager(void) { NANOSTUB_ONCE(); return nullptr; }
APerformanceHintSession* APerformanceHint_createSession(APerformanceHintManager*, const int32_t*, size_t, int64_t) {
    NANOSTUB_ONCE();
    return nullptr;
}
int APerformanceHint_reportActualWorkDuration(APerformanceHintSession*, int64_t) { NANOSTUB_ONCE(); return EINVAL; }

AIBinder* AServiceManager_checkService(const char*) { NANOSTUB_ONCE(); return nullptr; }

}  // extern "C"
