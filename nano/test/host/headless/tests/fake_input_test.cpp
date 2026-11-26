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

// The host's fake input policy (platform/host/FakeInput.h) through the real call path: this program
// links with -Wl,--wrap=ioctl as nano-headless does, so the grabs it makes on a fake device (a
// FIFO, FakeEvdev.h) reach FakeEvdev's grab observer, as the boot_home scenario's {"ev":"grab"}.

#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <nanocompat/nanocompat.h>

#include "headless_test_util.h"
#include "platform/host/FakeEvdev.h"
#include "platform/host/FakeInput.h"
#include "platform/host/HostControl.h"

using namespace nanohost;
using headless_test::TmpDir;
using Opened = nanoplat::InputPolicy::Opened;

namespace {

struct FakeInputTest : public ::testing::Test {
    TmpDir dir;
    std::string node;
    int fd = -1;
    std::vector<std::pair<std::string, bool>> grabs;

    void SetUp() override {
        nanocompat::InitOptions o;
        o.persistFile = dir / "persist.props";
        o.runDir = dir / "run";
        o.watch = false;
        std::string err;
        ASSERT_TRUE(nanocompat::init(o, &err)) << err;

        const std::string fifo = dir / "event0";
        ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
        char real[PATH_MAX];
        ASSERT_TRUE(realpath(fifo.c_str(), real) != nullptr);
        node = real;
        evdev::Device pad;
        pad.path = node;
        pad.name = "Thimsy Gamepad";
        pad.keys = {BTN_SOUTH};
        evdev::setDevices({pad});
        evdev::setGrabObserver([this](const std::string& p, bool on) { grabs.emplace_back(p, on); });
        fd = open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);  // as NanoMenuInput.cpp does
        ASSERT_GE(fd, 0);
    }
    void TearDown() override {
        if (fd >= 0) close(fd);
        evdev::setDevices({});
        evdev::setGrabObserver(nullptr);
    }
};

TEST_F(FakeInputTest, GrabsAsTheFirmwareWhenGrabInputIsSet) {
    ASSERT_EQ(property_set("persist.gammaos.nano.grab_input", "1"), 0);
    std::unique_ptr<nanoplat::InputPolicy> in = createFakeInput();
    in->deviceOpened(fd, node.c_str(), Opened::AtStart);
    in->deviceOpened(fd, node.c_str(), Opened::Hotplugged);
    in->deviceOpened(fd, node.c_str(), Opened::Swept);
    in->deviceClosing(fd);
    ASSERT_EQ(grabs.size(), 4u);
    EXPECT_EQ(grabs[0].first, node);
    EXPECT_TRUE(grabs[0].second);
    EXPECT_TRUE(grabs[1].second);
    EXPECT_TRUE(grabs[2].second);
    EXPECT_FALSE(grabs[3].second);  // released before Nano closes it
    InputRecord r = inputRecord();
    EXPECT_EQ(r.opened, 3);
    EXPECT_EQ(r.grabs, 3);
    EXPECT_EQ(r.closing, 1);
}

TEST_F(FakeInputTest, NoGrabWithoutGrabInput) {
    std::unique_ptr<nanoplat::InputPolicy> in = createFakeInput();
    in->deviceOpened(fd, node.c_str(), Opened::AtStart);
    ASSERT_EQ(property_set("persist.gammaos.nano.grab_input", "0"), 0);
    in->deviceOpened(fd, node.c_str(), Opened::Hotplugged);
    EXPECT_TRUE(grabs.empty());
    in->deviceClosing(fd);  // the release is unconditional, as the firmware's
    ASSERT_EQ(grabs.size(), 1u);
    EXPECT_FALSE(grabs[0].second);
    EXPECT_EQ(inputRecord().opened, 2);
    EXPECT_EQ(inputRecord().grabs, 0);
}

TEST_F(FakeInputTest, SilenceIsRecordedAndReported) {
    const std::string events = dir / "events.jsonl";
    ASSERT_TRUE(openEvents(events));
    std::unique_ptr<nanoplat::InputPolicy> in = createFakeInput();
    EXPECT_EQ(inputRecord().silenceCalls, 0);  // a new fake starts a new record
    in->silenceGamePad(true);
    EXPECT_TRUE(inputRecord().silenced);
    in->silenceGamePad(false);
    InputRecord r = inputRecord();
    EXPECT_FALSE(r.silenced);
    EXPECT_EQ(r.silenceCalls, 2);
    EXPECT_TRUE(grabs.empty());  // no padd on the host: nothing is grabbed
    const std::string text = headless_test::readFile(events);
    EXPECT_NE(text.find("{\"ev\":\"input\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"op\":\"silence\",\"on\":true}"), std::string::npos) << text;
    EXPECT_NE(text.find("\"op\":\"silence\",\"on\":false}"), std::string::npos) << text;
    ASSERT_TRUE(openEvents(""));
}

}  // namespace
