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

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "compat_test_util.h"

extern char** environ;

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

const char* const kTzDate = "TZ=Europe/Berlin date -d @1767225600 +%z 2>/dev/null";

}  // namespace

// ------------------------------------------------------------------------------- the allow-list

TEST_F(ShellGateTest, TheAllowListIsShort) {
    std::vector<std::string> want = {"NanoMenuSetupWizard.cpp:318 tzStandardOffset"};
    EXPECT_EQ(allowListSites(), want);
}

TEST_F(ShellGateTest, AnAllowListedShapePasses) {
    Decision d = classifyCommand(kTzDate);
    EXPECT_EQ(d.verdict, Verdict::Pass);
    EXPECT_EQ(d.command, std::string(kTzDate));
    ASSERT_EQ(d.commands.size(), 1u);
    EXPECT_STREQ(d.commands[0].allow, "NanoMenuSetupWizard.cpp:318 tzStandardOffset");
    EXPECT_EQ(classifyCommand("TZ=America/Los_Angeles date -d @1782864000 +%z").verdict, Verdict::Pass);
    EXPECT_EQ(classifyCommand("TZ=Etc/GMT+5 date -d @0 +%z 2>&1").verdict, Verdict::Pass);
}

TEST_F(ShellGateTest, OnlyTheExactShapePasses) {
    for (const char* c : {
             "TZ=Europe/Berlin date -d @1767225600 +%z -u",           // an extra word
             "TZ=Europe/Berlin date -d @1767225600",                  // a missing word
             "date -d @1767225600 +%z",                               // no TZ
             "TZ=../../etc/passwd date -d @1 +%z",                    // not a zone
             "TZ=Europe/Berlin date -d @12x +%z",                     // not an epoch
             "TZ=Europe/Berlin date -d @1 +%z > /tmp/out",            // a redirection that writes
             "TZ=Europe/Berlin date -d @1 +%z | cat",                 // a pipe
             "TZ=Europe/Berlin date -d @1 +%z &",                     // a background job
             "(TZ=Europe/Berlin date -d @1 +%z)",                     // a subshell
             "nice TZ=Europe/Berlin date -d @1 +%z",                  // a wrapper
             "timeout 3 TZ=Europe/Berlin date -d @1 +%z",
             "TZ=Europe/Berlin /bin/date -d @1 +%z",                  // another spelling of the program
             "TZ=Europe/Berlin date -d @1 +%z; sync",                 // with a command that is not allowed
             "TZ=Europe/Berlin date -d @1 +%z; setprop a 1",          // mixed with a route
         }) {
        EXPECT_EQ(classifyCommand(c).verdict, Verdict::Refuse) << c;
    }
}

// --------------------------------------------------------------------------- refused by default

TEST_F(ShellGateTest, LinuxCommandsAreRefusedByDefault) {
    struct {
        const char* cmd;
        const char* shape;
    } cases[] = {
        {"sync", "sync"},
        {"echo 1 > /sys/class/backlight/x/brightness", "echo"},
        {"mkdir -p /data/x && chmod 755 /data/x", "mkdir"},
        {"curl -sI https://example.org", "curl"},
        {"/usr/bin/true", "/usr/bin/true"},
        {"cat /proc/cpuinfo | grep Hardware", "cat"},
        {"", "(empty)"},
        {"   ", "(empty)"},
        {"X=1", "(assignment only)"},
        {"> /tmp/x", "(no command)"},
    };
    for (const auto& c : cases) {
        Decision d = classifyCommand(c.cmd);
        EXPECT_EQ(d.verdict, Verdict::Refuse) << c.cmd;
        EXPECT_EQ(d.shape, std::string(c.shape)) << c.cmd;
    }
}

