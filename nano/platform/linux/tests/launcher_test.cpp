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

// The Linux launcher (Launcher.h) on a scratch property store: the argv, the handshake's request,
// the writes, the queue until Nano's menu has ended, and real children (small sh scripts standing
// in for RetroArch): fork, setsid, execve after the display's release, waitpid polling, SIGTERM to
// the group, a failed execve, the handshake cleared at a launch's end; and the whole path from a
// commit through the control keys to a started child, with a ROM path too long for a property; the
// game's pid for the session (sys.thimsy.game.pid) and the exit path that leaves the game running
// (startAndLeave, leaveQueued).

#include "Launcher.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cutils/properties.h>

#include "ControlKeys.h"
#include "NanoLaunchCommit.h"
#include "NanoPlatform.h"
#include "compat_test_util.h"

using android::LaunchTuple;
using nanolinux::launcher::LinuxLauncher;
using nanolinux::launcher::Options;
using nanoplat::LaunchRequest;

namespace {

// A display that records the hand-off, and whether a child had already run when it was released.
class RecordingDisplay : public nanoplat::Display {
 public:
    int startScanout(int, bool, bool (*)()) override { return -1; }
    bool releaseForChild() override {
        calls.push_back("release");
        childRanBeforeRelease = childRanBeforeRelease || (!marker.empty() && access(marker.c_str(), F_OK) == 0);
        return releaseOk;
    }
    bool reacquire() override {
        calls.push_back("reacquire");
        return true;
    }
    std::string joined() const {
        std::string s;
        for (const std::string& c : calls) s += (s.empty() ? "" : " ") + c;
        return s;
    }
    std::vector<std::string> calls;
    std::string marker;  // a file the child writes
    bool releaseOk = true;
    bool childRanBeforeRelease = false;
};

// The installed platform's launcher: the current test's (install() runs once per process).
class ForwardingLauncher : public LinuxLauncher {
 public:
    bool nanoExitsOnLaunch() const override { return to->nanoExitsOnLaunch(); }
    void commit(const android::LaunchTuple& tuple) override { to->commit(tuple); }
    bool launch(const LaunchRequest& request, std::string* error) override { return to->launch(request, error); }
    bool childRunning() const override { return to->childRunning(); }
    std::optional<nanoplat::ChildExit> pollChild() override { return to->pollChild(); }
    void requestChildQuit() override { to->requestChildQuit(); }
    LaunchRequest requestFor(const nanolinux::controlkeys::LaunchRequest& h) const override { return to->requestFor(h); }
    bool inCommit() const override { return to->inCommit(); }
    bool queued() const override { return to->queued(); }
    bool startQueued(std::string* error) override { return to->startQueued(error); }
    Leave startAndLeave(int* pid, std::string* error) override { return to->startAndLeave(pid, error); }
    LinuxLauncher* to = nullptr;
};

bool writeExecutable(const std::string& path, const std::string& text, mode_t mode = 0755) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fputs(text.c_str(), f);
    fclose(f);
    return chmod(path.c_str(), mode) == 0;
}

class LauncherTest : public compat_test::StoreTest {
 protected:
    void SetUp() override {
        compat_test::StoreTest::SetUp();
        out = dir_ / "out";
        display.marker = out;
        options.retroarch = dir_ / "retroarch";
        options.coreDir = dir_.path();
        options.retroarchConfig = dir_ / "retroarch.cfg";
        ASSERT_TRUE(writeExecutable(dir_ / "snes9x_libretro.so", "", 0644));
    }
    // The stand-in RetroArch: writes its arguments, then its process group and session, to `out`.
    void script(const std::string& tail) {
        ASSERT_TRUE(writeExecutable(options.retroarch, "#!/bin/sh\necho \"$@\" > '" + out +
                                                           "'\ncut -d' ' -f5,6 /proc/$$/stat >> '" + out + "'\n" +
                                                           tail));
    }
    void TearDown() override {
        nanolinux::controlkeys::uninstall();
        compat_test::StoreTest::TearDown();
    }
    std::unique_ptr<LinuxLauncher> make() { return nanolinux::launcher::create(&display, options); }
    // launch(), then startQueued() as main_linux.cpp's runQueued() does once the menu has ended.
    bool launchNow(LinuxLauncher& l, const LaunchRequest& r, std::string* err) {
        return l.launch(r, err) && l.startQueued(err);
    }
    // The platform as main_linux.cpp installs it, and the control keys with the launcher's handler.
    // install() runs once per process, so the installed launcher forwards to this test's.
    LinuxLauncher* installPlatform() {
        static ForwardingLauncher* forward = [] {
            std::unique_ptr<nanoplat::Platform> platform(new nanoplat::Platform);
            ForwardingLauncher* f = new ForwardingLauncher;
            platform->launcher.reset(f);
            nanoplat::install(std::move(platform));
            return f;
        }();
        installed = make();
        forward->to = installed.get();
        nanolinux::controlkeys::Handlers h;
        h.sessionRequest = [](const std::string&) {};
        h.runHook = [](const std::string&) {};
        nanolinux::controlkeys::install(h);
        nanolinux::controlkeys::setLaunchHandler(nanolinux::launcher::launchFromHandshake);
        return nanolinux::launcher::linuxLauncher();
    }

