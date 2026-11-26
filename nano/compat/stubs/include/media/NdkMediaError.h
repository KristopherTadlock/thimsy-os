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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkMediaError.h>: the status type
// and the two values Nano tests for. Every failing media stub returns AMEDIA_ERROR_UNKNOWN.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AMEDIA_OK = 0,
    AMEDIA_ERROR_UNKNOWN = -10000,
} media_status_t;

#ifdef __cplusplus
}
#endif