TEST_F(ShellGateTest, AndroidToolsAreRefused) {
    struct {
        const char* cmd;
        const char* shape;
    } cases[] = {
        {"am start -n com.x/.Main", "am start"},
        {"settings put system screen_brightness 40 2>/dev/null", "settings put"},
        {"cmd wifi forget-network 3 2>&1", "cmd wifi"},
        {"dumpsys activity services x 2>/dev/null", "dumpsys activity"},
        {"/system/bin/dumpsys audio", "/system/bin/dumpsys audio"},
        {"monkey -p com.x -c android.intent.category.LAUNCHER 1", "monkey"},
        {"mkdir -p /data/x && am broadcast -a y", "mkdir"},
        {"FOO=1 nice -n 5 am start x", "am start"},
        {"timeout 5 pm list packages", "pm list"},
        {"exec input keyevent 26", "input keyevent"},
        {"sh -c 'cmd wifi status'", "sh -c: cmd wifi"},
        {"/system/bin/sh -c \"settings get global x\"", "/system/bin/sh -c: settings get"},
        {"echo $(getprop ro.x) > /tmp/y", "echo"},
        {"D=$(dumpsys activity activities); echo \"$D\" | grep x", "dumpsys activity"},
        {"gammaos-net bt scan", "gammaos-net bt"},
        {"svc wifi enable", "svc wifi"},
        {"getprop", "getprop"},
        {"/vendor/bin/setclock_max.sh &", "/vendor/bin/setclock_max.sh"},
        {"toybox chmod 755 /data/x", "chmod"},
    };
    for (const auto& c : cases) {
        Decision d = classifyCommand(c.cmd);
        EXPECT_EQ(d.verdict, Verdict::Refuse) << c.cmd;
        EXPECT_EQ(d.shape, std::string(c.shape)) << c.cmd;
    }
}

// The Linux programs Nano's shared code used to reach with Android arguments (audit defect 3), and the
// SELinux tools whose "success" was a lie.
TEST_F(ShellGateTest, AndroidShapedLinuxToolsAreRefused) {
    struct {
        const char* cmd;
        const char* shape;
    } cases[] = {
        {"/system/bin/chown -R system:system /data/system/nano_esde_themes/x", "/system/bin/chown"},
        {"chown -R system:system /data/x", "chown"},
        {"/system/bin/pidof org.vita3k.emulator", "/system/bin/pidof"},
        {"pidof com.retroarch.aarch64", "pidof"},
        {"/system/bin/curl -s -L --max-time 15 -A 'gammaos-nano-iptv' 'https://h/x.m3u8' 2>/dev/null",
         "/system/bin/curl"},
        {"/system/bin/rm -rf '/data/x y' && /system/xbin/sync", "/system/bin/rm"},
        {"restorecon -R /data/x", "restorecon"},
        {"restorecon '/data/x' >/dev/null 2>&1", "restorecon"},
        {"chcon u:object_r:system_data_file:s0 /data/x", "chcon"},
        {"gammaos-net download https://h/t.zip /data/local/tmp/t.zip", "gammaos-net download"},
        {"nsenter -t 1 -m -- /system/bin/gammaos-net download u f", "nsenter"},
    };
    for (const auto& c : cases) {
        Decision d = classifyCommand(c.cmd);
        EXPECT_EQ(d.verdict, Verdict::Refuse) << c.cmd;
        EXPECT_EQ(d.shape, std::string(c.shape)) << c.cmd;
    }
}

