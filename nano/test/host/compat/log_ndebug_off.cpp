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

// log_test's translation unit with LOG_NDEBUG 1: ALOGV, SLOGV and LOG_FATAL_IF compile to nothing.
#define LOG_NDEBUG 1
#define LOG_TAG "VerboseOff"

#include <log/log.h>

int logNdebug_off() { return LOG_NDEBUG; }

// Logs with every verbose form; *evaluated counts the argument evaluations.
void logVerbose_off(int* evaluated) {
    ALOGV("v %d", ++*evaluated);
    ALOGV_IF(true, "v_if %d", ++*evaluated);
    SLOGV("s %d", ++*evaluated);
    SLOGV_IF(true, "s_if %d", ++*evaluated);
    LOG_FATAL_IF(*evaluated > 1000, "never %d", ++*evaluated);
    ALOG_ASSERT(*evaluated < 1000);
    ALOG_ASSERT(*evaluated < 1000, "with a message %d", *evaluated);
}
