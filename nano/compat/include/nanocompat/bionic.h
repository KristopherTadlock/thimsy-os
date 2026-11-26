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
 * Bionic-only libc extras that Nano's shared files use, for the Linux and host builds.
 *
 * Force-included (-include nanocompat/bionic.h) into every Linux and host translation unit, so no
 * Nano file is edited (docs/linux/IMPLEMENTATION.md B.2, decision I31). It is never on Android's
 * include path, and it does nothing when it finds Bionic.
 *
 * It stays valid C and C++. In C it only defines constants and includes nothing, because a header
 * included before a C file's own `#define _GNU_SOURCE` (or _POSIX_C_SOURCE) would fix the feature
 * set too early. C++ translation units are safe to include libc headers into: g++ predefines
 * _GNU_SOURCE, and the targets are 64-bit, so _FILE_OFFSET_BITS changes nothing.
 */
#ifndef NANOCOMPAT_BIONIC_H
#define NANOCOMPAT_BIONIC_H

#if !defined(__BIONIC__)

/*
 * Bionic's mallopt() parameters. glibc's and musl's mallopt() (and the sanitizers' replacements)
 * don't know these values and do nothing, which is what Nano needs on Linux, where they are only
 * hints (purge the allocator's caches, cap its cache) and Nano ignores the result. They never
 * collide with glibc's own M_* values (1 to 8 and -1 to -8).
 */
#ifndef M_DECAY_TIME
#define M_DECAY_TIME (-100)
#endif
#ifndef M_PURGE
#define M_PURGE (-101)
#endif
#ifndef M_MEMTAG_TUNING
#define M_MEMTAG_TUNING (-102)
#endif
#ifndef M_THREAD_DISABLE_MEM_INIT
#define M_THREAD_DISABLE_MEM_INIT (-103)
#endif
#ifndef M_PURGE_ALL
#define M_PURGE_ALL (-104)
#endif
#ifndef M_CACHE_COUNT_MAX
#define M_CACHE_COUNT_MAX (-105)
#endif
#ifndef M_CACHE_SIZE_MAX
#define M_CACHE_SIZE_MAX (-106)
#endif
#ifndef M_TSDS_COUNT_MAX
#define M_TSDS_COUNT_MAX (-107)
#endif
#ifndef M_BIONIC_ZERO_INIT
#define M_BIONIC_ZERO_INIT (-108)
#endif
#ifndef M_BIONIC_SET_HEAP_TAGGING_LEVEL
#define M_BIONIC_SET_HEAP_TAGGING_LEVEL (-109)
#endif
#ifndef M_LOG_STATS
#define M_LOG_STATS (-110)
#endif

#if defined(__cplusplus)

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <sys/syscall.h>
#endif

/*
 * strlcpy and strlcat: in glibc only from 2.38 (musl has them). Same contract as Bionic's and the
 * BSDs': copy or append at most size - 1 bytes, always terminate when size > 0, and return the
 * length of the string they tried to make, so a result >= size means it was cut.
 */
#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 38)
static inline size_t strlcpy(char* dst, const char* src, size_t size) {
    const size_t len = strlen(src);
    if (size != 0) {
        const size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

static inline size_t strlcat(char* dst, const char* src, size_t size) {
    const size_t dlen = strnlen(dst, size);
    if (dlen == size) return size + strlen(src);   /* dst not terminated within size: append nothing */
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}
#endif

/* gettid(): in glibc only from 2.30 (musl has it from 1.2.2). */
#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 30)
static inline pid_t gettid(void) { return (pid_t)syscall(SYS_gettid); }
#endif

/*
 * getprogname(): Bionic and the BSDs have it; glibc and musl don't. Both keep the same string as
 * program_invocation_short_name (declared by <errno.h> under _GNU_SOURCE; repeated here so the
 * header doesn't depend on that).
 */
extern "C" char* program_invocation_short_name;
static inline const char* getprogname(void) { return program_invocation_short_name; }

#endif /* __cplusplus */

#endif /* !__BIONIC__ */

#endif /* NANOCOMPAT_BIONIC_H */
