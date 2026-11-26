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

// log_test's translation unit with NDEBUG and no LOG_NDEBUG: as on Android, LOG_NDEBUG becomes 1.
#ifndef NDEBUG
#define NDEBUG 1
#endif
#define LOG_TAG "VerboseDefault"

#include <log/log.h>

int logNdebug_default() { return LOG_NDEBUG; }

// Logs with every verbose form; *evaluated counts the argument evaluations.
void logVerbose_default(int* evaluated) {
    ALOGV("v %d", ++*evaluated);
    ALOGV_IF(true, "v_if %d", ++*evaluated);
    SLOGV("s %d", ++*evaluated);
    SLOGV_IF(true, "s_if %d", ++*evaluated);
    LOG_FATAL_IF(*evaluated > 1000, "never %d", ++*evaluated);
    ALOG_ASSERT(*evaluated < 1000);
    ALOG_ASSERT(*evaluated < 1000, "with a message %d", *evaluated);
}