// Audit defect 2: "timeout -s KILL 10 cmd ..." was read as duration KILL, command "10", and passed.
TEST_F(ShellGateTest, TimeoutOptionsAreSkippedAndTheCommandRefused) {
    struct {
        const char* cmd;
        const char* shape;
    } cases[] = {
        // NanoMenuMonitorMode.cpp:84 and NanoMenuPS3Menu.cpp:2469 (from :2483).
        {"timeout -s KILL 10 cmd display gammaos-monitor-mode list 2>&1", "cmd display"},
        {"timeout -s KILL 3 cmd package resolve-activity --brief -a android.settings.SETTINGS 2>/dev/null",
         "cmd package"},
        {"timeout -sKILL 5 dumpsys window", "dumpsys window"},
        {"timeout -k 2 5 am start x", "am start"},
        {"timeout -k2 -s TERM 5 am start x", "am start"},
        {"timeout --signal=KILL 5 pm list packages", "pm list"},
        {"timeout --signal KILL 5 pm list packages", "pm list"},
        {"timeout --kill-after=1 --preserve-status --foreground 5 settings get global x", "settings get"},
        {"timeout --kill-after 1 -v 5 settings get global x", "settings get"},
        {"timeout -- 5 am force-stop x", "am force-stop"},
        {"timeout 30 gammaos-net wifi configure x", "gammaos-net wifi"},
        {"timeout 5 nice -n 3 am start x", "am start"},
        // An option timeout does not have, or no command: the wrapper itself is what is refused.
        {"timeout -z 5 am start", "timeout"},
        {"timeout 5", "timeout"},
        {"timeout -s KILL", "timeout"},
    };
    for (const auto& c : cases) {
        Decision d = classifyCommand(c.cmd);
        EXPECT_EQ(d.verdict, Verdict::Refuse) << c.cmd;
        EXPECT_EQ(d.shape, std::string(c.shape)) << c.cmd;
    }
}

TEST_F(ShellGateTest, KeywordsAndUnknownWrappersAreRefused) {
    for (const char* c : {
             "if true; then am start x; fi", "! am start x", "{ am start x; }", "do am start x", "then sync",
             "else sync", "su -c 'am start x'", "su 0 am start x", "time am start x", "stdbuf -oL am start x",
             "chrt -f 10 am start x", "taskset 1 am start x", "ionice -c3 am start x", "xargs am start",
             "while true; do sync; done", "time TZ=UTC date -d @1 +%z", "env TZ=UTC date -d @1 +%z",
             "$CMD start", "\"$CMD\" start"}) {
        EXPECT_EQ(classifyCommand(c).verdict, Verdict::Refuse) << c;
    }
    EXPECT_EQ(classifyCommand("su -c 'am start x'").shape, std::string("su"));
    EXPECT_EQ(classifyCommand("! am start x").shape, std::string("! am"));
    EXPECT_EQ(classifyCommand("if true; then am start x; fi").shape, std::string("if true"));
    EXPECT_EQ(classifyCommand("nohup").shape, std::string("nohup"));
}

TEST_F(ShellGateTest, QuotedWordsAreNotCommands) {
    Decision d = classifyCommand("echo 'am start' \"cmd wifi\" > /dev/null; printf '%s\\n' settings");
    EXPECT_EQ(d.verdict, Verdict::Refuse);
    EXPECT_EQ(d.shape, std::string("echo"));
    ASSERT_EQ(d.commands.size(), 2u);
    EXPECT_EQ(d.commands[0].words[1], std::string("am start"));
}

TEST_F(ShellGateTest, ShapesNameTheSubcommandOnly) {
    EXPECT_EQ(shapeOf({"am", "force-stop", "com.x"}), std::string("am force-stop"));
    EXPECT_EQ(shapeOf({"pidof", "org.vita3k.emulator"}), std::string("pidof"));
    EXPECT_EQ(shapeOf({"chown", "-R", "system:system"}), std::string("chown"));
    EXPECT_EQ(shapeOf({"cmd", "bluetooth_manager", "enable"}), std::string("cmd bluetooth_manager"));
    EXPECT_EQ(shapeOf({}), std::string("(no command)"));
}

// ------------------------------------------------------------------------------------- routes

