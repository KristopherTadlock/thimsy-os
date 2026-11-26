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

// NanoPlatform.cpp: install() and get(). The aborting cases run in forked children.

#include <signal.h>
#include <sys/wait.h>

#include <thread>

#include <gtest/gtest.h>

#include "NanoPlatform.h"
#include "headless_test_util.h"

namespace {

struct TestPlatform : nanoplat::Platform {
    int tag = 0;
};

bool abortedIn(const std::function<void()>& fn) {
    const int status = headless_test::inChild([&] {
        // Keep the expected abort quiet: ASan and UBSan stay out of it, and stderr goes nowhere.
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        fn();
    });
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

TEST(NanoPlatform, GetBeforeInstallAborts) {
    EXPECT_TRUE(abortedIn([] { (void)nanoplat::get(); }));
}

TEST(NanoPlatform, InstallNullAborts) {
    EXPECT_TRUE(abortedIn([] { nanoplat::install(nullptr); }));
}

TEST(NanoPlatform, InstallThenGet) {
    const int status = headless_test::inChild([] {
        std::unique_ptr<TestPlatform> p(new TestPlatform);
        p->tag = 42;
        TestPlatform* raw = p.get();
        nanoplat::install(std::move(p));
        nanoplat::Platform& got = nanoplat::get();
        if (&got != raw || static_cast<TestPlatform&>(got).tag != 42) _exit(1);
        // The same object from another thread.
        nanoplat::Platform* other = nullptr;
        std::thread([&] { other = &nanoplat::get(); }).join();
        _exit(other == raw ? 0 : 2);
    });
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(NanoPlatform, SecondInstallAborts) {
    EXPECT_TRUE(abortedIn([] {
        nanoplat::install(std::unique_ptr<nanoplat::Platform>(new TestPlatform));
        nanoplat::install(std::unique_ptr<nanoplat::Platform>(new TestPlatform));
    }));
}

}  // namespace