    LaunchRequest core(const std::string& rom = "/storage/1234-ABCD/ROMs/snes/a.sfc") {
        LaunchRequest r;
        r.kind = LaunchRequest::Kind::Core;
        r.package = nanolinux::launcher::kRetroarchPackage;
        r.core = "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so";
        r.rom = rom;
        r.trigger = "sys.gammaos.nano.do_launch";
        return r;
    }

    std::string out;
    Options options;
    RecordingDisplay display;
    std::unique_ptr<LinuxLauncher> installed;  // installPlatform()'s
};

std::string get(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "");
    return v;
}

// threadLoop's exit path after a launch (NanoMenu.cpp): both of its waits (the DRM boot path's and
// the "Loading..." one) are this loop around their drawing; it returns the frames waited.
// tools/nano-tree/tests/test_exit_path_linux.py checks that NanoMenu.cpp's waits still are.
// exit-path-wait: begin
int exitPathWaits() {
    char launched[PROPERTY_VALUE_MAX] = {};
    int wait = 0;
    for (; wait < 600; wait++) {
        property_get("sys.gammaos.nano.app_launched", launched, "0");
        if (!strcmp(launched, "1")) break;
        usleep(16666);
    }
    return wait;
}
// exit-path-wait: end

int64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

TEST(LauncherArgv, CoreNames) {
    using nanolinux::launcher::coreName;
    EXPECT_EQ(coreName("/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so"), "snes9x");
    EXPECT_EQ(coreName("mupen64plus_next_gles3_libretro_android.so"), "mupen64plus_next_gles3");
    EXPECT_EQ(coreName("/usr/lib/libretro/mgba_libretro.so"), "mgba");
    EXPECT_EQ(coreName("fbneo.so"), "fbneo");
    EXPECT_EQ(coreName("genesis_plus_gx"), "genesis_plus_gx");
    EXPECT_EQ(coreName(""), "");
    EXPECT_EQ(coreName("/cores/"), "");
    EXPECT_EQ(coreName("/cores/_libretro_android.so"), "");
    EXPECT_EQ(coreName("/cores/..so"), "");
}

TEST(LauncherArgv, CorePackageAndRefusals) {
    Options o;
    LaunchRequest r;
    r.kind = LaunchRequest::Kind::Core;
    r.core = "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so";
    r.rom = "/storage/1234-ABCD/ROMs/snes/Test Game.sfc";
    std::vector<std::string> argv;
    std::string err;
    ASSERT_TRUE(nanolinux::launcher::argvFor(r, o, &argv, &err)) << err;
    EXPECT_EQ(argv, (std::vector<std::string>{"/usr/bin/retroarch", "-L", "/usr/lib/libretro/snes9x_libretro.so",
                                              "--config", "/storage/.config/retroarch/retroarch.cfg",
                                              "/storage/1234-ABCD/ROMs/snes/Test Game.sfc"}));
    r.rom = "";
    ASSERT_TRUE(nanolinux::launcher::argvFor(r, o, &argv, &err));
    EXPECT_EQ(argv.size(), 5u);  // no content: the core alone
    r.core = "/cores/";
    EXPECT_FALSE(nanolinux::launcher::argvFor(r, o, &argv, &err));
    EXPECT_NE(err.find("no core"), std::string::npos);
    EXPECT_TRUE(argv.empty());

    LaunchRequest p;
    p.kind = LaunchRequest::Kind::Package;
    p.package = "com.retroarch.aarch64";
    ASSERT_TRUE(nanolinux::launcher::argvFor(p, o, &argv, &err));
    EXPECT_EQ(argv, (std::vector<std::string>{"/usr/bin/retroarch", "--config",
                                              "/storage/.config/retroarch/retroarch.cfg"}));
    p.package = "";  // launch_app "": RetroArch, as Nano and the framework read it
    ASSERT_TRUE(nanolinux::launcher::argvFor(p, o, &argv, &err));
    EXPECT_EQ(argv.size(), 3u);
    p.package = "org.ppsspp.ppsspp";
    EXPECT_FALSE(nanolinux::launcher::argvFor(p, o, &argv, &err));
    EXPECT_NE(err.find("org.ppsspp.ppsspp"), std::string::npos);
}

