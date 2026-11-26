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

// The shell gate (ShellGate.h). This program links with linker-wraps.txt's --wrap flags, so its own
// system(), popen(), pclose() and exec*() calls go through the gate, as Nano's do.

#include "ShellGate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "compat_test_util.h"

using namespace nanolinux::shellgate;

namespace {

class ShellGateTest : public ::testing::Test {
 protected:
    void SetUp() override {
        resetForTests();
        Routes r;
        r.setProperty = [this](const std::string& k, const std::string& v) {
            props[k] = v;
            sets.push_back(k + "=" + v);
            return k != "fail.key";
        };
        r.getProperty = [this](const std::string& k, const std::string& def) {
            auto it = props.find(k);
            return it == props.end() ? def : it->second;
        };
        setRoutes(r);
    }
    void TearDown() override { resetForTests(); }

    std::map<std::string, std::string> props;
    std::vector<std::string> sets;
};

std::string readAll(FILE* f) {
    std::string s;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    return s;
}

// Runs fn in a forked child and returns its exit status (or -1).
template <typename F>
int inChild(F fn) {
    fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(99);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

}  // namespace

// ------------------------------------------------------------------------------- classification

TEST_F(ShellGateTest, PlainLinuxCommandsPass) {
    for (const char* c : {"sync", "echo 1 > /sys/class/backlight/x/brightness", "mkdir -p /data/x && chmod 755 /data/x",
                          "curl -sI https://example.org", "/usr/bin/true", "cat /proc/cpuinfo | grep Hardware"}) {
        Decision d = classifyCommand(c);
        EXPECT_EQ(d.verdict, Verdict::Pass) << c;
        EXPECT_EQ(d.command, std::string(c)) << c;
    }
}

TEST_F(ShellGateTest, AndroidToolsAreRefused) {
    struct {
        const char* cmd;
        const char* tool;
    } cases[] = {
        {"am start -n com.x/.Main", "am"},
        {"settings put system screen_brightness 40 2>/dev/null", "settings"},
        {"cmd wifi forget-network 3 2>&1", "cmd"},
        {"dumpsys activity services x 2>/dev/null", "dumpsys"},
        {"/system/bin/dumpsys audio", "dumpsys"},
        {"monkey -p com.x -c android.intent.category.LAUNCHER 1", "monkey"},
        {"mkdir -p /data/x && am broadcast -a y", "am"},
        {"FOO=1 nice -n 5 am start x", "am"},
        {"timeout 5 pm list packages", "pm"},
        {"exec input keyevent 26", "input"},
        {"sh -c 'cmd wifi status'", "cmd"},
        {"/system/bin/sh -c \"settings get global x\"", "settings"},
        {"echo $(getprop ro.x) > /tmp/y", "getprop"},
        {"gammaos-net bt scan", "gammaos-net"},
        {"svc wifi enable", "svc wifi"},
        {"getprop", "getprop"},
    };
    for (const auto& c : cases) {
        Decision d = classifyCommand(c.cmd);
        EXPECT_EQ(d.verdict, Verdict::Refuse) << c.cmd;
        EXPECT_EQ(d.tool, std::string(c.tool)) << c.cmd;
    }
}

TEST_F(ShellGateTest, QuotedWordsAreNotCommands) {
    Decision d = classifyCommand("echo 'am start' \"cmd wifi\" > /dev/null; printf '%s\\n' settings");
    EXPECT_EQ(d.verdict, Verdict::Pass);
    ASSERT_EQ(d.commands.size(), 2u);
    EXPECT_EQ(d.commands[0].words[1], std::string("am start"));
}

TEST_F(ShellGateTest, SystemBinPosixToolsAreRewritten) {
    Decision d = classifyCommand("/system/bin/rm -rf '/data/x y' && /system/xbin/sync");
    EXPECT_EQ(d.verdict, Verdict::Pass);
    EXPECT_EQ(d.command, std::string("rm -rf '/data/x y' && sync"));
    d = classifyCommand("toybox chmod 755 /data/x");
    EXPECT_EQ(d.verdict, Verdict::Pass);
    EXPECT_EQ(d.command, std::string(" chmod 755 /data/x"));
}

TEST_F(ShellGateTest, AndroidPathsPassOnlyWhenTheImageHasThem) {
    Decision d = classifyCommand("/vendor/bin/setclock_max.sh &");
    EXPECT_EQ(d.verdict, Verdict::Refuse);
    EXPECT_EQ(d.tool, std::string("/vendor/bin/setclock_max.sh"));

    compat_test::ScratchDir root;
    ASSERT_FALSE(root.path().empty());
    ASSERT_EQ(mkdir((root / "vendor").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((root / "vendor/bin").c_str(), 0755), 0);
    std::string script = root / "vendor/bin/setclock_max.sh";
    int fd = open(script.c_str(), O_WRONLY | O_CREAT, 0755);
    ASSERT_GE(fd, 0);
    close(fd);
    setRoot(root.path().c_str());
    d = classifyCommand("/vendor/bin/setclock_max.sh &");
    EXPECT_EQ(d.verdict, Verdict::Pass);
    setRoot("");
}

TEST_F(ShellGateTest, RoutesAreRecognised) {
    Decision d = classifyCommand("setprop persist.sys.timezone Europe/Berlin");
    EXPECT_EQ(d.verdict, Verdict::Route);
    EXPECT_EQ(d.tool, std::string("setprop"));
    d = classifyCommand("setprop a 1; setprop b 2");
    EXPECT_EQ(d.verdict, Verdict::Route);
    EXPECT_EQ(d.commands.size(), 2u);
    EXPECT_EQ(classifyCommand("svc power reboot").verdict, Verdict::Route);
    EXPECT_EQ(classifyCommand("restorecon -R /data/x").verdict, Verdict::Route);
    // Mixed with Linux commands, piped or in a subshell: not taken apart.
    EXPECT_EQ(classifyCommand("setprop a 1; sync").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("setprop a 1 | cat").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("(setprop a 1)").verdict, Verdict::Refuse);
}

// ------------------------------------------------------------------------------ system and popen

TEST_F(ShellGateTest, SystemRunsLinuxCommands) {
    int st = system("exit 3");
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 3);
    EXPECT_NE(system(nullptr), 0);
}

TEST_F(ShellGateTest, SystemRefusesOncePerTool) {
    int st = system("am force-stop com.x 2>/dev/null");
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 127);
    st = system("am start -n com.x/.Main");
    EXPECT_EQ(WEXITSTATUS(st), 127);
    st = system("cmd alarm set-time 1 2>/dev/null");
    EXPECT_EQ(WEXITSTATUS(st), 127);
    std::vector<std::string> want = {"am", "cmd"};
    EXPECT_EQ(refusedTools(), want);
}

