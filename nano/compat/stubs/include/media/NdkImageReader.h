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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkImageReader.h>: no reader can
// be created (AImageReader_new fails with a null reader).
#pragma once

#include <stdint.h>

#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkMediaError.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AImageReader;
typedef struct AImageReader AImageReader;

media_status_t AImageReader_new(int32_t width, int32_t height, int32_t format, int32_t maxImages,
                                AImageReader** reader);
void AImageReader_delete(AImageReader* reader);
media_status_t AImageReader_getWindow(AImageReader* reader, ANativeWindow** window);
media_status_t AImageReader_acquireLatestImage(AImageReader* reader, AImage** image);

#ifdef __cplusplus
}
#endif
