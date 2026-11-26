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
 * Thimsy's <log/log.h> for the Linux and host builds of Nano: liblog's macros with Android's
 * behaviour, on top of <android/log.h>.
 *
 *   ALOGV/D/I/W/E(fmt, ...)      the main log; SLOGV/D/I/W/E the system log (the same sink here)
 *   ALOGx_IF(cond, fmt, ...)     log only when cond is true; cond is evaluated once
 *   LOG_ALWAYS_FATAL(fmt, ...)   log at FATAL and abort; LOG_ALWAYS_FATAL_IF(cond, ...)
 *   LOG_FATAL_IF(cond, ...)      as LOG_ALWAYS_FATAL_IF, but only when LOG_NDEBUG is 0
 *   ALOG_ASSERT(cond, ...)       LOG_FATAL_IF(!(cond), ...)
 *   ALOG(LOG_INFO, tag, ...)     LOG_PRI(ANDROID_LOG_INFO, tag, ...); IF_ALOG(LOG_INFO, tag)
 *
 * As on Android, LOG_NDEBUG defaults to 1 when NDEBUG is defined and to 0 otherwise, and is read
 * when this header is first included. With LOG_NDEBUG set, ALOGV and SLOGV compile to nothing (the
 * arguments are still type-checked, never evaluated), and the _IF forms of V and LOG_FATAL_IF
 * vanish. LOG_TAG is read where a macro is used, so it may be defined after the include; without
 * one the tag is empty.
 */
#ifndef NANOCOMPAT_LOG_LOG_H
#define NANOCOMPAT_LOG_LOG_H

#include <android/log.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef LOG_NDEBUG
#ifdef NDEBUG
#define LOG_NDEBUG 1
#else
#define LOG_NDEBUG 0
#endif
#endif

#ifndef LOG_TAG
#define LOG_TAG NULL
#endif

#ifndef __predict_false
#define NANOCOMPAT_PREDICT_FALSE(exp) __builtin_expect((exp) != 0, 0)
#else
#define NANOCOMPAT_PREDICT_FALSE(exp) __predict_false(exp)
#endif

#ifndef CONDITION
#define CONDITION(cond) (NANOCOMPAT_PREDICT_FALSE(cond))
#endif

/* ---- priority-generic forms ---- */

#ifndef LOG_PRI
#define LOG_PRI(priority, tag, ...) __android_log_print(priority, tag, __VA_ARGS__)
#endif
#ifndef LOG_PRI_VA
#define LOG_PRI_VA(priority, tag, fmt, args) __android_log_vprint(priority, tag, fmt, args)
#endif
#ifndef android_printLog
#define android_printLog(prio, tag, ...) __android_log_print(prio, tag, __VA_ARGS__)
#endif
#ifndef android_testLog
#define android_testLog(prio, tag) (__android_log_is_loggable(prio, tag, ANDROID_LOG_VERBOSE) != 0)
#endif
#ifndef ALOG
#define ALOG(priority, tag, ...) LOG_PRI(ANDROID_##priority, tag, __VA_ARGS__)
#endif
#ifndef IF_ALOG
#define IF_ALOG(priority, tag) if (android_testLog(ANDROID_##priority, tag))
#endif

/* ---- the main log ---- */

#ifndef __ALOGV
#define __ALOGV(...) ((void)ALOG(LOG_VERBOSE, LOG_TAG, __VA_ARGS__))
#endif

#ifndef ALOGV
#if LOG_NDEBUG
#define ALOGV(...)                   \
    do {                             \
        if (0) {                     \
            __ALOGV(__VA_ARGS__);    \
        }                            \
    } while (0)
#else
#define ALOGV(...) __ALOGV(__VA_ARGS__)
#endif
#endif

