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

// log/log.h, utils/Log.h and android/log.h: the macros (LOG_NDEBUG, _IF, SLOG, fatal), the line
// format, the ring file and the priority filter.

#define LOG_TAG "LogTest"

#include <utils/Log.h>

#include <android/log.h>
#include <log/log.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "compat_test_util.h"

int logNdebug_on();
int logNdebug_off();
int logNdebug_default();
void logVerbose_on(int* evaluated);
void logVerbose_off(int* evaluated);
void logVerbose_default(int* evaluated);

namespace {

struct Line {
    int prio;
    std::string tag, msg;
};

class Captured : public ::testing::Test {
 protected:
    void SetUp() override {
        nanocompat::setLogMinPriority(ANDROID_LOG_VERBOSE);
        nanocompat::setLogSink([this](int prio, const char* tag, const char* msg) {
            lines_.push_back(Line{prio, tag, msg});
        });
    }
    void TearDown() override {
        nanocompat::setLogSink(nullptr);
        nanocompat::setLogFile("");
        nanocompat::setLogMinPriority(ANDROID_LOG_VERBOSE);
    }
    std::vector<Line> lines_;
};

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// "<digits> <letter> <rest>"
bool hasPrefix(const std::string& line, char letter, const std::string& rest) {
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') i++;
    if (i == 0 || i + 3 > line.size() || line[i] != ' ' || line[i + 1] != letter || line[i + 2] != ' ') return false;
    return line.substr(i + 3) == rest;
}

// Runs fn in a child with stderr captured; returns the child's wait status and its stderr.
template <class Fn>
int runChild(Fn fn, std::string* err) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        nanocompat::setLogSink(nullptr);
        fn();
        _exit(0);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) err->append(buf, size_t(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

int sideEffects = 0;
int bump() { return ++sideEffects; }

}  // namespace

TEST_F(Captured, PriorityMacrosUseTheTag) {
    ALOGD("d %d", 1);
    ALOGI("i %s", "two");
    ALOGW("w");
    ALOGE("e %c", '!');
    ASSERT_EQ(lines_.size(), 4u);
    EXPECT_EQ(lines_[0].prio, ANDROID_LOG_DEBUG);
    EXPECT_EQ(lines_[0].tag, "LogTest");
    EXPECT_EQ(lines_[0].msg, "d 1");
    EXPECT_EQ(lines_[1].prio, ANDROID_LOG_INFO);
    EXPECT_EQ(lines_[1].msg, "i two");
    EXPECT_EQ(lines_[2].prio, ANDROID_LOG_WARN);
    EXPECT_EQ(lines_[3].prio, ANDROID_LOG_ERROR);
    EXPECT_EQ(lines_[3].msg, "e !");
}

TEST_F(Captured, IfFormsEvaluateTheConditionOnce) {
    sideEffects = 0;
    ALOGE_IF(bump() > 100, "not logged %d", bump());
    EXPECT_EQ(sideEffects, 1);  // the arguments of a false _IF are not evaluated
    ALOGW_IF(bump() == 2, "logged %d", 7);
    EXPECT_EQ(sideEffects, 2);
    ALOGI_IF(false, "no");
    ALOGD_IF(true, "yes");
    ASSERT_EQ(lines_.size(), 2u);
    EXPECT_EQ(lines_[0].msg, "logged 7");
    EXPECT_EQ(lines_[1].msg, "yes");
}

TEST_F(Captured, SystemLogMacros) {
    SLOGE_IF(true, "slog %d", 1);  // NanoMenu.cpp uses SLOGE_IF
    SLOGE_IF(false, "never");
    SLOGW("w");
    SLOGI("i");
    SLOGD("d");
    SLOGE("e");
    ASSERT_EQ(lines_.size(), 5u);
    EXPECT_EQ(lines_[0].prio, ANDROID_LOG_ERROR);
    EXPECT_EQ(lines_[0].msg, "slog 1");
    EXPECT_EQ(lines_[0].tag, "LogTest");
}

TEST_F(Captured, NdkCalls) {
    __android_log_print(ANDROID_LOG_INFO, "Ndk", "n=%d", 5);
    __android_log_write(ANDROID_LOG_WARN, "Ndk", "plain %d text");
    __android_log_buf_print(LOG_ID_SYSTEM, ANDROID_LOG_ERROR, "Ndk", "buf %s", "x");
    __android_log_buf_write(LOG_ID_MAIN, ANDROID_LOG_DEBUG, nullptr, nullptr);
    ALOG(LOG_INFO, "Alog", "alog %d", 3);
    LOG_PRI(ANDROID_LOG_ERROR, "Pri", "pri");
    ASSERT_EQ(lines_.size(), 6u);
    EXPECT_EQ(lines_[0].msg, "n=5");
    EXPECT_EQ(lines_[1].msg, "plain %d text");
    EXPECT_EQ(lines_[2].msg, "buf x");
    EXPECT_EQ(lines_[3].tag, "");
    EXPECT_EQ(lines_[4].tag, "Alog");
    EXPECT_EQ(lines_[4].prio, ANDROID_LOG_INFO);
    EXPECT_EQ(lines_[5].tag, "Pri");
}

TEST_F(Captured, LongMessagesAreWhole) {
    const std::string big(5000, 'q');
    ALOGI("%s|", big.c_str());
    ASSERT_EQ(lines_.size(), 1u);
    EXPECT_EQ(lines_[0].msg, big + "|");
}

TEST_F(Captured, LogNdebugAsOnAndroid) {
    EXPECT_EQ(LOG_NDEBUG, 0);  // this file: no NDEBUG
    EXPECT_EQ(logNdebug_on(), 0);
    EXPECT_EQ(logNdebug_off(), 1);
    EXPECT_EQ(logNdebug_default(), 1);  // NDEBUG and no LOG_NDEBUG

    int evaluated = 0;
    logVerbose_on(&evaluated);
    EXPECT_EQ(evaluated, 4);
    ASSERT_EQ(lines_.size(), 4u);
    EXPECT_EQ(lines_[0].prio, ANDROID_LOG_VERBOSE);
    EXPECT_EQ(lines_[0].tag, "VerboseOn");
    EXPECT_EQ(lines_[0].msg, "v 1");
    EXPECT_EQ(lines_[3].msg, "s_if 4");

    lines_.clear();
    evaluated = 0;
    logVerbose_off(&evaluated);
    logVerbose_default(&evaluated);
    EXPECT_EQ(evaluated, 0);  // compiled out: not even the arguments run
    EXPECT_TRUE(lines_.empty());
}

TEST_F(Captured, MinPriorityFilters) {
    nanocompat::setLogMinPriority(ANDROID_LOG_WARN);
    ALOGI("dropped");
    ALOGW("kept");
    ASSERT_EQ(lines_.size(), 1u);
    EXPECT_EQ(lines_[0].msg, "kept");
    EXPECT_EQ(__android_log_is_loggable(ANDROID_LOG_INFO, "x", ANDROID_LOG_INFO), 0);
    EXPECT_EQ(__android_log_is_loggable(ANDROID_LOG_ERROR, "x", ANDROID_LOG_INFO), 1);
    EXPECT_FALSE(android_testLog(ANDROID_LOG_DEBUG, "x"));
}

TEST_F(Captured, LineFormatAndMultiLineMessages) {
    compat_test::ScratchDir dir;
    const std::string path = dir / "nano.log";
    nanocompat::setLogFile(path);
    ALOGI("one");
    ALOGW("first\nsecond\n");
    __android_log_write(ANDROID_LOG_ERROR, "T", "");
    const std::vector<std::string> got = splitLines(compat_test::readFile(path));
    ASSERT_EQ(got.size(), 4u);
    EXPECT_TRUE(hasPrefix(got[0], 'I', "LogTest: one")) << got[0];
    EXPECT_TRUE(hasPrefix(got[1], 'W', "LogTest: first")) << got[1];
    EXPECT_TRUE(hasPrefix(got[2], 'W', "LogTest: second")) << got[2];
    EXPECT_TRUE(hasPrefix(got[3], 'E', "T: ")) << got[3];
}

TEST_F(Captured, RingFileKeepsTwoHalves) {
    compat_test::ScratchDir dir;
    const std::string path = dir / "ring.log";
    nanocompat::setLogFile(path, 2000);
    for (int i = 0; i < 200; i++) ALOGI("line %03d of the ring test", i);
    struct stat cur, old;
    ASSERT_EQ(stat(path.c_str(), &cur), 0);
    ASSERT_EQ(stat((path + ".1").c_str(), &old), 0);
    EXPECT_LE(cur.st_size, 1000);
    EXPECT_LE(old.st_size, 1000);
    EXPECT_GT(old.st_size, 900);
    const std::string text = compat_test::readFile(path);
    EXPECT_NE(text.find("line 199 of the ring test"), std::string::npos);
}

TEST_F(Captured, StderrGetsOneLinePerLine) {
    std::string err;
    const int status = runChild([] { ALOGW("to %s", "stderr"); }, &err);
    ASSERT_TRUE(WIFEXITED(status));
    const std::vector<std::string> got = splitLines(err);
    ASSERT_EQ(got.size(), 1u) << err;
    EXPECT_TRUE(hasPrefix(got[0], 'W', "LogTest: to stderr")) << err;
}

TEST_F(Captured, AlwaysFatalAborts) {
    std::string err;
    int status = runChild([] { LOG_ALWAYS_FATAL("boom %d", 42); }, &err);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGABRT);
    EXPECT_NE(err.find(" F LogTest: boom 42"), std::string::npos) << err;

    err.clear();
    status = runChild([] { LOG_ALWAYS_FATAL_IF(1 + 1 == 2, "math %s", "works"); }, &err);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_NE(err.find("Assertion failed: 1 + 1 == 2: math works"), std::string::npos) << err;

    err.clear();
    status = runChild([] { LOG_ALWAYS_FATAL_IF(true); }, &err);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_NE(err.find("Assertion failed: true"), std::string::npos) << err;

    err.clear();
    status = runChild([] { __android_log_assert("cond", "Tag", "msg %d", 1); }, &err);
    ASSERT_TRUE(WIFSIGNALED(status));
    EXPECT_NE(err.find("F Tag: Assertion failed: cond: msg 1"), std::string::npos) << err;

    LOG_ALWAYS_FATAL_IF(false, "not fatal");  // and this process goes on
    LOG_FATAL_IF(false);
}