TEST(LauncherArgv, RequestFromTheHandshake) {
    nanolinux::controlkeys::LaunchRequest h;
    h.trigger = "service.bootanim.nano_retroarch";
    h.keys["sys.gammaos.nano.launch_app"] = "com.retroarch.aarch64";
    h.keys["sys.gammaos.nano.launch_core"] = "/c/snes9x_libretro_android.so";
    h.keys["sys.gammaos.nano.launch_rom"] = "/r/a.sfc";
    h.keys["sys.gammaos.nano.launch_game"] = "/r/other.sfc";
    h.keys["sys.gammaos.nano.launch_system"] = "snes";
    h.keys["sys.gammaos.nano.launch_intent"] = "";
    LaunchRequest r = nanolinux::launcher::requestFromHandshake(h);
    EXPECT_TRUE(r.kind == LaunchRequest::Kind::Core);
    EXPECT_EQ(r.package, "com.retroarch.aarch64");
    EXPECT_EQ(r.core, "/c/snes9x_libretro_android.so");
    EXPECT_EQ(r.rom, "/r/a.sfc");
    EXPECT_EQ(r.system, "snes");
    EXPECT_EQ(r.trigger, "service.bootanim.nano_retroarch");
    // A standalone: no core, no launch_rom (setLaunchRomPath("")), the game in launch_game.
    h.keys["sys.gammaos.nano.launch_app"] = "org.ppsspp.ppsspp";
    h.keys["sys.gammaos.nano.launch_core"] = "";
    h.keys["sys.gammaos.nano.launch_rom"] = "";
    h.keys["sys.gammaos.nano.launch_game"] = "/r/b.iso";
    h.keys["sys.gammaos.nano.launch_intent"] = "file";
    r = nanolinux::launcher::requestFromHandshake(h);
    EXPECT_TRUE(r.kind == LaunchRequest::Kind::Package);
    EXPECT_EQ(r.rom, "/r/b.iso");
    EXPECT_EQ(r.intent, "file");
}

TEST_F(LauncherTest, CommitWritesTheHandshakeInOrder) {
    std::vector<std::string> seen;
    const int id = nanocompat::addPropertyObserver("", [&](std::string_view k, std::string_view v) {
        if (k == "sys.gammaos.nano.launch_time") v = "<time>";
        seen.push_back(std::string(k) + "=" + std::string(v));
    });
    std::unique_ptr<LinuxLauncher> l = make();
    EXPECT_FALSE(l->nanoExitsOnLaunch());
    l->commit(LaunchTuple()
                      .romPath("")
                      .game("snes", "/r/a.sfc", "/c/x.so")
                      .core("/c/x.so")
                      .app("com.retroarch.aarch64")
                      .intent("")
                      .doLaunch("1"));
    nanocompat::removePropertyObserver(id);
    std::string s;
    for (const std::string& x : seen) s += x + "\n";
    EXPECT_EQ(s, "sys.gammaos.nano.launch_rom=\n"
                 "sys.gammaos.nano.launch_system=\n"
                 "sys.gammaos.nano.launch_game=\n"
                 "sys.gammaos.nano.launch_game_core=\n"
                 "sys.gammaos.nano.launch_time=<time>\n"
                 "sys.gammaos.nano.launch_system=snes\n"
                 "sys.gammaos.nano.launch_game=/r/a.sfc\n"
                 "sys.gammaos.nano.launch_game_core=/c/x.so\n"
                 "sys.gammaos.nano.launch_core=/c/x.so\n"
                 "sys.gammaos.nano.launch_app=com.retroarch.aarch64\n"
                 "sys.gammaos.nano.launch_intent=\n"
                 "sys.gammaos.nano.do_launch=1\n");
    EXPECT_FALSE(l->childRunning());  // no control keys here: a commit alone starts nothing
    EXPECT_FALSE(l->queued());
}

