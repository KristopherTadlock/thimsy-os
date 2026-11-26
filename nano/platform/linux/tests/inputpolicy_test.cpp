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

// The Linux input policy (InputPolicy.h) on a scratch property store, against a stand-in for
// padd's tap: a SOCK_SEQPACKET socket in the scratch directory that sends packets the way padd's
// TapServer does (8-byte {type, code, value} records, one packet per batch).

#include "InputPolicy.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <cutils/properties.h>

#include "compat_test_util.h"

using namespace nanolinux::input;

namespace {

std::string intercept() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kInterceptKey, v, "<unset>");
    return v;
}

#pragma pack(push, 1)
struct Record {
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 8, "padd's TapRecord is 8 bytes");

// padd's side of the tap.
class Tap {
 public:
    explicit Tap(const std::string& path) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof addr.sun_path) return;
        memcpy(addr.sun_path, path.c_str(), path.size());
        listen_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_ < 0) return;
        if (bind(listen_, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0 || listen(listen_, 4) != 0) {
            close(listen_);
            listen_ = -1;
        }
        path_ = path;
    }
    ~Tap() {
        dropClient();
        if (listen_ >= 0) {
            close(listen_);
            unlink(path_.c_str());
        }
    }
    bool ok() const { return listen_ >= 0; }
    bool accept() {
        client_ = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
        return client_ >= 0;
    }
    bool send(const std::vector<Record>& batch) {
        ssize_t n = ::send(client_, batch.data(), batch.size() * sizeof(Record), MSG_NOSIGNAL);
        return n == static_cast<ssize_t>(batch.size() * sizeof(Record));
    }
    bool sendRaw(const void* data, size_t size) {
        return ::send(client_, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
    }
    void dropClient() {
        if (client_ >= 0) close(client_);
        client_ = -1;
    }

 private:
    std::string path_;
    int listen_ = -1;
    int client_ = -1;
};

class InputPolicyTest : public compat_test::StoreTest {
 protected:
    std::unique_ptr<LinuxInputPolicy> make() {
        Options o;
        o.tapPath = tapPath;
        return create(o);
    }
    std::string tapPath = dir_ / "padd-tap.sock";
};

TEST(LinuxInputPolicyKeys, PaddsNames) {
    // linux/src/thimsy-padd/src/io/Control.h's kInterceptKey and kDefaultTapPath.
    EXPECT_STREQ(kInterceptKey, "sys.thimsy.padd.intercept");
    EXPECT_STREQ(kDefaultTapPath, "/run/thimsy/padd-tap.sock");
}

TEST_F(InputPolicyTest, DevicesAreLeftAlone) {
    std::unique_ptr<LinuxInputPolicy> p = make();
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    // Nothing is asked of the descriptor (a grab on a pipe would fail and log); nothing is set.
    p->deviceOpened(fds[0], "/dev/input/event0", nanoplat::InputPolicy::Opened::AtStart);
    p->deviceOpened(fds[0], "/dev/input/event0", nanoplat::InputPolicy::Opened::Hotplugged);
    p->deviceOpened(fds[0], "/dev/input/event0", nanoplat::InputPolicy::Opened::Swept);
    p->deviceClosing(fds[0]);
    EXPECT_EQ(fcntl(fds[0], F_GETFD), 0);  // still open, flags untouched
    EXPECT_EQ(intercept(), "<unset>");
    EXPECT_FALSE(p->silenced());
    close(fds[0]);
    close(fds[1]);
}

TEST_F(InputPolicyTest, CreateClearsAStaleIntercept) {
    property_set(kInterceptKey, "1");
    std::unique_ptr<LinuxInputPolicy> p = make();
    EXPECT_EQ(intercept(), "0");
    EXPECT_FALSE(p->silenced());
}

TEST_F(InputPolicyTest, CreateLeavesAnUnsetKeyUnset) {
    std::unique_ptr<LinuxInputPolicy> p = make();
    EXPECT_EQ(intercept(), "<unset>");
}

TEST_F(InputPolicyTest, SilenceSetsTheKeyAndConnectsTheTap) {
    Tap tap(tapPath);
    ASSERT_TRUE(tap.ok());
    std::unique_ptr<LinuxInputPolicy> p = make();
    p->silenceGamePad(true);
    EXPECT_TRUE(p->silenced());
    EXPECT_EQ(intercept(), "1");
    EXPECT_GE(p->tapFd(), 0);
    EXPECT_TRUE(tap.accept());  // connected before the key was set

    p->silenceGamePad(true);  // again: nothing changes
    EXPECT_EQ(intercept(), "1");

    p->silenceGamePad(false);
    EXPECT_FALSE(p->silenced());
    EXPECT_EQ(intercept(), "0");
    EXPECT_EQ(p->tapFd(), -1);
}

