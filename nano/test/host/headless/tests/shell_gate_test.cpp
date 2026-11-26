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

// The record-only shell gate through the real call path: linked with nano-headless's
// -Wl,--wrap=system,--wrap=popen,--wrap=pclose,--wrap=execv,--wrap=execvp,--wrap=execve.

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/HostControl.h"
#include "platform/host/ShellGateHost.h"

using namespace nanohost;
using headless_test::TmpDir;

namespace {

struct ShellGateTest : public ::testing::Test {
    TmpDir dir;
    std::string events;
    void SetUp() override {
        resetForTest();
        events = dir / "events.jsonl";
        ASSERT_TRUE(openEvents(events));
        shell::Rule am;
        am.match = "am start";
        am.status = 0;
        shell::Rule getprop;
        getprop.match = "getprop ro.product.model";
        getprop.exact = true;
        getprop.status = 0;
        getprop.out = "Thimsy Host\n";
        shell::Rule failing;
        failing.match = "/system/bin/nsenter";
        failing.status = 3;
        shell::setRules({am, getprop, failing}, 127);
    }
    void TearDown() override {
        shell::setRules({}, 127);
        resetForTest();
    }
    std::string log() { return headless_test::readFile(events); }
};

TEST_F(ShellGateTest, SystemIsRecordedNotRun) {
    const std::string marker = dir / "must-not-exist";
    EXPECT_EQ(system(("touch " + marker).c_str()), 127 << 8);  // no rule: 127, and nothing ran
    EXPECT_NE(access(marker.c_str(), F_OK), 0);
    EXPECT_EQ(system("am start -n com.example/.Main"), 0);
    EXPECT_NE(system(nullptr), 0);
    const std::string l = log();
    EXPECT_NE(l.find("\"fn\":\"system\",\"cmd\":\"touch "), std::string::npos);
    EXPECT_NE(l.find("\"rule\":-1,\"status\":127"), std::string::npos);
    EXPECT_NE(l.find("\"cmd\":\"am start -n com.example/.Main\",\"rule\":0,\"status\":0"), std::string::npos);
    EXPECT_EQ(shell::match("am start x"), 0);
    EXPECT_EQ(shell::match("getprop ro.product.model "), -1);  // exact rule
    EXPECT_EQ(shell::match("getprop ro.product.model"), 1);
}

TEST_F(ShellGateTest, PopenReadsTheRulesOutput) {
    FILE* f = popen("getprop ro.product.model", "r");
    ASSERT_TRUE(f != nullptr);
    char buf[64] = {};
    ASSERT_TRUE(fgets(buf, sizeof(buf), f) != nullptr);
    EXPECT_STREQ(buf, "Thimsy Host\n");
    EXPECT_TRUE(fgets(buf, sizeof(buf), f) == nullptr);  // then end of file
    EXPECT_EQ(pclose(f), 0);

    FILE* g = popen("dumpsys battery", "r");
    ASSERT_TRUE(g != nullptr);
    EXPECT_EQ(fgetc(g), EOF);
    EXPECT_EQ(pclose(g), 127 << 8);

    FILE* w = popen("/system/bin/nsenter -t 1", "w");
    ASSERT_TRUE(w != nullptr);
    EXPECT_GT(fputs("ignored\n", w), 0);
    EXPECT_EQ(pclose(w), 3 << 8);
    EXPECT_NE(log().find("\"fn\":\"popen\",\"cmd\":\"dumpsys battery\""), std::string::npos);
}

TEST_F(ShellGateTest, ExecInAChildExitsWithTheRulesStatus) {
    pid_t pid = fork();
    if (pid == 0) {
        char* const argv[] = {(char*)"nsenter", (char*)"-t", (char*)"1", nullptr};
        execv("/system/bin/nsenter", argv);
        _exit(99);  // not reached: the wrapper exits
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 3);

    pid = fork();
    if (pid == 0) {
        char* const argv[] = {(char*)"curl", (char*)"-s", (char*)"https://example.invalid/\"q\"", nullptr};
        execvp("curl", argv);
        _exit(99);
    }
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 127);

    const std::string l = log();
    EXPECT_NE(l.find("\"fn\":\"execv\",\"cmd\":\"/system/bin/nsenter -t 1\",\"rule\":2,\"status\":3"),
              std::string::npos);
    EXPECT_NE(l.find("\"fn\":\"execvp\",\"cmd\":\"curl -s https://example.invalid/\\\"q\\\"\",\"rule\":-1"),
              std::string::npos);
}

}  // namespace