TEST_F(LauncherTest, TheLaunchWaitsForTheMenusEnd) {
    script("exit 3\n");
    std::unique_ptr<LinuxLauncher> l = make();
    std::string err;
    ASSERT_TRUE(l->launch(core(), &err)) << err;
    // Queued: nothing runs and the panel stays Nano's while Nano finishes its launch site.
    EXPECT_TRUE(l->queued());
    EXPECT_FALSE(l->childRunning());
    EXPECT_EQ(display.joined(), "");
    EXPECT_FALSE(l->launch(core(), &err));  // one launch at a time
    EXPECT_NE(err.find("already queued"), std::string::npos) << err;
    usleep(50 * 1000);
    EXPECT_NE(access(out.c_str(), F_OK), 0);

    ASSERT_TRUE(l->startQueued(&err)) << err;
    EXPECT_FALSE(l->queued());
    EXPECT_TRUE(l->childRunning());
    std::optional<nanoplat::ChildExit> e = nanolinux::launcher::waitForChild(*l, 5);
    ASSERT_TRUE(e.has_value());
    EXPECT_FALSE(e->signaled);
    EXPECT_EQ(e->status, 3);
    EXPECT_GT(e->pid, 0);
    EXPECT_FALSE(l->childRunning());
    EXPECT_FALSE(l->pollChild().has_value());  // reported once
    EXPECT_EQ(display.joined(), "release");    // the panel goes back by Nano's restart, not here
    EXPECT_FALSE(display.childRanBeforeRelease);
    const std::string text = compat_test::readFile(out);
    EXPECT_EQ(text, "-L " + (dir_ / "snes9x_libretro.so") + " --config " + (dir_ / "retroarch.cfg") +
                            " /storage/1234-ABCD/ROMs/snes/a.sfc\n" + std::to_string(e->pid) + " " +
                            std::to_string(e->pid) + "\n");  // its own process group and session
    // Nothing queued: nothing starts.
    EXPECT_FALSE(l->startQueued(&err));
    EXPECT_EQ(err, "");
}

// linux-host-impls-2: the session sees the game through sys.thimsy.game.pid, set when the child
// starts and cleared when pollChild reports its end.
TEST_F(LauncherTest, TheGamesPidIsTheSessions) {
    script("sleep 30 & wait\n");
    std::unique_ptr<LinuxLauncher> l = make();
    std::string err;
    EXPECT_EQ(get(nanolinux::launcher::kGamePidKey), "");
    ASSERT_TRUE(launchNow(*l, core(), &err)) << err;
    const std::string pid = get(nanolinux::launcher::kGamePidKey);
    ASSERT_FALSE(pid.empty());
    EXPECT_EQ(getpgid(atoi(pid.c_str())), atoi(pid.c_str()));  // the group the session signals
    l->requestChildQuit();
    std::optional<nanoplat::ChildExit> e = nanolinux::launcher::waitForChild(*l, 5);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(std::to_string(e->pid), pid);
    EXPECT_EQ(get(nanolinux::launcher::kGamePidKey), "");
    // A launch that does not start leaves the key alone.
    ASSERT_TRUE(writeExecutable(options.retroarch, std::string("\x7f" "garbage", 8)));
    EXPECT_FALSE(launchNow(*l, core(), &err));
    EXPECT_EQ(get(nanolinux::launcher::kGamePidKey), "");
}

// linux-host-impls-1 and -6: main_linux.cpp's leaveQueued(): the game is left running for the
// session, the launch is over for Nano (the handshake cleared) and the result says what happened.
TEST_F(LauncherTest, StartAndLeaveLeavesTheGameRunning) {
    using Leave = LinuxLauncher::Leave;
    script("sleep 30 & echo $! > '" + (dir_ / "sleep.pid") + "'\nwait\n");
    std::unique_ptr<LinuxLauncher> l = make();
    std::string err = "x";
    int pid = 0;
    EXPECT_EQ(l->startAndLeave(&pid, &err), Leave::NoLaunch);  // nothing asked for
    EXPECT_EQ(pid, -1);
    EXPECT_EQ(err, "");

    l->commit(LaunchTuple().core("/c/snes9x_libretro_android.so").app("com.retroarch.aarch64"));
    ASSERT_TRUE(l->launch(core(), &err)) << err;
    ASSERT_EQ(l->startAndLeave(&pid, &err), Leave::Started) << err;
    ASSERT_GT(pid, 0);
    EXPECT_EQ(get(nanolinux::launcher::kGamePidKey), std::to_string(pid));  // stays for the session
    EXPECT_EQ(get("sys.gammaos.nano.launch_core"), "");                      // over for Nano
    EXPECT_EQ(get("sys.gammaos.nano.launch_app"), "");
    EXPECT_FALSE(l->childRunning());            // no longer this launcher's to poll
    EXPECT_FALSE(l->pollChild().has_value());
    EXPECT_EQ(display.joined(), "release");
    EXPECT_EQ(kill(pid, 0), 0);                 // still running
    EXPECT_TRUE(l->launch(core(), &err)) << err;  // the next Nano's launch is not refused here
    EXPECT_EQ(l->startAndLeave(nullptr, &err), Leave::Started) << err;
    // The test reaps what it left running: SIGTERM to both groups.
    const std::string second = get(nanolinux::launcher::kGamePidKey);
    kill(-pid, SIGTERM);
    kill(-atoi(second.c_str()), SIGTERM);
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_EQ(waitpid(atoi(second.c_str()), &status, 0), atoi(second.c_str()));

    // Refused (a package with no Linux emulator) or not started (a failed execve): NotStarted.
    LaunchRequest p;
    p.kind = LaunchRequest::Kind::Package;
    p.package = "org.ppsspp.ppsspp";
    EXPECT_FALSE(l->launch(p, &err));
    EXPECT_EQ(l->startAndLeave(&pid, &err), Leave::NotStarted);
    EXPECT_EQ(pid, -1);
    ASSERT_TRUE(writeExecutable(options.retroarch, std::string("\x7f" "garbage", 8)));
    ASSERT_TRUE(l->launch(core(), &err)) << err;
    EXPECT_EQ(l->startAndLeave(&pid, &err), Leave::NotStarted);
    EXPECT_NE(err.find("execve"), std::string::npos) << err;
    EXPECT_EQ(l->startAndLeave(&pid, &err), Leave::NoLaunch);  // and that launch is over
}

