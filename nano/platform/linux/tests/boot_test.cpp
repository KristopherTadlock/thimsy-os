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

// The Linux boot answers (Boot.h) on a scratch property store holding what made Android's decision
// end Nano (ARCHITECTURE-AUDIT.md defect 1): persist.bootanim.skip_nano not "0" and no
// ro.persistent_properties.ready. Nano still owns the screen, at once, and nothing waits.

#include "Boot.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>

#include <cutils/properties.h>

#include "compat_test_util.h"

using compat_test::ScratchDir;
using Reader = nanoplat::Boot::SettingsReader;

namespace {

std::string prop(const char* key) {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(key, v, "<unset>");
    return v;
}

int64_t elapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

class LinuxBootTest : public ::testing::Test {
 protected:
    void SetUp() override {
        std::string err;
        ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(dir), &err)) << err;
        // Android's "boot to Android" choice, as a card carried over from Android might hold it.
        ASSERT_EQ(property_set("persist.bootanim.skip_nano", "1"), 0);
        ASSERT_EQ(prop("ro.persistent_properties.ready"), "<unset>");
        boot = nanolinux::boot::create();
        ASSERT_TRUE(boot != nullptr);
    }

    ScratchDir dir;
    std::unique_ptr<nanoplat::Boot> boot;
};

TEST_F(LinuxBootTest, NanoOwnsTheScreenAtOnce) {
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(boot->nanoOwnsBootScreen());
    EXPECT_TRUE(boot->nanoOwnsBootScreen());   // every boot, every start
    EXPECT_LT(elapsedMs(t0), 500);             // the firmware's would wait 10 s here
    EXPECT_EQ(prop("ctl.start"), "<unset>");
}

TEST_F(LinuxBootTest, SettingsAreLoadedAndNothingWaits) {
    EXPECT_TRUE(boot->savedSettingsLoaded());
    const auto t0 = std::chrono::steady_clock::now();
    boot->awaitSavedSettings(Reader::QuickResumeCheck);   // the firmware's: up to 2 s
    boot->awaitSavedSettings(Reader::MenuSetup);          // the firmware's: up to 3 s
    EXPECT_LT(elapsedMs(t0), 500);
    EXPECT_EQ(prop("ro.persistent_properties.ready"), "<unset>");   // nothing is faked either
}

TEST_F(LinuxBootTest, SoundSystemNeedsNoService) {
    EXPECT_TRUE(boot->soundSystemReady());
}

TEST_F(LinuxBootTest, HandOverTouchesNothingAndReturns) {
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC), 0);
    boot->handOverBootScreen(fds[0], false);   // returns: the process goes on
    boot->handOverBootScreen(-1, true);
    EXPECT_NE(fcntl(fds[0], F_GETFD), -1);     // the fd is still open
    EXPECT_EQ(prop("ctl.start"), "<unset>");   // no service start is asked for
    close(fds[0]);
    close(fds[1]);
}

}  // namespace
