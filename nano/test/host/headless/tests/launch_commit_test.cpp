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

// commitLaunch() (nano/NanoLaunchCommit.cpp, G7-launch-a): the launch handshake's writes reach the
// property store in the tuple's order, every one of them, as the compat layer's property observers
// (the control keys' hook, B.2) see each set. The sequences below are the call sites' own.
// Since G7-launch-b commitLaunch hands the tuple to the installed platform's launcher: this program
// installs a platform with the firmware's (platform/firmware/Launcher.cpp, the moved writes), so
// the same sequences check the move.

#include <string.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <nanocompat/nanocompat.h>

#include "NanoLaunchCommit.h"
#include "NanoPlatform.h"
#include "headless_test_util.h"
#include "platform/firmware/Launcher.h"

using android::LaunchKey;
using android::LaunchTuple;
using android::commitLaunch;

namespace {

// The firmware platform's launcher, installed once for the program (install() is once per process).
void installFirmwareLauncher() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    std::unique_ptr<nanoplat::Platform> p(new nanoplat::Platform);
    p->launcher = nanofirmware::createLauncher();
    nanoplat::install(std::move(p));
}

class LaunchCommit : public ::testing::Test {
 protected:
    void SetUp() override {
        installFirmwareLauncher();
        nanocompat::InitOptions o;
        o.persistFile = dir / "persist.props";
        o.runDir = dir / "run";
        o.watch = false;
        std::string err;
        ASSERT_TRUE(nanocompat::init(o, &err)) << err;
        id = nanocompat::addPropertyObserver("", [this](std::string_view k, std::string_view v) {
            std::string key(k);
            // setLaunchRomPath stamps the launch with the time: only that it was written counts.
            if (key == "sys.gammaos.nano.launch_time") v = "<time>";
            const std::string pre = "sys.gammaos.nano.";
            if (key.compare(0, pre.size(), pre) == 0) key = key.substr(pre.size());
            seen.push_back(key + "=" + std::string(v));
        });
    }
    void TearDown() override { nanocompat::removePropertyObserver(id); }

    // The writes seen since the last call, one "key=value" per line (sys.gammaos.nano. dropped).
    std::string take() {
        std::string out;
        for (const std::string& s : seen) out += s + "\n";
        seen.clear();
        return out;
    }

    headless_test::TmpDir dir;
    int id = -1;
    std::vector<std::string> seen;
};

const char* const kRomPathEmpty =
        "launch_rom=\n"
        "launch_system=\n"
        "launch_game=\n"
        "launch_game_core=\n"
        "launch_time=<time>\n";

TEST_F(LaunchCommit, EveryKeyNamesItsProperty) {
    struct Row { LaunchKey key; const char* prop; };
    const Row rows[] = {
        {LaunchKey::App, "sys.gammaos.nano.launch_app"},
        {LaunchKey::LaunchedPkg, "sys.gammaos.nano.launched_pkg"},
        {LaunchKey::Intent, "sys.gammaos.nano.launch_intent"},
        {LaunchKey::Core, "sys.gammaos.nano.launch_core"},
        {LaunchKey::Rom, "sys.gammaos.nano.launch_rom"},
        {LaunchKey::ReturnRecent, "sys.gammaos.nano.return_recent"},
        {LaunchKey::DropInput, "sys.gammaos.nano.drop_input"},
        {LaunchKey::HandoffFired, "sys.gammaos.nano.handoff_fired"},
        {LaunchKey::PendingExit, "sys.gammaos.nano.pending_exit"},
        {LaunchKey::DoLaunch, "sys.gammaos.nano.do_launch"},
        {LaunchKey::Retroarch, "service.bootanim.nano_retroarch"},
    };
    for (const Row& r : rows) {
        ASSERT_TRUE(android::launchKeyProperty(r.key) != nullptr) << r.prop;
        EXPECT_EQ(std::string(android::launchKeyProperty(r.key)), std::string(r.prop));
    }
    EXPECT_TRUE(android::launchKeyProperty(LaunchKey::RomPath) == nullptr);
    EXPECT_TRUE(android::launchKeyProperty(LaunchKey::Game) == nullptr);
}

TEST_F(LaunchCommit, AnEmptyTupleWritesNothing) {
    commitLaunch(LaunchTuple());
    EXPECT_EQ(take(), "");
}

// NanoMenu.cpp's threadLoop, the drastic-nano SurfaceFlinger hand-off (DrasticSf).
TEST_F(LaunchCommit, DrasticSfHandOffInItsOrder) {
    commitLaunch(LaunchTuple()
            .app("com.gammaos.drasticsf")
            .launchedPkg("com.gammaos.drasticsf")
            .intent("")
            .romPath("")
            .core("")
            .returnRecent("1")
            .retroarch("1")
            .dropInput("1"));
    EXPECT_EQ(take(), std::string("launch_app=com.gammaos.drasticsf\n"
                                  "launched_pkg=com.gammaos.drasticsf\n"
                                  "launch_intent=\n") +
                              kRomPathEmpty +
                              "launch_core=\n"
                              "return_recent=1\n"
                              "service.bootanim.nano_retroarch=1\n"
                              "drop_input=1\n");
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("sys.gammaos.nano.launch_app", v, "");
    EXPECT_EQ(std::string(v), "com.gammaos.drasticsf");
}

