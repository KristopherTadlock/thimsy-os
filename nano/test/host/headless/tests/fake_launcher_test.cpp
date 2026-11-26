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

// The host's fake launcher (platform/host/FakeLauncher.h) on a scratch property store: the writes
// in the call site's order, a launch at each trigger (one for do_launch and then nano_retroarch),
// the request read from the store, the {"ev":"launch"} event, and the scripted child.

#include <signal.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <nanocompat/nanocompat.h>

#include "NanoLaunchCommit.h"
#include "headless_test_util.h"
#include "platform/host/FakeLauncher.h"
#include "platform/host/HostControl.h"

using android::LaunchTuple;
using nanoplat::LaunchRequest;
using namespace nanohost;

namespace {

class FakeLauncherTest : public ::testing::Test {
 protected:
    void SetUp() override {
        nanocompat::InitOptions o;
        o.persistFile = dir / "persist.props";
        o.runDir = dir / "run";
        o.watch = false;
        std::string err;
        ASSERT_TRUE(nanocompat::init(o, &err)) << err;
        events = dir / "events.jsonl";
        ASSERT_TRUE(openEvents(events));
        id = nanocompat::addPropertyObserver("sys.gammaos.nano.launch_app", [this](std::string_view, std::string_view v) {
            apps.push_back(std::string(v));
        });
    }
    void TearDown() override {
        nanocompat::removePropertyObserver(id);
        openEvents("");
    }

    // The XMB's RetroArch launch and its hand-off, as NanoMenuXmb.cpp and NanoMenu.cpp commit them.
    void retroarchLaunch(nanoplat::Launcher& l, const std::string& rom) {
        l.commit(LaunchTuple()
                         .romPath(rom)
                         .game("snes", rom, "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so")
                         .core("/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so")
                         .app("com.retroarch.aarch64")
                         .intent(""));
        l.commit(LaunchTuple().handoffFired("1").pendingExit("0").dropInput("1"));
        l.commit(LaunchTuple().doLaunch("1"));
        l.commit(LaunchTuple().retroarch("1"));
    }

    headless_test::TmpDir dir;
    std::string events;
    int id = -1;
    std::vector<std::string> apps;
};

TEST_F(FakeLauncherTest, OneLaunchForTheHandshake) {
    std::unique_ptr<nanoplat::Launcher> l = createFakeLauncher();
    EXPECT_FALSE(l->nanoExitsOnLaunch());
    EXPECT_TRUE(launchRecord().requests.empty());
    retroarchLaunch(*l, "/storage/1234-ABCD/ROMs/snes/Test Game.sfc");
    const LaunchRecord r = launchRecord();
    ASSERT_EQ(r.requests.size(), 1u);
    const LaunchRequest& q = r.requests[0];
    EXPECT_TRUE(q.kind == LaunchRequest::Kind::Core);
    EXPECT_EQ(q.package, "com.retroarch.aarch64");
    EXPECT_EQ(q.core, "/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so");
    EXPECT_EQ(q.rom, "/storage/1234-ABCD/ROMs/snes/Test Game.sfc");
    EXPECT_EQ(q.system, "snes");
    EXPECT_EQ(q.trigger, "sys.gammaos.nano.do_launch");
    // The store keeps the handshake, as the firmware's writes leave it.
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("service.bootanim.nano_retroarch", v, "");
    EXPECT_STREQ(v, "1");
    EXPECT_EQ(apps, (std::vector<std::string>{"com.retroarch.aarch64"}));

    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("{\"ev\":\"launch\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"kind\":\"Core\",\"package\":\"com.retroarch.aarch64\",\"core\":"
                        "\"/data/data/com.retroarch.aarch64/cores/snes9x_libretro_android.so\","
                        "\"rom\":\"/storage/1234-ABCD/ROMs/snes/Test Game.sfc\",\"system\":\"snes\","
                        "\"intent\":\"\",\"trigger\":\"sys.gammaos.nano.do_launch\""),
              std::string::npos)
            << text;
    EXPECT_EQ(text.find("\"ev\":\"launch\"", text.find("\"ev\":\"launch\"") + 1), std::string::npos) << text;
}

TEST_F(FakeLauncherTest, AStandaloneIsAPackageLaunch) {
    std::unique_ptr<nanoplat::Launcher> l = createFakeLauncher();
    // NanoMenuXmb.cpp's standalone launch: the app, the intent file's mark, the game, then the trigger.
    l->commit(LaunchTuple().app("org.ppsspp.ppsspp"));
    l->commit(LaunchTuple().intent("file"));
    l->commit(LaunchTuple().romPath("").game("psp", "/storage/1234-ABCD/ROMs/psp/b.iso").core(""));
    l->commit(LaunchTuple().retroarch("1"));
    const LaunchRecord r = launchRecord();
    ASSERT_EQ(r.requests.size(), 1u);
    EXPECT_TRUE(r.requests[0].kind == LaunchRequest::Kind::Package);
    EXPECT_EQ(r.requests[0].package, "org.ppsspp.ppsspp");
    EXPECT_EQ(r.requests[0].rom, "/storage/1234-ABCD/ROMs/psp/b.iso");
    EXPECT_EQ(r.requests[0].intent, "file");
    EXPECT_EQ(r.requests[0].trigger, "service.bootanim.nano_retroarch");
    EXPECT_NE(headless_test::readFile(events).find("\"kind\":\"Package\""), std::string::npos);
}

