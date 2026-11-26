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

/* The layer-2 stubs whose Android originals are C headers (the NDK's) compile as C too, all in one
 * translation unit, as a C file of Nano's would include them. */
#define _LARGEFILE64_SOURCE 1   /* off64_t, which Bionic always has */
#include <aaudio/AAudio.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/hardware_buffer.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/performance_hint.h>
#include <android/sensor.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <nanostubs/stub.h>

int nanostubs_c_headers_compile(void) {
    ASensorEvent ev;
    ev.acceleration.x = 0.0f;
    AMediaCodecBufferInfo info;
    info.size = 0;
    AHardwareBuffer_Desc d;
    d.width = 1;
    return (int)ev.acceleration.x + info.size + (int)d.width + (AMEDIA_OK == 0 ? 0 : 1);
}
