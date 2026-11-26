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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <android/sensor.h>: there is no sensor
// manager (ASensorManager_getInstance returns null), so the PSP clock's tilt parallax stays still.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <android/looper.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ASensorManager;
typedef struct ASensorManager ASensorManager;
struct ASensorEventQueue;
typedef struct ASensorEventQueue ASensorEventQueue;
struct ASensor;
typedef struct ASensor ASensor;

enum {
    ASENSOR_TYPE_ACCELEROMETER = 1,
};

typedef struct ASensorVector {
    float x;
    float y;
    float z;
    int8_t status;
    uint8_t reserved[3];
} ASensorVector;

typedef struct ASensorEvent {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union {
        float data[16];
        ASensorVector acceleration;
    };
    uint32_t flags;
    int32_t reserved1[3];
} ASensorEvent;

ASensorManager* ASensorManager_getInstance(void);
ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName);
const ASensor* ASensorManager_getDefaultSensor(ASensorManager* manager, int type);
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager, ALooper* looper, int ident,
                                                   ALooper_callbackFunc callback, void* data);
int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, const ASensor* sensor);
int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, const ASensor* sensor);
int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, const ASensor* sensor, int32_t usec);
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count);

#ifdef __cplusplus
}
#endif
