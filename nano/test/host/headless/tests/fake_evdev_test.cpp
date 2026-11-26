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

// FakeEvdev through the real call path: this program links with -Wl,--wrap=ioctl as nano-headless
// does, opens a FIFO the way Nano opens /dev/input/eventN, and asks it what Nano asks.

#include <errno.h>
#include <limits.h>
#include <linux/input.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/FakeEvdev.h"

using namespace nanohost;
using headless_test::TmpDir;

namespace {

constexpr size_t kBits = 8 * sizeof(unsigned long);
bool testBit(const unsigned long* map, int bit) { return (map[bit / kBits] >> (bit % kBits)) & 1UL; }

struct FakeEvdevTest : public ::testing::Test {
    TmpDir dir;
    std::string node;
    int fd = -1;
    std::vector<std::pair<std::string, bool>> grabs;

    void SetUp() override {
        const std::string fifo = dir / "event0";
        ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
        char real[PATH_MAX];
        ASSERT_TRUE(realpath(fifo.c_str(), real) != nullptr);
        node = real;
        evdev::Device pad;
        pad.path = node;
        pad.name = "Thimsy Gamepad";
        pad.bus = BUS_VIRTUAL;
        pad.vendor = 0x5448;
        pad.product = 1;
        pad.version = 0x100;
        pad.keys = {BTN_SOUTH, BTN_EAST, BTN_START, KEY_UP, KEY_DOWN};
        evdev::AbsAxis hat;
        hat.code = ABS_HAT0X;
        hat.minimum = -1;
        hat.maximum = 1;
        evdev::AbsAxis stick;
        stick.code = ABS_X;
        stick.minimum = -32768;
        stick.maximum = 32767;
        stick.flat = 128;
        pad.abs = {hat, stick};
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

TEST_F(FakeEvdevTest, Identity) {
    char name[128] = {};
    EXPECT_EQ(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name), (int)strlen("Thimsy Gamepad") + 1);
    EXPECT_STREQ(name, "Thimsy Gamepad");
    char small[7] = {};
    EXPECT_EQ(ioctl(fd, EVIOCGNAME(sizeof(small) - 1), small), 6);  // cut at the caller's size
    EXPECT_EQ(std::string(small, 6), "Thimsy");

    struct input_id id = {};
    EXPECT_EQ(ioctl(fd, EVIOCGID, &id), 0);
    EXPECT_EQ(id.bustype, BUS_VIRTUAL);
    EXPECT_EQ(id.vendor, 0x5448);
    EXPECT_EQ(id.product, 1);
    EXPECT_EQ(id.version, 0x100);

    int version = 0;
    EXPECT_EQ(ioctl(fd, EVIOCGVERSION, &version), 0);
    EXPECT_EQ(version, EV_VERSION);

    char phys[32];
    errno = 0;
    EXPECT_EQ(ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(FakeEvdevTest, Capabilities) {
    unsigned long ev[(EV_MAX + kBits) / kBits] = {};
    EXPECT_GT(ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev), 0);
    EXPECT_TRUE(testBit(ev, EV_SYN));
    EXPECT_TRUE(testBit(ev, EV_KEY));
    EXPECT_TRUE(testBit(ev, EV_ABS));
    EXPECT_FALSE(testBit(ev, EV_REL));

    unsigned long keys[(KEY_MAX + kBits) / kBits] = {};
    EXPECT_EQ(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys), (int)sizeof(keys));
    EXPECT_TRUE(testBit(keys, BTN_SOUTH));
    EXPECT_TRUE(testBit(keys, KEY_UP));
    EXPECT_FALSE(testBit(keys, BTN_NORTH));

    unsigned long abs[(ABS_MAX + kBits) / kBits] = {};
    EXPECT_GT(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs), 0);
    EXPECT_TRUE(testBit(abs, ABS_HAT0X));
    EXPECT_TRUE(testBit(abs, ABS_X));
    EXPECT_FALSE(testBit(abs, ABS_Y));

    struct input_absinfo info = {};
    EXPECT_EQ(ioctl(fd, EVIOCGABS(ABS_X), &info), 0);
    EXPECT_EQ(info.minimum, -32768);
    EXPECT_EQ(info.maximum, 32767);
    EXPECT_EQ(info.flat, 128);
    EXPECT_EQ(ioctl(fd, EVIOCGABS(ABS_Y), &info), 0);  // an axis it lacks: zeros, as the kernel
    EXPECT_EQ(info.maximum, 0);

    unsigned long state[(KEY_MAX + kBits) / kBits];
    memset(state, 0xff, sizeof(state));
    EXPECT_GT(ioctl(fd, EVIOCGKEY(sizeof(state)), state), 0);
    EXPECT_FALSE(testBit(state, BTN_SOUTH));  // nothing held
}

TEST_F(FakeEvdevTest, GrabIsRecorded) {
    EXPECT_EQ(ioctl(fd, EVIOCGRAB, 1), 0);
    EXPECT_EQ(ioctl(fd, EVIOCGRAB, 0), 0);
    ASSERT_EQ(grabs.size(), 2u);
    EXPECT_EQ(grabs[0].first, node);
    EXPECT_TRUE(grabs[0].second);
    EXPECT_FALSE(grabs[1].second);
}

TEST_F(FakeEvdevTest, OtherDescriptorsReachTheRealIoctl) {
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(null, 0);
    char name[16];
    errno = 0;
    EXPECT_EQ(ioctl(null, EVIOCGNAME(sizeof(name)), name), -1);
    EXPECT_EQ(errno, ENOTTY);
    close(null);

    // A FIFO that is no configured device: the kernel's answer (FIONREAD works on a pipe).
    const std::string other = dir / "other";
    ASSERT_EQ(mkfifo(other.c_str(), 0600), 0);
    int ofd = open(other.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_GE(ofd, 0);
    ASSERT_EQ(write(ofd, "abc", 3), 3);
    int avail = -1;
    EXPECT_EQ(ioctl(ofd, FIONREAD, &avail), 0);
    EXPECT_EQ(avail, 3);
    errno = 0;
    EXPECT_EQ(ioctl(ofd, EVIOCGNAME(sizeof(name)), name), -1);
    EXPECT_EQ(errno, ENOTTY);
    close(ofd);
}

TEST_F(FakeEvdevTest, EventsStillFlowThroughTheFifo) {
    int w = open(node.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(w, 0);
    struct input_event in[2] = {};
    in[0].type = EV_KEY;
    in[0].code = BTN_SOUTH;
    in[0].value = 1;
    in[1].type = EV_SYN;
    in[1].code = SYN_REPORT;
    ASSERT_EQ(write(w, in, sizeof(in)), (ssize_t)sizeof(in));
    struct input_event out[2] = {};
    ASSERT_EQ(read(fd, out, sizeof(out)), (ssize_t)sizeof(out));
    EXPECT_EQ(out[0].code, BTN_SOUTH);
    EXPECT_EQ(out[0].value, 1);
    EXPECT_EQ(out[1].type, EV_SYN);
    close(w);
}

}  // namespace