TEST_F(LauncherTest, AChildOnlyAfterTheLastOneEnded) {
    script("sleep 30 & echo $! > '" + (dir_ / "sleep.pid") + "'\nwait\n");
    std::unique_ptr<LinuxLauncher> l = make();
    std::string err;
    ASSERT_TRUE(launchNow(*l, core(), &err)) << err;
    EXPECT_FALSE(l->launch(core(), &err));
    EXPECT_NE(err.find("already running"), std::string::npos);
    EXPECT_FALSE(l->pollChild().has_value());
    ASSERT_TRUE(compat_test::eventually([&] { return !compat_test::readFile(dir_ / "sleep.pid").empty(); }));
    const int sleepPid = atoi(compat_test::readFile(dir_ / "sleep.pid").c_str());
    ASSERT_GT(sleepPid, 0);

    // SIGTERM to the group: the script and the sleep it started both end.
    l->requestChildQuit();
    std::optional<nanoplat::ChildExit> e = nanolinux::launcher::waitForChild(*l, 5);
    ASSERT_TRUE(e.has_value());
    EXPECT_TRUE(e->signaled);
    EXPECT_EQ(e->status, SIGTERM);
    EXPECT_TRUE(compat_test::eventually([&] {
        const std::string stat = compat_test::readFile("/proc/" + std::to_string(sleepPid) + "/stat");
        return stat.empty() || stat.find(") Z ") != std::string::npos;
    }));
    l->requestChildQuit();  // no child: nothing
    script("exit 0\n");
    ASSERT_TRUE(launchNow(*l, core(), &err)) << err;
    e = nanolinux::launcher::waitForChild(*l, 5);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->status, 0);
}

TEST_F(LauncherTest, AFailedExecveGivesThePanelBack) {
    // Executable but no format execve knows (no #! line): ENOEXEC from the child, through the pipe.
    ASSERT_TRUE(writeExecutable(options.retroarch, std::string("\x7f" "garbage", 8)));
    std::unique_ptr<LinuxLauncher> l = make();
    l->commit(LaunchTuple().core("/c/snes9x_libretro_android.so").app("com.retroarch.aarch64"));
    std::string err;
    ASSERT_TRUE(l->launch(core(), &err)) << err;
    EXPECT_FALSE(l->startQueued(&err));
    EXPECT_NE(err.find("execve " + options.retroarch), std::string::npos) << err;
    EXPECT_FALSE(l->childRunning());
    EXPECT_FALSE(l->queued());
    EXPECT_EQ(display.joined(), "release reacquire");
    // The launch is over: the handshake is cleared, as the framework clears it.
    EXPECT_EQ(get("sys.gammaos.nano.launch_core"), "");
    EXPECT_EQ(get("sys.gammaos.nano.launch_app"), "");
}

TEST_F(LauncherTest, NothingStartsWithoutTheProgramOrTheCore) {
    std::unique_ptr<LinuxLauncher> l = make();
    std::string err;
    EXPECT_FALSE(l->launch(core(), &err));  // no retroarch yet
    EXPECT_NE(err.find(options.retroarch), std::string::npos);
    script("exit 0\n");
    LaunchRequest r = core();
    r.core = "/cores/missing_libretro_android.so";
    EXPECT_FALSE(l->launch(r, &err));
    EXPECT_NE(err.find("missing_libretro.so"), std::string::npos);
    LaunchRequest p;
    p.kind = LaunchRequest::Kind::Package;
    p.package = "org.ppsspp.ppsspp";
    l->commit(LaunchTuple().app("org.ppsspp.ppsspp").intent("file"));
    EXPECT_FALSE(l->launch(p, &err));
    EXPECT_FALSE(l->queued());
    // At the menu's end: nothing to start, and the refused launch is over (its handshake cleared).
    EXPECT_FALSE(l->startQueued(&err));
    EXPECT_EQ(err, "");
    EXPECT_EQ(get("sys.gammaos.nano.launch_app"), "");
    EXPECT_EQ(get("sys.gammaos.nano.launch_intent"), "");
    EXPECT_EQ(display.joined(), "");  // the panel never left
    EXPECT_FALSE(l->childRunning());
}

