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
// the writes, and real children (small sh scripts standing in for RetroArch): fork, setsid, execve
// after the display's release, waitpid polling, SIGTERM to the group, a failed execve; and the whole
// path from a commit through the control keys to a started child.

#include "Launcher.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    std::unique_ptr<nanoplat::Launcher> make() { return nanolinux::launcher::create(&display, options); }

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
};

std::string get(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "");
    return v;
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
    std::unique_ptr<nanoplat::Launcher> l = make();
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
}

TEST_F(LauncherTest, StartsTheChildInItsOwnSessionAfterTheRelease) {
    script("exit 3\n");
    std::unique_ptr<nanoplat::Launcher> l = make();
    std::string err;
    ASSERT_TRUE(l->launch(core(), &err)) << err;
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
}

TEST_F(LauncherTest, AChildOnlyAfterTheLastOneEnded) {
    script("sleep 30 & echo $! > '" + (dir_ / "sleep.pid") + "'\nwait\n");
    std::unique_ptr<nanoplat::Launcher> l = make();
    std::string err;
    ASSERT_TRUE(l->launch(core(), &err)) << err;
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
    ASSERT_TRUE(l->launch(core(), &err)) << err;
    e = nanolinux::launcher::waitForChild(*l, 5);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->status, 0);
}

TEST_F(LauncherTest, AFailedExecveGivesThePanelBack) {
    // Executable but no format execve knows (no #! line): ENOEXEC from the child, through the pipe.
    ASSERT_TRUE(writeExecutable(options.retroarch, std::string("\x7f" "garbage", 8)));
    std::unique_ptr<nanoplat::Launcher> l = make();
    std::string err;
    EXPECT_FALSE(l->launch(core(), &err));
    EXPECT_NE(err.find("execve " + options.retroarch), std::string::npos) << err;
    EXPECT_FALSE(l->childRunning());
    EXPECT_EQ(display.joined(), "release reacquire");
}

TEST_F(LauncherTest, NothingStartsWithoutTheProgramOrTheCore) {
    std::unique_ptr<nanoplat::Launcher> l = make();
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
    EXPECT_FALSE(l->launch(p, &err));
    EXPECT_EQ(display.joined(), "");  // the panel never left
    EXPECT_FALSE(l->childRunning());
}

TEST_F(LauncherTest, APanelThatWasNotNanosIsNotTakenBack) {
    display.releaseOk = false;
    ASSERT_TRUE(writeExecutable(options.retroarch, std::string("\x7f" "garbage", 8)));
    std::unique_ptr<nanoplat::Launcher> l = make();
    std::string err;
    EXPECT_FALSE(l->launch(core(), &err));
    EXPECT_EQ(display.joined(), "release");
}

// The whole Linux path, as thimsy-nano wires it: the call site's commits reach the installed
// platform's launcher, the control keys see the trigger (one launch for do_launch and then
// nano_retroarch), and launchFromHandshake starts the child.
TEST_F(LauncherTest, FromACommitThroughTheControlKeysToAChild) {
    script("exit 0\n");
    std::unique_ptr<nanoplat::Platform> platform(new nanoplat::Platform);
    platform->launcher = make();
    nanoplat::Launcher* launcher = platform->launcher.get();
    nanoplat::install(std::move(platform));
    nanolinux::controlkeys::Handlers h;
    h.launch = nanolinux::launcher::launchFromHandshake;
    h.sessionRequest = [](const std::string&) {};
    h.runHook = [](const std::string&) {};
    nanolinux::controlkeys::install(h);

    android::commitLaunch(LaunchTuple()
                                  .romPath("/storage/1234-ABCD/ROMs/snes/b.sfc")
                                  .game("snes", "/storage/1234-ABCD/ROMs/snes/b.sfc", "/c/snes9x_libretro_android.so")
                                  .core("/c/snes9x_libretro_android.so")
                                  .app("com.retroarch.aarch64")
                                  .intent(""));
    EXPECT_FALSE(launcher->childRunning());
    android::commitLaunch(LaunchTuple().handoffFired("1").pendingExit("0").dropInput("1"));
    android::commitLaunch(LaunchTuple().doLaunch("1"));
    EXPECT_TRUE(launcher->childRunning());
    android::commitLaunch(LaunchTuple().retroarch("1"));   // the same launch: no second request
    std::optional<nanoplat::ChildExit> e = nanolinux::launcher::waitForChild(*launcher, 5);
    nanolinux::controlkeys::uninstall();
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->status, 0);
    EXPECT_EQ(display.joined(), "release");
    EXPECT_NE(compat_test::readFile(out).find("/storage/1234-ABCD/ROMs/snes/b.sfc"), std::string::npos);
    EXPECT_EQ(get("sys.gammaos.nano.launch_rom"), "/storage/1234-ABCD/ROMs/snes/b.sfc");  // the store keeps it
    EXPECT_EQ(get("sys.gammaos.nano.do_launch"), "1");
}

}  // namespace