#ifndef ALOGV_IF
#if LOG_NDEBUG
#define ALOGV_IF(cond, ...) ((void)0)
#else
#define ALOGV_IF(cond, ...) \
    ((CONDITION(cond)) ? ((void)ALOG(LOG_VERBOSE, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif
#endif

#ifndef ALOGD
#define ALOGD(...) ((void)ALOG(LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#endif
#ifndef ALOGD_IF
#define ALOGD_IF(cond, ...) \
    ((CONDITION(cond)) ? ((void)ALOG(LOG_DEBUG, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif
#ifndef ALOGI
#define ALOGI(...) ((void)ALOG(LOG_INFO, LOG_TAG, __VA_ARGS__))
#endif
#ifndef ALOGI_IF
#define ALOGI_IF(cond, ...) \
    ((CONDITION(cond)) ? ((void)ALOG(LOG_INFO, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif
#ifndef ALOGW
#define ALOGW(...) ((void)ALOG(LOG_WARN, LOG_TAG, __VA_ARGS__))
#endif
#ifndef ALOGW_IF
#define ALOGW_IF(cond, ...) \
    ((CONDITION(cond)) ? ((void)ALOG(LOG_WARN, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif
#ifndef ALOGE
#define ALOGE(...) ((void)ALOG(LOG_ERROR, LOG_TAG, __VA_ARGS__))
#endif
#ifndef ALOGE_IF
#define ALOGE_IF(cond, ...) \
    ((CONDITION(cond)) ? ((void)ALOG(LOG_ERROR, LOG_TAG, __VA_ARGS__)) : (void)0)
#endif

/* ---- the system log (liblog's log_system.h) ---- */

#ifndef __SLOGV
#define __SLOGV(...) \
    ((void)__android_log_buf_print(LOG_ID_SYSTEM, ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__))
#endif

#ifndef SLOGV
#if LOG_NDEBUG
#define SLOGV(...)                   \
    do {                             \
        if (0) {                     \
            __SLOGV(__VA_ARGS__);    \
        }                            \
    } while (0)
#else
#define SLOGV(...) __SLOGV(__VA_ARGS__)
#endif
#endif

#ifndef SLOGV_IF
#if LOG_NDEBUG
#define SLOGV_IF(cond, ...) ((void)0)
#else
#define SLOGV_IF(cond, ...)                                                                      \
    ((CONDITION(cond))                                                                           \
         ? ((void)__android_log_buf_print(LOG_ID_SYSTEM, ANDROID_LOG_VERBOSE, LOG_TAG,           \
                                          __VA_ARGS__))                                          \
         : (void)0)
#endif
#endif

#define NANOCOMPAT_SLOG(prio, ...) \
    ((void)__android_log_buf_print(LOG_ID_SYSTEM, prio, LOG_TAG, __VA_ARGS__))
#define NANOCOMPAT_SLOG_IF(cond, prio, ...) \
    ((CONDITION(cond)) ? NANOCOMPAT_SLOG(prio, __VA_ARGS__) : (void)0)

#ifndef SLOGD
#define SLOGD(...) NANOCOMPAT_SLOG(ANDROID_LOG_DEBUG, __VA_ARGS__)
#endif
#ifndef SLOGD_IF
#define SLOGD_IF(cond, ...) NANOCOMPAT_SLOG_IF(cond, ANDROID_LOG_DEBUG, __VA_ARGS__)
#endif
#ifndef SLOGI
#define SLOGI(...) NANOCOMPAT_SLOG(ANDROID_LOG_INFO, __VA_ARGS__)
#endif
#ifndef SLOGI_IF
#define SLOGI_IF(cond, ...) NANOCOMPAT_SLOG_IF(cond, ANDROID_LOG_INFO, __VA_ARGS__)
#endif
#ifndef SLOGW
#define SLOGW(...) NANOCOMPAT_SLOG(ANDROID_LOG_WARN, __VA_ARGS__)
#endif
#ifndef SLOGW_IF
#define SLOGW_IF(cond, ...) NANOCOMPAT_SLOG_IF(cond, ANDROID_LOG_WARN, __VA_ARGS__)
#endif
#ifndef SLOGE
#define SLOGE(...) NANOCOMPAT_SLOG(ANDROID_LOG_ERROR, __VA_ARGS__)
#endif
#ifndef SLOGE_IF
#define SLOGE_IF(cond, ...) NANOCOMPAT_SLOG_IF(cond, ANDROID_LOG_ERROR, __VA_ARGS__)
#endif

/* ---- fatal ---- */

/*
 * The message is optional, as on Android: "" is pasted before it, so the format must be a string
 * literal (it is in every Nano call), and with no message only the condition is logged.
 */
#ifndef LOG_ALWAYS_FATAL_IF
#define LOG_ALWAYS_FATAL_IF(cond, ...) \
    ((CONDITION(cond)) ? nanocompat_log_fatal(#cond, LOG_TAG, "" __VA_ARGS__) : (void)0)
#endif
#ifndef LOG_ALWAYS_FATAL
#define LOG_ALWAYS_FATAL(...) nanocompat_log_fatal(NULL, LOG_TAG, "" __VA_ARGS__)
#endif

#ifndef LOG_FATAL_IF
#if LOG_NDEBUG
#define LOG_FATAL_IF(cond, ...) ((void)0)
#else
#define LOG_FATAL_IF(cond, ...) LOG_ALWAYS_FATAL_IF(cond, __VA_ARGS__)
#endif
#endif
#ifndef LOG_FATAL
#if LOG_NDEBUG
#define LOG_FATAL(...) ((void)0)
#else
#define LOG_FATAL(...) LOG_ALWAYS_FATAL(__VA_ARGS__)
#endif
#endif
#ifndef ALOG_ASSERT
#define ALOG_ASSERT(cond, ...) LOG_FATAL_IF(!(cond), __VA_ARGS__)
#endif

#endif /* NANOCOMPAT_LOG_LOG_H */
