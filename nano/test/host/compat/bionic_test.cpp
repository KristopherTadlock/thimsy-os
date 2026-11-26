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

// nanocompat/bionic.h, force-included into this file by compat.mk as into every Linux and host
// translation unit: strlcpy, strlcat, getprogname, gettid and the mallopt parameters.

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <string>

#include <gtest/gtest.h>

extern "C" {
int bionicC_purgeAll(void);
int bionicC_cacheCountMax(void);
int bionicC_mallopt(void);
int headersC_values(void);
int headersC_log(void);
}

#ifndef NANOCOMPAT_BIONIC_H
#error "nanocompat/bionic.h was not force-included"
#endif

TEST(Bionic, Strlcpy) {
    char buf[8];
    memset(buf, 'x', sizeof buf);
    EXPECT_EQ(strlcpy(buf, "abc", sizeof buf), 3u);
    EXPECT_STREQ(buf, "abc");
    EXPECT_EQ(strlcpy(buf, "abcdefghijkl", sizeof buf), 12u);  // cut, and says so
    EXPECT_STREQ(buf, "abcdefg");
    memset(buf, 'x', sizeof buf);
    EXPECT_EQ(strlcpy(buf, "abc", 0), 3u);  // writes nothing
    EXPECT_EQ(buf[0], 'x');
    EXPECT_EQ(strlcpy(buf, "", sizeof buf), 0u);
    EXPECT_STREQ(buf, "");
}

TEST(Bionic, Strlcat) {
    char buf[8] = "ab";
    EXPECT_EQ(strlcat(buf, "cd", sizeof buf), 4u);
    EXPECT_STREQ(buf, "abcd");
    EXPECT_EQ(strlcat(buf, "efghij", sizeof buf), 10u);
    EXPECT_STREQ(buf, "abcdefg");
    char full[4] = {'a', 'b', 'c', 'd'};  // not terminated within size
    EXPECT_EQ(strlcat(full, "xy", sizeof full), 6u);
    EXPECT_EQ(full[3], 'd');
}

TEST(Bionic, Getprogname) {
    const char* name = getprogname();
    ASSERT_TRUE(name != nullptr);
    EXPECT_EQ(std::string(name), "bionic_test");
}

TEST(Bionic, Gettid) {
    EXPECT_EQ(gettid(), static_cast<pid_t>(syscall(SYS_gettid)));
    EXPECT_EQ(gettid(), getpid());  // the main thread
}

TEST(Bionic, MalloptParametersAreHarmless) {
    EXPECT_LT(M_PURGE_ALL, -8);  // outside glibc's own parameters
    EXPECT_LT(M_CACHE_COUNT_MAX, -8);
    EXPECT_NE(M_PURGE_ALL, M_CACHE_COUNT_MAX);
    EXPECT_NE(M_PURGE, M_PURGE_ALL);
    // NanoMenu.cpp's calls: no-ops on glibc, musl and ASan (whose results differ; Nano ignores them).
    (void)mallopt(M_PURGE_ALL, 0);
    (void)mallopt(M_CACHE_COUNT_MAX, 0);
    void* p = malloc(1000);
    EXPECT_TRUE(p != nullptr);
    free(p);
}

TEST(Bionic, TheCTranslationUnitAgrees) {
    EXPECT_EQ(bionicC_purgeAll(), M_PURGE_ALL);
    EXPECT_EQ(bionicC_cacheCountMax(), M_CACHE_COUNT_MAX);
    EXPECT_EQ(bionicC_mallopt(), 1);
}

TEST(Bionic, ShimHeadersCompileAsC) {
    EXPECT_EQ(headersC_values(), 1);
    EXPECT_EQ(headersC_log(), 1);
}
