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

/*
 * Thimsy's <android/log.h> for the Linux and host builds of Nano: the NDK logging API with
 * Android's names, priorities and signatures, written to stderr (and optionally a ring file) by
 * nano/compat/src/log.cpp. See nanocompat/nanocompat.h for the line format and the controls.
 */
#ifndef NANOCOMPAT_ANDROID_LOG_H
#define NANOCOMPAT_ANDROID_LOG_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
} android_LogPriority;

typedef enum log_id {
    LOG_ID_MIN = 0,
    LOG_ID_MAIN = 0,
    LOG_ID_RADIO = 1,
    LOG_ID_EVENTS = 2,
    LOG_ID_SYSTEM = 3,
    LOG_ID_CRASH = 4,
    LOG_ID_STATS = 5,
    LOG_ID_SECURITY = 6,
    LOG_ID_KERNEL = 7,
    LOG_ID_MAX,
    LOG_ID_DEFAULT = 0x7FFFFFFF
} log_id_t;

#if defined(__GNUC__)
#define NANOCOMPAT_PRINTF(fmt, args) __attribute__((__format__(__printf__, fmt, args)))
#define NANOCOMPAT_NORETURN __attribute__((__noreturn__))
#else
#define NANOCOMPAT_PRINTF(fmt, args)
#define NANOCOMPAT_NORETURN
#endif

int __android_log_write(int prio, const char* tag, const char* text);
int __android_log_print(int prio, const char* tag, const char* fmt, ...) NANOCOMPAT_PRINTF(3, 4);
int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap)
    NANOCOMPAT_PRINTF(3, 0);
int __android_log_buf_write(int bufID, int prio, const char* tag, const char* text);
int __android_log_buf_print(int bufID, int prio, const char* tag, const char* fmt, ...)
    NANOCOMPAT_PRINTF(4, 5);
/* Logs "cond" and the message at ANDROID_LOG_FATAL, then aborts. */
void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...)
    NANOCOMPAT_PRINTF(3, 4) NANOCOMPAT_NORETURN;
/* Whether a message of this priority is written (the tag is not consulted). */
int __android_log_is_loggable(int prio, const char* tag, int default_prio);

/*
 * What LOG_ALWAYS_FATAL and its kind call: like __android_log_assert, but fmt may be "" (then the
 * condition alone is logged), so the macros work with or without a message. Not an Android API.
 */
void nanocompat_log_fatal(const char* cond, const char* tag, const char* fmt, ...) NANOCOMPAT_NORETURN;

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_ANDROID_LOG_H */