TEST_F(ShellGateTest, RoutesAreRecognised) {
    Decision d = classifyCommand("setprop persist.sys.timezone Europe/Berlin");
    EXPECT_EQ(d.verdict, Verdict::Route);
    EXPECT_EQ(d.shape, std::string("setprop persist.sys.timezone"));
    d = classifyCommand("setprop a 1; setprop b 2");
    EXPECT_EQ(d.verdict, Verdict::Route);
    EXPECT_EQ(d.commands.size(), 2u);
    EXPECT_EQ(classifyCommand("setprop a 1 2>/dev/null").verdict, Verdict::Route);
    EXPECT_EQ(classifyCommand("svc power reboot").verdict, Verdict::Route);
    EXPECT_EQ(classifyCommand("/system/bin/svc power shutdown").verdict, Verdict::Route);
    EXPECT_EQ(classifyCommand("reboot -p").verdict, Verdict::Route);
    EXPECT_EQ(classifyCommand("getprop ro.x").verdict, Verdict::Route);
    // Mixed with other commands, piped, in a subshell, wrapped or redirected to a file: refused.
    EXPECT_EQ(classifyCommand("setprop a 1; sync").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("setprop a 1 | cat").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("(setprop a 1)").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("setprop a 1 &").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("nice setprop a 1").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("X=1 setprop a 1").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("setprop a 1 > /tmp/x").verdict, Verdict::Refuse);
    EXPECT_EQ(classifyCommand("svc power").verdict, Verdict::Refuse);
}

// ------------------------------------------------------------------------------ system and popen

TEST_F(ShellGateTest, SystemRunsTheAllowListedShape) {
    // Any zone and epoch: the host's date and zoneinfo answer.
    int st = system("TZ=UTC date -d @0 +%z >/dev/null");
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 0);
    FILE* f = popen("TZ=UTC date -d @0 +%z", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string("+0000\n"));
    EXPECT_EQ(pclose(f), 0);
    EXPECT_NE(system(nullptr), 0);
    EXPECT_EQ(refusalCount(), 0u);
}

TEST_F(ShellGateTest, SystemRefusesAndCountsEachLogsOncePerShape) {
    int st = system("am force-stop com.x 2>/dev/null");
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 127);
    EXPECT_EQ(WEXITSTATUS(system("am force-stop com.y")), 127);
    EXPECT_EQ(WEXITSTATUS(system("am start -n com.x/.Main")), 127);
    EXPECT_EQ(WEXITSTATUS(system("cmd alarm set-time 1 2>/dev/null")), 127);
    EXPECT_EQ(WEXITSTATUS(system("exit 3")), 127);   // a plain shell command too
    std::vector<std::string> want = {"am force-stop", "am start", "cmd alarm", "exit"};
    EXPECT_EQ(refusedShapes(), want);
    EXPECT_EQ(refusalCount(), 5u);
}

TEST_F(ShellGateTest, SystemRoutesSetprop) {
    int st = system("setprop persist.sys.locale de-DE; setprop sys.x 'a b'");
    ASSERT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 0);
    EXPECT_EQ(props["persist.sys.locale"], std::string("de-DE"));
    EXPECT_EQ(props["sys.x"], std::string("a b"));
    st = system("setprop fail.key 1");
    EXPECT_EQ(WEXITSTATUS(st), 1);
    system("setprop sys.x c");
    std::vector<std::string> want = {"setprop persist.sys.locale", "setprop sys.x", "setprop fail.key"};
    EXPECT_EQ(routedShapes(), want);
    EXPECT_EQ(refusalCount(), 0u);
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
    // The usual Android spelling, by absolute path: still svc, not reboot.
    props["sys.powerctl"] = "";
    EXPECT_EQ(WEXITSTATUS(system("/system/bin/svc power shutdown")), 0);
    EXPECT_EQ(props["sys.powerctl"], std::string("shutdown"));
    system("/system/bin/svc power reboot");
    EXPECT_EQ(props["sys.powerctl"], std::string("reboot"));
    system("/system/bin/reboot -p");
    EXPECT_EQ(props["sys.powerctl"], std::string("shutdown"));
    // restorecon is no longer a successful no-op.
    EXPECT_EQ(WEXITSTATUS(system("restorecon -R /data/x")), 127);
}

