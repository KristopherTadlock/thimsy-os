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

// The control keys (ControlKeys.h) on a scratch property store.

#include "ControlKeys.h"

#include <string.h>

#include <set>
#include <string>
#include <vector>

#include <cutils/properties.h>

#include "compat_test_util.h"

using namespace nanolinux::controlkeys;

namespace {

std::string get(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "");
    return v;
}

class ControlKeysTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(dir)));
        Handlers h;
        h.sessionRequest = [this](const std::string& v) { requests.push_back(v); };
        h.launch = [this](const LaunchRequest& r) { launches.push_back(r); };
        h.runHook = [this](const std::string& a) { hooks.push_back(a); };
        h.nowMs = [this]() { return now; };
        install(h);
    }
    void TearDown() override { uninstall(); }

    compat_test::ScratchDir dir;
    std::vector<std::string> requests, hooks;
    std::vector<LaunchRequest> launches;
    long long now = 1000;
};

}  // namespace

TEST(ControlKeysTable, RowsAreWellFormed) {
    const std::set<std::string> verbs = {"sleep", "poweroff", "reboot", "quit-game", "scrape", "restart-nano"};
    std::set<std::string> seen;
    for (size_t i = 0; i < kControlRowCount; i++) {
        const Row& r = kControlRows[i];
        EXPECT_GT(strlen(r.key), 0u);
        EXPECT_TRUE(seen.insert(std::string(r.key) + "=" + r.value).second) << r.key;
        if (r.action == Action::Session) {
            EXPECT_TRUE(verbs.count(r.arg) == 1) << r.arg;
        } else if (r.action == Action::Hook) {
            EXPECT_GT(strlen(r.arg), 0u);
        } else {
            EXPECT_STREQ(r.arg, "");
        }
    }
}

TEST(ControlKeysRequest, Sequence) {
    EXPECT_EQ(nextRequest("", "sleep"), std::string("1:sleep"));
    EXPECT_EQ(nextRequest("41:restart-nano", "sleep"), std::string("42:sleep"));
    EXPECT_EQ(nextRequest("sleep", "sleep"), std::string("1:sleep"));
    EXPECT_EQ(nextRequest("7", "scrape"), std::string("1:scrape"));
}

TEST_F(ControlKeysTest, DosleepRequestsSleepAndResets) {
    property_set("sys.gammaos.nano.dosleep", "1");
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], std::string("sleep"));
    EXPECT_EQ(get("sys.gammaos.nano.dosleep"), std::string("0"));
    property_set("sys.gammaos.nano.dosleep", "0");   // Nano's own clear on wake
    EXPECT_EQ(requests.size(), 1u);
}

TEST_F(ControlKeysTest, ParkRestartRestartsNano) {
    property_set("sys.gammaos.nano.park_restart", "1");
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], std::string("restart-nano"));
    EXPECT_EQ(get("sys.gammaos.nano.park_restart"), std::string("0"));
}

TEST_F(ControlKeysTest, DowakeOnlyResets) {
    property_set("sys.gammaos.nano.dowake", "1");
    EXPECT_TRUE(requests.empty());
    EXPECT_EQ(get("sys.gammaos.nano.dowake"), std::string("0"));
}

TEST_F(ControlKeysTest, FixedperfRunsTheFamilyHook) {
    property_set("sys.gammaos.nano.fixedperf", "1");
    property_set("sys.gammaos.nano.fixedperf", "0");
    std::vector<std::string> want = {"fixedperf-on", "fixedperf-off"};
    EXPECT_EQ(hooks, want);
    EXPECT_EQ(get("sys.gammaos.nano.fixedperf"), std::string("0"));   // the store keeps the write
}