TEST_F(ShellGateTest, SystemRoutesSetprop) {
    int st = system("setprop persist.sys.locale de-DE; setprop sys.x 'a b'");
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 0);
    EXPECT_EQ(props["persist.sys.locale"], std::string("de-DE"));
    EXPECT_EQ(props["sys.x"], std::string("a b"));
    st = system("setprop fail.key 1");
    EXPECT_EQ(WEXITSTATUS(st), 1);
}

TEST_F(ShellGateTest, PowerCommandsGoThroughPowerctl) {
    EXPECT_EQ(WEXITSTATUS(system("reboot")), 0);
    EXPECT_EQ(props["sys.powerctl"], std::string("reboot"));
    system("reboot -p");
    EXPECT_EQ(props["sys.powerctl"], std::string("shutdown"));
    system("svc power reboot recovery");
    EXPECT_EQ(props["sys.powerctl"], std::string("reboot,recovery"));
    system("svc power shutdown");
    EXPECT_EQ(props["sys.powerctl"], std::string("shutdown"));
    EXPECT_EQ(WEXITSTATUS(system("restorecon -R /data/x")), 0);
}

TEST_F(ShellGateTest, PopenPassesAndRoutes) {
    FILE* f = popen("echo hello", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string("hello\n"));
    EXPECT_EQ(pclose(f), 0);

    props["ro.product.model"] = "RG ARC-D";
    f = popen("getprop ro.product.model", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string("RG ARC-D\n"));
    EXPECT_EQ(pclose(f), 0);

    f = popen("getprop missing.key fallback", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string("fallback\n"));
    pclose(f);
}

TEST_F(ShellGateTest, PopenRefusedGivesNoOutputAnd127) {
    FILE* f = popen("/system/bin/dumpsys media.audio_flinger", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string());
    int st = pclose(f);
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 127);

    f = popen("am broadcast -a x", "w");
    ASSERT_TRUE(f != nullptr);
    EXPECT_GT(fputs("ignored\n", f), -1);
    EXPECT_EQ(WEXITSTATUS(pclose(f)), 127);
}