TEST_F(LauncherTest, APanelThatWasNotNanosIsNotTakenBack) {
    display.releaseOk = false;
    ASSERT_TRUE(writeExecutable(options.retroarch, std::string("\x7f" "garbage", 8)));
    std::unique_ptr<LinuxLauncher> l = make();
    std::string err;
    EXPECT_FALSE(launchNow(*l, core(), &err));
    EXPECT_EQ(display.joined(), "release");
}

TEST_F(LauncherTest, RequestForPrefersTheFullValuesItWrote) {
    std::unique_ptr<LinuxLauncher> l = make();
    const std::string longRom = "/storage/1234-ABCD/ROMs/snes/" + std::string(100, 'x') + ".sfc";
    l->commit(LaunchTuple().romPath("/r/short.sfc").core("/c/snes9x_libretro_android.so"));
    l->commit(LaunchTuple().romPath(longRom).game("snes", longRom, "/c/snes9x_libretro_android.so"));
    // The store refused the long value: launch_rom keeps the older path, launch_game stays "".
    ASSERT_EQ(get("sys.gammaos.nano.launch_rom"), "/r/short.sfc");
    ASSERT_EQ(get("sys.gammaos.nano.launch_game"), "");
    nanolinux::controlkeys::LaunchRequest h;
    h.trigger = "sys.gammaos.nano.do_launch";
    h.keys["sys.gammaos.nano.launch_rom"] = get("sys.gammaos.nano.launch_rom");
    h.keys["sys.gammaos.nano.launch_game"] = "";
    h.keys["sys.gammaos.nano.launch_core"] = get("sys.gammaos.nano.launch_core");
    h.keys["sys.gammaos.nano.launch_system"] = get("sys.gammaos.nano.launch_system");
    LaunchRequest r = l->requestFor(h);
    EXPECT_EQ(r.rom, longRom);
    EXPECT_EQ(r.system, "snes");
    EXPECT_EQ(r.core, "/c/snes9x_libretro_android.so");
    // Another process changed a key since: its value, not the launcher's.
    h.keys["sys.gammaos.nano.launch_rom"] = "/r/other.sfc";
    h.keys["sys.gammaos.nano.launch_core"] = "/c/mgba_libretro_android.so";
    r = l->requestFor(h);
    EXPECT_EQ(r.rom, "/r/other.sfc");
    EXPECT_EQ(r.core, "/c/mgba_libretro_android.so");
}

// The whole Linux path, as thimsy-nano wires it: the call site's commits reach the installed
// platform's launcher, the control keys see the trigger (one launch for do_launch and then
// nano_retroarch), launchFromHandshake queues it, and runQueued starts the child at the menu's end.
TEST_F(LauncherTest, FromACommitThroughTheControlKeysToAChild) {
    script("exit 0\n");
    LinuxLauncher* launcher = installPlatform();
    ASSERT_TRUE(launcher != nullptr);

    android::commitLaunch(LaunchTuple()
                                  .romPath("/storage/1234-ABCD/ROMs/snes/b.sfc")
                                  .game("snes", "/storage/1234-ABCD/ROMs/snes/b.sfc", "/c/snes9x_libretro_android.so")
                                  .core("/c/snes9x_libretro_android.so")
                                  .app("com.retroarch.aarch64")
                                  .intent(""));
    EXPECT_FALSE(launcher->queued());
    android::commitLaunch(LaunchTuple().handoffFired("1").pendingExit("0").dropInput("1"));
    android::commitLaunch(LaunchTuple().doLaunch("1"));
    EXPECT_TRUE(launcher->queued());
    EXPECT_FALSE(launcher->childRunning());
    android::commitLaunch(LaunchTuple().retroarch("1"));   // the same launch: no second request
    std::optional<nanoplat::ChildExit> e = nanolinux::launcher::runQueued(5);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->status, 0);
    EXPECT_EQ(display.joined(), "release");
    EXPECT_NE(compat_test::readFile(out).find("/storage/1234-ABCD/ROMs/snes/b.sfc"), std::string::npos);
    // The launch is over: the keys the framework clears are cleared; the rest stays in the store.
    EXPECT_EQ(get("sys.gammaos.nano.launch_rom"), "");
    EXPECT_EQ(get("sys.gammaos.nano.launch_core"), "");
    EXPECT_EQ(get("sys.gammaos.nano.launch_app"), "");
    EXPECT_EQ(get("sys.gammaos.nano.launch_game"), "/storage/1234-ABCD/ROMs/snes/b.sfc");
    EXPECT_EQ(get("sys.gammaos.nano.do_launch"), "1");
    EXPECT_FALSE(nanolinux::launcher::runQueued(5).has_value());  // nothing more
}