TEST_F(ControlKeysTest, LaunchHandshakeIsOneRequest) {
    property_set("sys.gammaos.nano.launch_app", "org.libretro.RetroArch");
    property_set("sys.gammaos.nano.launch_core", "snes9x");
    property_set("sys.gammaos.nano.launch_rom", "/storage/1234-ABCD/snes/a.sfc");
    property_set("sys.gammaos.nano.do_launch", "1");
    property_set("service.bootanim.nano_retroarch", "1");   // the same launch
    ASSERT_EQ(launches.size(), 1u);
    EXPECT_EQ(launches[0].trigger, std::string("sys.gammaos.nano.do_launch"));
    EXPECT_EQ(launches[0].keys["sys.gammaos.nano.launch_core"], std::string("snes9x"));
    EXPECT_EQ(launches[0].keys["sys.gammaos.nano.launch_rom"], std::string("/storage/1234-ABCD/snes/a.sfc"));
    now += 5000;   // later, the same keys again: a new launch
    property_set("sys.gammaos.nano.do_launch", "1");
    EXPECT_EQ(launches.size(), 2u);
    property_set("sys.gammaos.nano.launch_rom", "/storage/1234-ABCD/snes/b.sfc");
    property_set("service.bootanim.nano_retroarch", "1");   // different keys: a new launch at once
    EXPECT_EQ(launches.size(), 3u);
    property_set("sys.gammaos.nano.do_launch", "0");
    EXPECT_EQ(launches.size(), 3u);
}

TEST_F(ControlKeysTest, FirstFrameOnce) {
    property_set("sys.gammaos.nano.drm_active", "0");
    EXPECT_EQ(get("sys.thimsy.nano.first_frame"), std::string());
    property_set("sys.gammaos.nano.drm_active", "1");
    EXPECT_EQ(get("sys.thimsy.nano.first_frame"), std::string("1"));
    EXPECT_EQ(get("sys.boot_completed"), std::string("1"));
    property_set("sys.thimsy.nano.first_frame", "0");
    property_set("sys.gammaos.nano.drm_active", "1");   // re-acquired after a game: not a first frame
    EXPECT_EQ(get("sys.thimsy.nano.first_frame"), std::string("0"));
}

TEST_F(ControlKeysTest, AndroidOnlyKeysAreRefusedOnce) {
    property_set("ctl.start", "bootanim");
    property_set("ctl.start", "bootanim");
    property_set("ctl.restart", "nano-typec-guard");
    EXPECT_EQ(refusedCount(), 2u);
    EXPECT_EQ(get("ctl.start"), std::string("bootanim"));   // still stored
    property_set("sys.gammaos.nano.show_overlay", "0");     // a clear is not a message
    EXPECT_EQ(refusedCount(), 2u);
    property_set("sys.gammaos.nano.show_overlay", "1");
    EXPECT_EQ(refusedCount(), 3u);
    EXPECT_TRUE(requests.empty() && launches.empty() && hooks.empty());
}

TEST_F(ControlKeysTest, NanoActionRebootAndShutdownAreRequests) {
    // NanoMenu::prepareShutdown ends in nano_action=<action>: the menu's Reboot and Power Off,
    // the PS3 quick actions, the power-hold and the DRM-sleep timeout.
    property_set("service.bootanim.nano_action", "shutdown");
    property_set("service.bootanim.nano_action", "reboot");
    std::vector<std::string> want = {"poweroff", "reboot"};
    EXPECT_EQ(requests, want);
    EXPECT_EQ(refusedCount(), 0u);
    // Android, recovery and safe mode have no Linux counterpart.
    property_set("service.bootanim.nano_action", "android");
    property_set("service.bootanim.nano_action", "recovery");
    EXPECT_EQ(refusedCount(), 1u);
    EXPECT_EQ(requests, want);
    EXPECT_TRUE(launches.empty() && hooks.empty());
}

TEST_F(ControlKeysTest, DefaultSessionRequestWritesTheKey) {
    uninstall();
    Handlers h;
    h.runHook = [this](const std::string& a) { hooks.push_back(a); };
    install(h);
    property_set("sys.gammaos.nano.dosleep", "1");
    EXPECT_EQ(get("sys.thimsy.session.request"), std::string("1:sleep"));
    property_set("sys.gammaos.nano.park_restart", "1");
    EXPECT_EQ(get("sys.thimsy.session.request"), std::string("2:restart-nano"));
    // The default launcher only logs.
    property_set("sys.gammaos.nano.do_launch", "1");
}