TEST_F(ShellGateTest, PopenRoutesGetprop) {
    props["ro.product.model"] = "RG ARC-D";
    FILE* f = popen("getprop ro.product.model", "r");
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

    f = popen("echo hello", "r");
    ASSERT_TRUE(f != nullptr);
    EXPECT_EQ(readAll(f), std::string());
    EXPECT_EQ(WEXITSTATUS(pclose(f)), 127);

    f = popen("am broadcast -a x", "w");
    ASSERT_TRUE(f != nullptr);
    EXPECT_GT(fputs("ignored\n", f), -1);
    EXPECT_EQ(WEXITSTATUS(pclose(f)), 127);
    EXPECT_EQ(refusalCount(), 3u);
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
    EXPECT_TRUE(refusedShapes().empty());
}

// ------------------------------------------------------------------------------------------ exec

TEST_F(ShellGateTest, ExecIsRefusedByDefault) {
    ExecPlan p;
    char* curlArgv[] = {(char*)"curl", (char*)"-s", nullptr};
    classifyExec("curl", curlArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Refuse);
    EXPECT_STREQ(p.shape, "curl");
    classifyExec("/usr/bin/unzip", curlArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Refuse);
    EXPECT_STREQ(p.shape, "/usr/bin/unzip");

    struct {
        std::vector<const char*> argv;
        const char* shape;
    } cases[] = {
        {{"/system/bin/rm", "-rf", "/x"}, "/system/bin/rm"},
        {{"/system/bin/mkdir", "-p", "/data/system/nano_esde_themes"}, "/system/bin/mkdir"},
        {{"/system/bin/am", "force-stop", "x"}, "/system/bin/am force-stop"},
        {{"/system/bin/restorecon", "-R", "/x"}, "/system/bin/restorecon"},
        {{"/system/bin/gammaos-net", "download", "https://h/t.zip", "/data/local/tmp/t.zip"},
         "/system/bin/gammaos-net download"},
        {{"/system/bin/nsenter", "-t", "1", "-m", "--", "/system/bin/gammaos-net", "download", "u", "f"},
         "/system/bin/nsenter"},
        {{"/system/bin/chown", "-R", "system:system", "/data/system/nano_esde_themes/x"}, "/system/bin/chown"},
        {{"/system/bin/pidof", "org.vita3k.emulator"}, "/system/bin/pidof"},
        {{"sh", "-c", "exit 4"}, "sh"},
    };
    for (auto& c : cases) {
        c.argv.push_back(nullptr);
        classifyExec(c.argv[0], const_cast<char* const*>(c.argv.data()), &p);
        EXPECT_EQ(p.action, ExecAction::Refuse) << c.argv[0];
        EXPECT_STREQ(p.shape, c.shape);
    }
    classifyExec(nullptr, curlArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Refuse);
    // The allow-listed shell shape is not an exec entry.
    char* dateArgv[] = {(char*)"TZ=UTC", (char*)"date", (char*)"-d", (char*)"@1", (char*)"+%z", nullptr};
    classifyExec(dateArgv[0], dateArgv, &p);
    EXPECT_EQ(p.action, ExecAction::Refuse);
}