// NanoMenuXmb.cpp's RetroArch launch: the core before the app, unlike the hand-off above.
TEST_F(LaunchCommit, RetroArchLaunchKeepsTheCoreBeforeTheApp) {
    commitLaunch(LaunchTuple()
            .romPath("")
            .game("snes", "/r/a.sfc", "/c/snes9x.so")
            .core("/c/snes9x.so")
            .app("com.retroarch.aarch64")
            .intent(""));
    EXPECT_EQ(take(), std::string(kRomPathEmpty) +
                              "launch_system=snes\n"
                              "launch_game=/r/a.sfc\n"
                              "launch_game_core=/c/snes9x.so\n"
                              "launch_core=/c/snes9x.so\n"
                              "launch_app=com.retroarch.aarch64\n"
                              "launch_intent=\n");
}

// A game without a core (a standalone emulator): setLaunchGame's default core, "".
TEST_F(LaunchCommit, GameWithoutACore) {
    commitLaunch(LaunchTuple().game("psp", "/r/b.iso"));
    EXPECT_EQ(take(), "launch_system=psp\nlaunch_game=/r/b.iso\nlaunch_game_core=\n");
}

// NanoMenu.cpp's drastic Quick Resume hand-off: three commits around the keys the site writes
// between them (xmb_return_*, app_drawn), which stay at the site.
TEST_F(LaunchCommit, QuickResumeHandOffAroundTheSitesOtherWrites) {
    commitLaunch(LaunchTuple()
            .app("com.dsemu.drastic")
            .intent("file")
            .rom("")
            .core(""));
    property_set("sys.gammaos.nano.xmb_return_sys", "-1");
    property_set("sys.gammaos.nano.xmb_return_game", "0");
    commitLaunch(LaunchTuple()
            .returnRecent("0")
            .dropInput("1"));
    property_set("sys.gammaos.nano.app_drawn", "0");
    commitLaunch(LaunchTuple()
            .handoffFired("1")
            .pendingExit("0")
            .doLaunch("1"));
    EXPECT_EQ(take(), "launch_app=com.dsemu.drastic\n"
                      "launch_intent=file\n"
                      "launch_rom=\n"
                      "launch_core=\n"
                      "xmb_return_sys=-1\n"
                      "xmb_return_game=0\n"
                      "return_recent=0\n"
                      "drop_input=1\n"
                      "app_drawn=0\n"
                      "handoff_fired=1\n"
                      "pending_exit=0\n"
                      "do_launch=1\n");
}

// Nothing is merged or skipped: a repeated key and a key set to the value it has are written again
// (Android's property triggers fire on every set), and "" is written, not left out.
TEST_F(LaunchCommit, NothingMergedOrSkipped) {
    commitLaunch(LaunchTuple().dropInput("1"));
    commitLaunch(LaunchTuple().dropInput("1").app("x").app("").retroarch("1"));
    EXPECT_EQ(take(), "drop_input=1\n"
                      "drop_input=1\n"
                      "launch_app=x\n"
                      "launch_app=\n"
                      "service.bootanim.nano_retroarch=1\n");
}

// The values are the call site's as it builds the tuple: a buffer changed afterwards does not
// change what is written (the sites commit right after building, as they wrote before).
TEST_F(LaunchCommit, ValuesAreTakenWhenTheTupleIsBuilt) {
    char pkg[PROPERTY_VALUE_MAX] = "com.example.app";
    LaunchTuple t = LaunchTuple().app(pkg).launchedPkg(pkg);
    strcpy(pkg, "changed");
    commitLaunch(t);
    EXPECT_EQ(take(), "launch_app=com.example.app\nlaunched_pkg=com.example.app\n");
}

// The firmware launches only through the handshake: launch() refuses, and there is no child.
TEST_F(LaunchCommit, TheFirmwareLauncherOnlyCommits) {
    nanoplat::Launcher& l = *nanoplat::get().launcher;
    EXPECT_TRUE(l.nanoExitsOnLaunch());
    nanoplat::LaunchRequest r;
    r.package = "com.retroarch.aarch64";
    std::string err;
    EXPECT_FALSE(l.launch(r, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(l.childRunning());
    EXPECT_FALSE(l.pollChild().has_value());
    l.requestChildQuit();
    EXPECT_EQ(take(), "");  // none of it writes a key
}

}  // namespace
