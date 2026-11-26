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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <android/performance_hint.h>: there is
// no hint manager, so no session is ever created.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct APerformanceHintManager;
typedef struct APerformanceHintManager APerformanceHintManager;
struct APerformanceHintSession;
typedef struct APerformanceHintSession APerformanceHintSession;

APerformanceHintManager* APerformanceHint_getManager(void);
APerformanceHintSession* APerformanceHint_createSession(APerformanceHintManager* manager,
                                                        const int32_t* threadIds, size_t size,
                                                        int64_t initialTargetWorkDurationNanos);
int APerformanceHint_reportActualWorkDuration(APerformanceHintSession* session,
                                              int64_t actualDurationNanos);

#ifdef __cplusplus
}
#endif