TEST_F(ShellGateTest, ExecRefusalsInChildrenAreCountedInTheParent) {
    // Refused: exec fails with ENOENT and the child carries on (Nano's sites then _exit(127)).
    int rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/am", (char*)"start", nullptr};
        execv(argv[0], argv);
        _exit(errno == ENOENT ? 42 : 1);
    });
    EXPECT_EQ(rc, 42);
    rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/am", (char*)"start", (char*)"-W", nullptr};
        execv(argv[0], argv);
        _exit(errno == ENOENT ? 42 : 1);
    });
    EXPECT_EQ(rc, 42);
    // restorecon no longer "succeeds".
    rc = inChild([] {
        char* argv[] = {(char*)"/system/bin/restorecon", (char*)"-R", (char*)"/x", nullptr};
        char* envp[] = {nullptr};
        execve(argv[0], argv, envp);
        _exit(errno == ENOENT ? 43 : 1);
    });
    EXPECT_EQ(rc, 43);
    // A Linux program is refused too.
    rc = inChild([] {
        char* argv[] = {(char*)"sh", (char*)"-c", (char*)"exit 4", nullptr};
        execvp(argv[0], argv);
        _exit(errno == ENOENT ? 44 : 1);
    });
    EXPECT_EQ(rc, 44);
    EXPECT_EQ(refusalCount(), 4u);
    std::vector<std::string> want = {"/system/bin/am start", "/system/bin/restorecon", "sh"};
    EXPECT_EQ(refusedShapes(), want);
}

TEST_F(ShellGateTest, ThePlatformsOwnExecRuns) {
    int rc = inChild([] {
        char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)"exit 4", nullptr};
        platformExecve(argv[0], argv, environ);
        _exit(1);
    });
    EXPECT_EQ(rc, 4);
    EXPECT_EQ(refusalCount(), 0u);
}

// ------------------------------------------------------------------- the audited command shapes
// Every command Nano's Linux-compiled shared code sends to a shell or exec today
// (docs/linux/audit-2026-11-26/inv_shell.md, with facts.md's corrections: NanoOtaCheck and NanoArchive
// are compiled out on Linux), one representative string per shape. A change to what the gate passes,
// routes or refuses fails here: review it against OA-4, then update the lists.