// A ROM on a long card path (over a property's 91 bytes) after a launch on a short one: the child
// gets the long path, not the store's older value.
TEST_F(LauncherTest, ALongRomPathAfterAShortOne) {
    script("exit 0\n");
    LinuxLauncher* launcher = installPlatform();
    ASSERT_TRUE(launcher != nullptr);
    const std::string shortRom = "/storage/1234-ABCD/ROMs/snes/a.sfc";
    const std::string longRom =
            "/storage/1234-ABCD/ROMs/snes/Super Long Game Name - " + std::string(70, 'x') + ".sfc";
    ASSERT_GT(longRom.size(), 115u);
    const std::string core = "/c/snes9x_libretro_android.so";
    android::commitLaunch(LaunchTuple().romPath(shortRom).game("snes", shortRom, core).core(core).doLaunch("1"));
    ASSERT_TRUE(nanolinux::launcher::runQueued(5).has_value());
    EXPECT_NE(compat_test::readFile(out).find(shortRom), std::string::npos);

    // Nano commits the short path again without a trigger (the store keeps it), then the long one.
    android::commitLaunch(LaunchTuple().romPath(shortRom));
    android::commitLaunch(LaunchTuple().romPath(longRom).game("snes", longRom, core).core(core));
    EXPECT_EQ(get("sys.gammaos.nano.launch_rom"), shortRom);  // the store refused the long path
    android::commitLaunch(LaunchTuple().doLaunch("1"));
    ASSERT_TRUE(launcher->queued());
    ASSERT_TRUE(nanolinux::launcher::runQueued(5).has_value());
    const std::string text = compat_test::readFile(out);
    EXPECT_NE(text.find(" " + longRom + "\n"), std::string::npos) << text;
    EXPECT_EQ(text.find(shortRom), std::string::npos) << text;
}

// After a core launch, the main menu's RetroArch item writes only the trigger
// (armAppLaunchTrigger): plain RetroArch, not the last game's core.
TEST_F(LauncherTest, ATriggerAloneAfterACoreLaunchStartsRetroArch) {
    script("exit 0\n");
    LinuxLauncher* launcher = installPlatform();
    ASSERT_TRUE(launcher != nullptr);
    const std::string core = "/c/snes9x_libretro_android.so";
    android::commitLaunch(LaunchTuple()
                                  .romPath("/r/a.sfc")
                                  .game("snes", "/r/a.sfc", core)
                                  .core(core)
                                  .app("com.retroarch.aarch64")
                                  .doLaunch("1"));
    ASSERT_TRUE(nanolinux::launcher::runQueued(5).has_value());
    EXPECT_NE(compat_test::readFile(out).find("-L "), std::string::npos);

    android::commitLaunch(LaunchTuple().retroarch("1"));
    ASSERT_TRUE(launcher->queued());
    ASSERT_TRUE(nanolinux::launcher::runQueued(5).has_value());
    const std::string text = compat_test::readFile(out);
    EXPECT_EQ(text.substr(0, text.find('\n')), "--config " + (dir_ / "retroarch.cfg")) << text;
}

// main_linux.cpp's path: a commit through the control keys, then leaveQueued() at the menu's end.
TEST_F(LauncherTest, LeaveQueuedFromACommit) {
    using Leave = LinuxLauncher::Leave;
    script("exit 0\n");
    LinuxLauncher* launcher = installPlatform();
    ASSERT_TRUE(launcher != nullptr);
    int pid = 0;
    EXPECT_EQ(nanolinux::launcher::leaveQueued(&pid), Leave::NoLaunch);
    EXPECT_EQ(pid, -1);
    android::commitLaunch(LaunchTuple()
                                  .romPath("/r/a.sfc")
                                  .game("snes", "/r/a.sfc", "/c/snes9x_libretro_android.so")
                                  .core("/c/snes9x_libretro_android.so")
                                  .app("com.retroarch.aarch64")
                                  .doLaunch("1"));
    ASSERT_TRUE(launcher->queued());
    ASSERT_EQ(nanolinux::launcher::leaveQueued(&pid), Leave::Started);
    ASSERT_GT(pid, 0);
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);  // the test stands in for the session's reaper
    EXPECT_EQ(get("sys.gammaos.nano.launch_rom"), "");
    EXPECT_EQ(nanolinux::launcher::leaveQueued(&pid), Leave::NoLaunch);
}