TEST_F(FakeLauncherTest, TheTriggerReadsTheStoreAtItsWrite) {
    std::unique_ptr<nanoplat::Launcher> l = createFakeLauncher();
    // The drastic SF hand-off writes drop_input after the trigger; an app written after it in the
    // same tuple is not part of the launch, as on Linux, where the control keys fire at the write.
    l->commit(LaunchTuple().app("com.gammaos.drasticsf").core("").retroarch("1").app("later").dropInput("1"));
    ASSERT_EQ(launchRecord().requests.size(), 1u);
    EXPECT_EQ(launchRecord().requests[0].package, "com.gammaos.drasticsf");
    EXPECT_EQ(apps, (std::vector<std::string>{"com.gammaos.drasticsf", "later"}));
    // A trigger set to anything but 1 launches nothing.
    l->commit(LaunchTuple().doLaunch("0").retroarch("0"));
    EXPECT_EQ(launchRecord().requests.size(), 1u);
    // Another request is another launch, at once.
    l->commit(LaunchTuple().app("com.example.other").doLaunch("1"));
    ASSERT_EQ(launchRecord().requests.size(), 2u);
    EXPECT_EQ(launchRecord().requests[1].package, "com.example.other");
}

TEST_F(FakeLauncherTest, TheScriptedChild) {
    FakeLauncherOptions o;
    o.childExitAfterMs = 150;
    o.childExitStatus = 3;
    std::unique_ptr<nanoplat::Launcher> l = createFakeLauncher(o);
    EXPECT_FALSE(l->childRunning());
    EXPECT_FALSE(l->pollChild().has_value());
    LaunchRequest q;
    q.kind = LaunchRequest::Kind::Package;
    q.package = "com.retroarch.aarch64";
    std::string err;
    ASSERT_TRUE(l->launch(q, &err));
    EXPECT_TRUE(l->childRunning());
    EXPECT_FALSE(l->launch(q, &err));  // one child at a time
    EXPECT_NE(err.find("already running"), std::string::npos);
    EXPECT_FALSE(l->pollChild().has_value());
    usleep(200 * 1000);
    std::optional<nanoplat::ChildExit> e = l->pollChild();
    ASSERT_TRUE(e.has_value());
    EXPECT_FALSE(e->signaled);
    EXPECT_EQ(e->status, 3);
    EXPECT_FALSE(l->childRunning());
    EXPECT_FALSE(l->pollChild().has_value());

    // A quit ends it at the next poll, as SIGTERM would.
    ASSERT_TRUE(l->launch(q, &err));
    l->requestChildQuit();
    e = l->pollChild();
    ASSERT_TRUE(e.has_value());
    EXPECT_TRUE(e->signaled);
    EXPECT_EQ(e->status, SIGTERM);
    l->requestChildQuit();  // no child: not counted
    const LaunchRecord r = launchRecord();
    EXPECT_EQ(r.requests.size(), 2u);
    EXPECT_EQ(r.quits, 1);
    EXPECT_NE(headless_test::readFile(events).find("{\"ev\":\"launch_quit\""), std::string::npos);
}

// A ROM on a long card path (over a property's 91 bytes) after a launch on a short one: the request
// has the long path, not the store's older value.
TEST_F(FakeLauncherTest, ALongRomPathAfterAShortOne) {
    std::unique_ptr<nanoplat::Launcher> l = createFakeLauncher();
    retroarchLaunch(*l, "/storage/1234-ABCD/ROMs/snes/a.sfc");
    const std::string longRom = "/storage/1234-ABCD/ROMs/snes/Super Long Game Name - " + std::string(70, 'x') + ".sfc";
    retroarchLaunch(*l, longRom);
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("sys.gammaos.nano.launch_rom", v, "");
    EXPECT_STREQ(v, "/storage/1234-ABCD/ROMs/snes/a.sfc");  // the store refused the long path
    const LaunchRecord r = launchRecord();
    ASSERT_EQ(r.requests.size(), 2u);
    EXPECT_EQ(r.requests[1].rom, longRom);
    EXPECT_NE(headless_test::readFile(events).find(longRom), std::string::npos);
}

// The child's end clears what the framework clears when it returns to Nano: a trigger alone after a
// core launch is a package launch, not the last game again.
TEST_F(FakeLauncherTest, TheEndClearsTheHandshake) {
    std::unique_ptr<nanoplat::Launcher> l = createFakeLauncher();
    retroarchLaunch(*l, "/storage/1234-ABCD/ROMs/snes/a.sfc");
    ASSERT_TRUE(l->pollChild().has_value());   // childExitAfterMs 0: it has ended
    for (const char* k : {"sys.gammaos.nano.launch_app", "sys.gammaos.nano.launch_intent",
                          "sys.gammaos.nano.launch_core", "sys.gammaos.nano.launch_rom"}) {
        char v[PROPERTY_VALUE_MAX] = {};
        property_get(k, v, "");
        EXPECT_STREQ(v, "") << k;
    }
    l->commit(LaunchTuple().retroarch("1"));
    const LaunchRecord r = launchRecord();
    ASSERT_EQ(r.requests.size(), 2u);
    EXPECT_TRUE(r.requests[1].kind == LaunchRequest::Kind::Package);
    EXPECT_EQ(r.requests[1].core, "");
    EXPECT_EQ(r.requests[1].package, "");
}

TEST_F(FakeLauncherTest, ExitsOnLaunchIsScripted) {
    FakeLauncherOptions o;
    o.exitsOnLaunch = true;
    EXPECT_TRUE(createFakeLauncher(o)->nanoExitsOnLaunch());
}

}  // namespace