namespace {

const char* const kSharedShellCommands[] = {
    // Apps and activities.
    "am force-stop com.old; monkey -p com.new -c android.intent.category.LAUNCHER 1",     // ControlCenter:1121
    "am force-stop com.x; ACT=$(cmd package resolve-activity --brief com.x | tail -n 1); am start --display 1 -n $ACT",
    "D=$(dumpsys activity activities); echo \"$D\" | grep -q com.x",                     // ControlCenter:1246
    "dumpsys activity processes com.x",                                                    // Overlay:219
    "dumpsys activity activities | grep ResumedActivity",                                 // Overlay:316
    "am start -n com.x/.NanoPauseActivity",                                                // Overlay:412
    "dumpsys activity services com.x",                                                     // Overlay:1163
    "pm list packages -3",                                                                 // Overlay:1200
    "pm path com.retroarch.aarch64",                                                       // Overlay:1977
    "timeout -s KILL 3 cmd package resolve-activity --brief -a android.settings.SETTINGS 2>/dev/null",  // PS3Menu:2483
    "am start -a android.settings.SETTINGS",                                               // PS3Menu:2554
    "am start-foreground-service -n com.x/.ScreenMapper",                                  // PS3Menu:12839
    "am stopservice com.x/.ScreenMapper",
    "am force-stop com.retroarch.aarch64 >/dev/null 2>&1",                                // PS3Folder:881
    "appops set com.retroarch.aarch64 MANAGE_EXTERNAL_STORAGE allow",                     // PS3Folder:1199
    "am broadcast -a com.x.androidgames.SYNC",                                             // Xmb:3714
    "pm enable com.android.documentsui >/dev/null 2>&1 &",                                // State:393
    // Android settings and system UI.
    "settings get global x",                                                               // SettingsTree:845
    "settings put global x 1",                                                             // SettingsTree:877
    "ime reset",                                                                           // SetupWizard:209
    "locksettings clear --old 1234",                                                       // SetupWizard:212
    "settings put global device_provisioned 1 &",                                          // NanoMenu:4729
    "ime enable com.x/.Ime; ime set com.x/.Ime",                                           // Overlay:1513
    "settings get system font_scale",                                                      // Render:6259
    "cmd uimode night yes",                                                                // PS3Menu:12772
    "cmd notification set_dnd on",                                                         // PS3Menu:12876
    "cmd location set-location-enabled true",
    "cmd connectivity airplane-mode enable",                                               // PS3Menu:12849
    "dumpsys notification --noredact",                                                     // PS3Menu:4979
    // Display.
    "timeout -s KILL 10 cmd display gammaos-monitor-mode list 2>&1",                      // MonitorMode:84
    "cmd display enable-display 2",                                                        // PS3Menu:12392
    "cmd color_display set-saturation 100",                                                // SettingsTree:903
    "screencap -p /data/local/tmp/nano_bg.png",                                            // Render:439
    "mkdir -p /sdcard/Pictures/Screenshots; screencap -p /sdcard/Pictures/Screenshots/x.png",  // ControlCenter:1693
    // Audio.
    "cmd media_session volume --get",                                                      // ControlCenter:282
    // Power, performance and brightness.
    "/vendor/bin/setclock_performance.sh &",                                               // NanoMenu:1242
    "settings put system screen_brightness 50",                                            // ControlCenter:1617
    // Wi-Fi and Bluetooth.
    "timeout 30 gammaos-net wifi configure Home psk x --ip 10.0.0.2",                     // Settings:621
    "cmd wifi set-wifi-enabled disabled",                                                  // SettingsTree:1107
    "cmd bluetooth_manager disable",                                                       // SettingsTree:1108
    "svc wifi enable",                                                                     // ControlCenter:1697
    "gammaos-net bt discoverable 120",                                                     // PS3Menu:16252
    // Time, date and locale.
    "cmd alarm set-timezone Europe/Berlin",                                                // PS3Menu:12223
    "curl -sI --max-time 8 http://example.com 2>/dev/null | grep -i '^date:'",            // PS3Menu:18404
    "cmd alarm set-time 1767225600000 2>/dev/null",                                        // PS3Menu:18419
    "curl -s --max-time 6 https://ipapi.co/timezone/ 2>/dev/null",                        // SetupWizard:404
    "curl -s --max-time 6 'http://ip-api.com/line/?fields=timezone' 2>/dev/null",         // SetupWizard:405
    // Streaming.
    "/system/bin/curl -s -L --max-time 15 -A 'gammaos-nano-iptv' 'https://h/x.m3u8' 2>/dev/null",  // Hls:147
    "/system/bin/curl -s -L --max-time 8 --http0.9 -H 'Icy-MetaData: 0' -r 0-8191 -A 'gammaos-nano-radio' "
    "'http://h/s' 2>/dev/null",                                                            // IcyDemux:44
    "/system/bin/curl -s -L --max-time 20 -A 'gammaos-nano-iptv' -o '/data/system/nano_iptv_ftv.m3u8.tmp' "
    "'https://h/i.m3u' 2>/dev/null",                                                       // Iptv:107
    // Storage.
    "restorecon '/storage/x' >/dev/null 2>&1",                                            // PS3Folder:888
    "gammaos-net usb mtp",                                                                 // Mtp:184
};

const char* const kSharedRoutes[] = {
    "setprop sys.gammaos.multidisplay.split_brightness.d1.override 50; "
    "setprop persist.gammaos.multidisplay.split_brightness.d1.last 50",                  // ControlCenter:1601
    "setprop persist.sys.timezone Europe/Berlin",                                          // PS3Menu:18454
    "setprop persist.sys.locale de-DE",                                                    // Strings:663
};

const std::vector<std::vector<const char*>> kSharedExecs = {
    // esdeRunProc (NanoMenuEsdeMenu.cpp:902-991).
    {"/system/bin/mkdir", "-p", "/data/system/nano_esde_themes"},
    {"/system/bin/gammaos-net", "download", "https://h/themes.json", "/data/system/nano_esde_themes/.themes.json"},
    {"/system/bin/rm", "-rf", "/data/system/nano_esde_themes/.dl_stage"},
    {"/system/bin/unzip", "-o", "-q", "/data/system/nano_esde_themes/.dl_stage/theme.zip", "-d", "/s"},
    {"/system/bin/mv", "/data/system/nano_esde_themes/.dl_stage/t", "/data/system/nano_esde_themes/t"},
    {"/system/bin/chown", "-R", "system:system", "/data/system/nano_esde_themes/t"},
    {"/system/bin/restorecon", "-R", "/data/system/nano_esde_themes/t"},
    // runArgv (NanoSdMove.cpp) and the BIOS RunFn callbacks.
    {"/system/bin/am", "force-stop", "org.es_de.frontend"},
    {"/system/bin/restorecon", "/storage/x"},
    {"/system/bin/logcat", "-d", "-T", "1.000"},
    {"/system/bin/am", "start", "-W", "-n", "org.vita3k.emulator/.MainActivity"},
    {"/system/bin/am", "start-foreground-service", "-n", "org.vita3k.emulator/.InstallForegroundService"},
    {"/system/bin/pidof", "org.vita3k.emulator"},
};

}  // namespace