// A trigger another process writes is not a launch while Nano runs.
TEST_F(LauncherTest, ATriggerFromOutsideCommitLaunchIsRefused) {
    script("exit 0\n");
    LinuxLauncher* launcher = installPlatform();
    ASSERT_TRUE(launcher != nullptr);
    property_set("sys.gammaos.nano.launch_app", "com.retroarch.aarch64");
    property_set("sys.gammaos.nano.do_launch", "1");
    EXPECT_FALSE(launcher->queued());
    EXPECT_FALSE(nanolinux::launcher::runQueued(5).has_value());
    EXPECT_EQ(get("sys.gammaos.nano.launch_app"), "com.retroarch.aarch64");  // not Nano's launch: untouched
    EXPECT_EQ(get(nanolinux::launcher::kAppLaunchedKey), "");                // and no app in front of Nano
}

// The launch delay: Android's framework sets app_launched while the home finishes its launch site,
// and threadLoop's exit path waits for it (600 frames, ~10 s of "Loading..."). On Linux the game
// starts only after the menu has ended, so the launcher sets it when it takes the launch: by the
// time the launch site ends the menu, the exit path's wait ends on its first look.
TEST_F(LauncherTest, TheMenusExitPathEndsAtOnceAfterNanosLaunch) {
    using Leave = LinuxLauncher::Leave;
    script("exit 0\n");
    LinuxLauncher* launcher = installPlatform();
    ASSERT_TRUE(launcher != nullptr);
    EXPECT_EQ(get(nanolinux::launcher::kAppLaunchedKey), "");
    std::vector<std::string> seen;
    const int id = nanocompat::addPropertyObserver(nanolinux::launcher::kAppLaunchedKey,
                                                   [&](std::string_view, std::string_view v) {
                                                       seen.push_back(std::string(v) + (launcher->queued() ? " queued" : ""));
                                                   });
    android::commitLaunch(LaunchTuple()
                                  .romPath("/r/a.sfc")
                                  .game("snes", "/r/a.sfc", "/c/snes9x_libretro_android.so")
                                  .core("/c/snes9x_libretro_android.so")
                                  .app("com.retroarch.aarch64"));
    EXPECT_EQ(get(nanolinux::launcher::kAppLaunchedKey), "");  // the keys alone are no launch
    android::commitLaunch(LaunchTuple().handoffFired("1").pendingExit("0").dropInput("1").doLaunch("1"));
    android::commitLaunch(LaunchTuple().retroarch("1"));  // the same launch: written once
    nanocompat::removePropertyObserver(id);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "1 queued");  // set once the launch is queued, inside the launch site's commit
    const int64_t t0 = nowMs();
    EXPECT_EQ(exitPathWaits(), 0);
    EXPECT_LT(nowMs() - t0, 100);
    int pid = 0;
    ASSERT_EQ(nanolinux::launcher::leaveQueued(&pid), Leave::Started);
    ASSERT_GT(pid, 0);
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    // The launcher leaves it set: the game is in front until thimsy-session clears it at its end.
    EXPECT_EQ(get(nanolinux::launcher::kAppLaunchedKey), "1");
}

// A launch that cannot start (here: no RetroArch) still ends Nano's menu (the launch site exits and
// Nano exits for the session to restart it), so the exit path does not wait for it either.
TEST_F(LauncherTest, ARefusedLaunchDoesNotHoldTheExitPath) {
    using Leave = LinuxLauncher::Leave;
    LinuxLauncher* launcher = installPlatform();  // no script(): options.retroarch does not exist
    ASSERT_TRUE(launcher != nullptr);
    android::commitLaunch(LaunchTuple()
                                  .romPath("/r/a.sfc")
                                  .core("/c/snes9x_libretro_android.so")
                                  .app("com.retroarch.aarch64")
                                  .doLaunch("1"));
    EXPECT_FALSE(launcher->queued());
    EXPECT_EQ(get(nanolinux::launcher::kAppLaunchedKey), "1");
    EXPECT_EQ(exitPathWaits(), 0);
    int pid = 0;
    EXPECT_EQ(nanolinux::launcher::leaveQueued(&pid), Leave::NotStarted);
    EXPECT_EQ(pid, -1);
}

}  // namespace
