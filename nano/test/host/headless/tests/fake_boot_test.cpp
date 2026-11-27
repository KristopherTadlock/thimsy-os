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

// The boot answers (nanoplat::Boot, W6-boot) without NanoMenu.cpp: the host's fake
// (platform/host/FakeBoot.h), and the firmware's moved code (platform/firmware/Boot.cpp) against
// the property shims and the binder stubs, so the decision that ended every Linux boot
// (ARCHITECTURE-AUDIT.md defect 1) is checked where it now lives: the firmware's still bails to
// Android's boot animation when persist.bootanim.skip_nano is not "0", and the fake (like Linux's)
// does not.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <nanocompat/nanocompat.h>

#include "NanoPlatform.h"
#include "headless_test_util.h"
#include "platform/firmware/Boot.h"
#include "platform/host/FakeBoot.h"
#include "platform/host/HostControl.h"

using namespace nanohost;
using headless_test::TmpDir;
using Reader = nanoplat::Boot::SettingsReader;

namespace {

constexpr const char* kDeStateFile = "/data/misc/bootanim/nano_skip";  // the firmware's cache

int64_t elapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

class BootTest : public ::testing::Test {
 protected:
    void SetUp() override {
        nanocompat::InitOptions o;
        o.persistFile = dir / "persist.props";
        o.runDir = dir / "run";
        o.watch = false;
        std::string err;
        ASSERT_TRUE(nanocompat::init(o, &err)) << err;
    }

    // Runs `fn` in a child that records every ctl.* write on a pipe; returns the child's wait
    // status and, in *writes, what it wrote ("key=value\n" each).
    int childCtlWrites(const std::function<void()>& fn, std::string* writes) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return -1;
        const int status = headless_test::inChild([&] {
            close(p[0]);
            const int w = p[1];
            nanocompat::addPropertyObserver("ctl.", [w](std::string_view k, std::string_view v) {
                const std::string line = std::string(k) + "=" + std::string(v) + "\n";
                ssize_t n = write(w, line.data(), line.size());
                (void)n;
            });
            fn();
        });
        close(p[1]);
        writes->clear();
        char buf[256];
        ssize_t n;
        while ((n = read(p[0], buf, sizeof buf)) > 0) writes->append(buf, (size_t)n);
        close(p[0]);
        return status;
    }

    TmpDir dir;
};

// --- The host's fake --------------------------------------------------------------------------

TEST_F(BootTest, FakeRunsNanoAndWaitsForNothing) {
    std::unique_ptr<nanoplat::Boot> boot = createFakeBoot(true);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(boot->nanoOwnsBootScreen());
    boot->awaitSavedSettings(Reader::QuickResumeCheck);
    boot->awaitSavedSettings(Reader::MenuSetup);
    boot->awaitSavedSettings(Reader::MenuSetup);
    EXPECT_TRUE(boot->savedSettingsLoaded());
    EXPECT_TRUE(boot->soundSystemReady());
    EXPECT_LT(elapsedMs(t0), 500);
    const BootRecord r = bootRecord();
    EXPECT_EQ(r.ownerChecks, 1);
    EXPECT_EQ(r.quickResumeWaits, 1);
    EXPECT_EQ(r.menuSetupWaits, 2);
    EXPECT_EQ(r.loadedChecks, 1);
    EXPECT_EQ(r.soundChecks, 1);
    EXPECT_EQ(r.handOvers, 0);
}

TEST_F(BootTest, FakeIgnoresAndroidsBootChoice) {
    ASSERT_EQ(property_set("persist.bootanim.skip_nano", "1"), 0);   // what ended every Linux boot
    std::unique_ptr<nanoplat::Boot> boot = createFakeBoot(true);
    EXPECT_TRUE(boot->nanoOwnsBootScreen());
}

TEST_F(BootTest, FakeHandOverReportsAndExits) {
    const std::string events = dir / "events.jsonl";
    ASSERT_TRUE(openEvents(events));
    std::unique_ptr<nanoplat::Boot> boot = createFakeBoot(false);   // "nano_owns_boot_screen": false
    EXPECT_FALSE(boot->nanoOwnsBootScreen());
    int fds[2];
    ASSERT_EQ(pipe2(fds, O_CLOEXEC), 0);
    const int status = headless_test::inChild([&] {
        boot->handOverBootScreen(fds[0], false);
        _exit(3);   // not reached: the hand-over ends the process
    });
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("{\"ev\":\"boot\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"op\":\"hand_over\",\"overlay\":false}"), std::string::npos) << text;
    ASSERT_TRUE(openEvents(""));
    close(fds[0]);
    close(fds[1]);
}

// --- The firmware's moved code ------------------------------------------------------------------

TEST_F(BootTest, FirmwareRunsNanoWhenSkipNanoIsZero) {
    if (access(kDeStateFile, F_OK) == 0) GTEST_SKIP() << kDeStateFile << " exists on this host";
    ASSERT_EQ(property_set("ro.persistent_properties.ready", "true"), 0);
    ASSERT_EQ(property_set("persist.bootanim.skip_nano", "0"), 0);
    std::unique_ptr<nanoplat::Boot> boot = nanofirmware::createBoot();
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(boot->nanoOwnsBootScreen());
    EXPECT_TRUE(boot->savedSettingsLoaded());
    boot->awaitSavedSettings(Reader::QuickResumeCheck);   // ready: no wait
    boot->awaitSavedSettings(Reader::MenuSetup);
    EXPECT_LT(elapsedMs(t0), 1000);
}

TEST_F(BootTest, FirmwareHandsTheBootToAndroidOtherwise) {
    if (access(kDeStateFile, F_OK) == 0) GTEST_SKIP() << kDeStateFile << " exists on this host";
    ASSERT_EQ(property_set("ro.persistent_properties.ready", "true"), 0);
    std::unique_ptr<nanoplat::Boot> boot = nanofirmware::createBoot();
    EXPECT_FALSE(boot->nanoOwnsBootScreen());   // unset: not "0"
    ASSERT_EQ(property_set("persist.bootanim.skip_nano", "1"), 0);
    EXPECT_FALSE(boot->nanoOwnsBootScreen());
}

TEST_F(BootTest, FirmwareHandOverStartsBootanimAndExits) {
    std::unique_ptr<nanoplat::Boot> boot = nanofirmware::createBoot();
    std::string writes;
    int status = childCtlWrites([&] { boot->handOverBootScreen(-1, false); _exit(3); }, &writes);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(writes, "ctl.start=bootanim\n");
    // The overlay instance never starts the boot animation; it only exits.
    status = childCtlWrites([&] { boot->handOverBootScreen(-1, true); _exit(3); }, &writes);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(writes, "");
}

TEST_F(BootTest, FirmwareWaitsForPersistentProperties) {
    std::unique_ptr<nanoplat::Boot> boot = nanofirmware::createBoot();
    EXPECT_FALSE(boot->savedSettingsLoaded());
    const auto t0 = std::chrono::steady_clock::now();
    boot->awaitSavedSettings(Reader::QuickResumeCheck);   // 200 x 10 ms, as readyToRun waited
    const int64_t ms = elapsedMs(t0);
    EXPECT_GE(ms, 1900);
    EXPECT_LT(ms, 6000);
}

TEST_F(BootTest, FirmwareSoundSystemIsAndroidsAudioService) {
    // The binder stub's service manager knows no service, so there is no AudioService here.
    EXPECT_FALSE(nanofirmware::createBoot()->soundSystemReady());
}

}  // namespace