TEST_F(ShellGateTest, TheAuditedSharedCommandsAreDecidedAsReviewed) {
    std::set<std::string> refused;
    for (const char* c : kSharedShellCommands) {
        Decision d = classifyCommand(c);
        EXPECT_EQ(d.verdict, Verdict::Refuse) << c;
        refused.insert(d.shape);
    }
    std::set<std::string> wantShell = {
        "/system/bin/curl",  "/vendor/bin/setclock_performance.sh",
        "am broadcast",      "am force-stop",
        "am start",          "am start-foreground-service",
        "am stopservice",    "appops set",
        "cmd alarm",         "cmd bluetooth_manager",
        "cmd color_display", "cmd connectivity",
        "cmd display",       "cmd location",
        "cmd media_session", "cmd notification",
        "cmd package",       "cmd uimode",
        "cmd wifi",          "curl",
        "dumpsys activity",  "dumpsys notification",
        "gammaos-net bt",    "gammaos-net usb",
        "gammaos-net wifi",  "ime enable",
        "ime reset",         "locksettings clear",
        "mkdir",             "pm enable",
        "pm list",           "pm path",
        "restorecon",        "screencap",
        "settings get",      "settings put",
        "svc wifi",
    };
    EXPECT_EQ(refused, wantShell);

    std::set<std::string> routed;
    for (const char* c : kSharedRoutes) {
        Decision d = classifyCommand(c);
        EXPECT_EQ(d.verdict, Verdict::Route) << c;
        for (const Command& cmd : d.commands) routed.insert(cmd.shape);
    }
    std::set<std::string> wantRoutes = {
        "setprop persist.gammaos.multidisplay.split_brightness.d1.last", "setprop persist.sys.locale",
        "setprop persist.sys.timezone", "setprop sys.gammaos.multidisplay.split_brightness.d1.override"};
    EXPECT_EQ(routed, wantRoutes);

    // The one pass: the allow-list.
    EXPECT_EQ(classifyCommand(kTzDate).verdict, Verdict::Pass);

    std::set<std::string> refusedExec;
    for (auto argv : kSharedExecs) {
        argv.push_back(nullptr);
        ExecPlan p;
        classifyExec(argv[0], const_cast<char* const*>(argv.data()), &p);
        EXPECT_EQ(p.action, ExecAction::Refuse) << argv[0];
        refusedExec.insert(p.shape);
    }
    std::set<std::string> wantExec = {
        "/system/bin/am force-stop",
        "/system/bin/am start",
        "/system/bin/am start-foreground-service",
        "/system/bin/chown",
        "/system/bin/gammaos-net download",
        "/system/bin/logcat",
        "/system/bin/mkdir",
        "/system/bin/mv",
        "/system/bin/pidof",
        "/system/bin/restorecon",
        "/system/bin/rm",
        "/system/bin/unzip",
    };
    EXPECT_EQ(refusedExec, wantExec);
}