TEST_F(ShellGateTest, RecorderRunsNothing) {
    std::vector<std::string> seen;
    setRecorder([&](const Request& q) {
        seen.push_back(q.command);
        Result r;
        r.exitStatus = 5;
        r.output = "scripted";
        return r;
    });
    EXPECT_EQ(WEXITSTATUS(system("touch /nonexistent/should-not-run")), 5);
    FILE* f = popen("settings get global x", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string("scripted"));
    EXPECT_EQ(WEXITSTATUS(pclose(f)), 5);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], std::string("touch /nonexistent/should-not-run"));
    EXPECT_TRUE(refusedTools().empty());
}

// ------------------------------------------------------------------------------------------ exec

TEST_F(ShellGateTest, ExecPlans) {
    ExecPlan p;
    char* curlArgv[] = {(char*)"curl", (char*)"-s", nullptr};
    classifyExec("curl", curlArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Pass);
    classifyExec("/usr/bin/unzip", curlArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Pass);

    char* rmArgv[] = {(char*)"/system/bin/rm", (char*)"-rf", (char*)"/x", nullptr};
    classifyExec(rmArgv[0], rmArgv, &p);
    EXPECT_EQ(p.action, ExecAction::PathSearch);
    EXPECT_STREQ(p.file, "rm");
    EXPECT_TRUE(p.argv == rmArgv);

    char* amArgv[] = {(char*)"/system/bin/am", (char*)"force-stop", (char*)"x", nullptr};
    classifyExec(amArgv[0], amArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Refuse);
    EXPECT_STREQ(p.tool, "/system/bin/am");

    char* rcArgv[] = {(char*)"/system/bin/restorecon", (char*)"-R", (char*)"/x", nullptr};
    classifyExec(rcArgv[0], rcArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Succeed);

    char* dlArgv[] = {(char*)"/system/bin/gammaos-net", (char*)"download", (char*)"https://h/t.zip",
                      (char*)"/data/local/tmp/t.zip", nullptr};
    classifyExec(dlArgv[0], dlArgv, &p);
    ASSERT_EQ(p.action, ExecAction::Rewrite);
    EXPECT_STREQ(p.file, "curl");
    std::vector<std::string> got;
    for (size_t i = 0; p.argv[i]; i++) got.push_back(p.argv[i]);
    std::vector<std::string> want = {"curl", "-fsSL", "--retry", "2", "-o", "/data/local/tmp/t.zip", "https://h/t.zip"};
    EXPECT_EQ(got, want);

    char* nsArgv[] = {(char*)"/system/bin/nsenter", (char*)"-t", (char*)"1", (char*)"-m", (char*)"--",
                      (char*)"/system/bin/gammaos-net", (char*)"download", (char*)"u", (char*)"f", nullptr};
    classifyExec(nsArgv[0], nsArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Rewrite);
    char* nsNoTail[] = {(char*)"/system/bin/nsenter", (char*)"-t", (char*)"1", nullptr};
    classifyExec(nsNoTail[0], nsNoTail, &p);
    EXPECT_EQ(p.action, ExecAction::Refuse);
}

TEST_F(ShellGateTest, ExecRunsTranslatedAndRefusesAndroid) {
    // Refused: exec fails with ENOENT and the child carries on (Nano's sites then _exit(127)).
    int rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/am", (char*)"start", nullptr};
        execv(argv[0], argv);
        _exit(errno == ENOENT ? 42 : 1);
    });
    EXPECT_EQ(rc, 42);
    // /system/bin/<posix tool> runs the Linux tool.
    rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/sh", (char*)"-c", (char*)"exit 7", nullptr};
        execv(argv[0], argv);
        _exit(1);
    });
    EXPECT_EQ(rc, 7);
    // nsenter's prefix is dropped.
    rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/nsenter", (char*)"-t", (char*)"1", (char*)"-m", (char*)"--",
                        (char*)"/system/bin/sh", (char*)"-c", (char*)"exit 9", nullptr};
        execvp(argv[0], argv);
        _exit(1);
    });
    EXPECT_EQ(rc, 9);
    // restorecon succeeds without running anything.
    rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/restorecon", (char*)"-R", (char*)"/x", nullptr};
        char* envp[] = {nullptr};
        execve(argv[0], argv, envp);
        _exit(1);
    });
    EXPECT_EQ(rc, 0);
    // A Linux program runs as asked.
    rc = inChild([] {
        char* argv[] = {(char*)"sh", (char*)"-c", (char*)"exit 4", nullptr};
        execvp(argv[0], argv);
        _exit(1);
    });
    EXPECT_EQ(rc, 4);
}