TEST_F(InputPolicyTest, NoTapLeavesThePadToTheGame) {
    std::unique_ptr<LinuxInputPolicy> p = make();  // nothing listens at tapPath
    p->silenceGamePad(true);
    EXPECT_FALSE(p->silenced());
    EXPECT_EQ(intercept(), "<unset>");
    EXPECT_EQ(p->tapFd(), -1);
    std::vector<struct input_event> evs;
    EXPECT_EQ(p->readTap(&evs), 0u);
}

TEST_F(InputPolicyTest, ReadTapTurnsPacketsIntoEvents) {
    Tap tap(tapPath);
    ASSERT_TRUE(tap.ok());
    std::unique_ptr<LinuxInputPolicy> p = make();
    p->silenceGamePad(true);
    ASSERT_TRUE(tap.accept());

    std::vector<struct input_event> evs;
    EXPECT_EQ(p->readTap(&evs), 0u);  // nothing yet; never blocks

    ASSERT_TRUE(tap.send({{EV_KEY, BTN_SOUTH, 1}, {EV_SYN, SYN_REPORT, 0}}));
    ASSERT_TRUE(tap.send({{EV_ABS, ABS_HAT0Y, -1}, {EV_KEY, BTN_SOUTH, 0}, {EV_SYN, SYN_REPORT, 0}}));
    ASSERT_EQ(p->readTap(&evs), 5u);
    ASSERT_EQ(evs.size(), 5u);
    EXPECT_EQ(evs[0].type, EV_KEY);
    EXPECT_EQ(evs[0].code, BTN_SOUTH);
    EXPECT_EQ(evs[0].value, 1);
    EXPECT_EQ(evs[1].type, EV_SYN);
    EXPECT_EQ(evs[1].code, SYN_REPORT);
    EXPECT_EQ(evs[2].type, EV_ABS);
    EXPECT_EQ(evs[2].code, ABS_HAT0Y);
    EXPECT_EQ(evs[2].value, -1);
    EXPECT_EQ(evs[3].value, 0);
    EXPECT_EQ(evs[4].type, EV_SYN);
    EXPECT_GT(evs[0].input_event_sec, 0);  // stamped when read

    // Appends; a record cut short at the packet's end is dropped.
    const Record whole[2] = {{EV_KEY, BTN_EAST, 1}, {EV_SYN, SYN_REPORT, 0}};
    unsigned char raw[sizeof whole + 3];
    memcpy(raw, whole, sizeof whole);
    memset(raw + sizeof whole, 0x7f, 3);
    ASSERT_TRUE(tap.sendRaw(raw, sizeof raw));
    EXPECT_EQ(p->readTap(&evs), 2u);
    ASSERT_EQ(evs.size(), 7u);
    EXPECT_EQ(evs[5].code, BTN_EAST);
}

TEST_F(InputPolicyTest, NotSilencedReadsNothing) {
    Tap tap(tapPath);
    ASSERT_TRUE(tap.ok());
    std::unique_ptr<LinuxInputPolicy> p = make();
    std::vector<struct input_event> evs;
    EXPECT_EQ(p->readTap(&evs), 0u);
    EXPECT_FALSE(tap.accept());  // never connected
}

TEST_F(InputPolicyTest, ReconnectsWhenPaddComesBack) {
    std::unique_ptr<Tap> tap(new Tap(tapPath));
    ASSERT_TRUE(tap->ok());
    std::unique_ptr<LinuxInputPolicy> p = make();
    p->silenceGamePad(true);
    ASSERT_TRUE(tap->accept());

    tap.reset();  // padd exits: the client sees the end of the stream
    std::vector<struct input_event> evs;
    EXPECT_EQ(p->readTap(&evs), 0u);
    EXPECT_EQ(p->tapFd(), -1);
    EXPECT_TRUE(p->silenced());  // the key stays ours
    EXPECT_EQ(intercept(), "1");
    EXPECT_EQ(p->readTap(&evs), 0u);  // still no padd: nothing, no error

    tap.reset(new Tap(tapPath));  // a restarted padd
    ASSERT_TRUE(tap->ok());
    EXPECT_EQ(p->readTap(&evs), 0u);  // reconnects
    EXPECT_GE(p->tapFd(), 0);
    ASSERT_TRUE(tap->accept());
    ASSERT_TRUE(tap->send({{EV_KEY, BTN_START, 1}, {EV_SYN, SYN_REPORT, 0}}));
    EXPECT_EQ(p->readTap(&evs), 2u);
}

TEST_F(InputPolicyTest, DestroyGivesThePadBack) {
    Tap tap(tapPath);
    ASSERT_TRUE(tap.ok());
    {
        std::unique_ptr<LinuxInputPolicy> p = make();
        p->silenceGamePad(true);
        ASSERT_EQ(intercept(), "1");
    }
    EXPECT_EQ(intercept(), "0");
}

TEST_F(InputPolicyTest, ATooLongTapPathFails) {
    Options o;
    o.tapPath = std::string(200, 'x');
    std::unique_ptr<LinuxInputPolicy> p = create(o);
    p->silenceGamePad(true);
    EXPECT_FALSE(p->silenced());
    EXPECT_EQ(intercept(), "<unset>");
}

}  // namespace
