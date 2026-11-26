// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The I/O layer as far as it runs without a device: sysfs listing, event reading from a socket,
// the uinput setup sequence and force-feedback handshake through a recording ioctl, the control
// key through a real config store, and the tap socket. Grabbing, hiding and real uinput need root
// on a device (Phase 0 (c)).
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "io/Control.h"
#include "io/EvdevSource.h"
#include "io/UinputSink.h"
#include "padd_test_util.h"
#include "thimsy/atomic_file.h"
#include "thimsy/config.h"
#include "thimsy/watch.h"

using namespace padd;

namespace {

std::string scratch() {
    std::string tmpl = ::testing::TempDir() + "padd-io-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    return mkdtemp(buf.data()) ? std::string(buf.data()) : std::string();
}

void writeFile(const std::string& path, const std::string& text) {
    std::string dir = path.substr(0, path.rfind('/'));
    thimsy::makeDirs(dir);
    ASSERT_TRUE(thimsy::writeFileAtomic(path, text)) << path;
}

void removeTree(const std::string& p) {
    std::string cmd = "rm -rf '" + p + "'";
    int rc = system(cmd.c_str());
    (void)rc;
}

input_event ev(uint16_t type, uint16_t code, int32_t value) {
    input_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.code = code;
    e.value = value;
    return e;
}

}  // namespace

TEST(Io, ListNodesReadsSysfs) {
    std::string d = scratch();
    ASSERT_FALSE(d.empty());
    writeFile(d + "/class/input/event10/dev", "13:74\n");
    writeFile(d + "/class/input/event10/device/name", "adc-keys-home\n");
    writeFile(d + "/class/input/event2/dev", "13:66\n");
    writeFile(d + "/class/input/event2/device/name", "gpio-keys-control\n");
    writeFile(d + "/class/input/input2/name", "not an event node\n");
    writeFile(d + "/class/input/mouse0/dev", "13:32\n");
    std::vector<NodeEntry> n = listNodes(d);
    ASSERT_EQ(n.size(), 2u);
    EXPECT_EQ(n[0].event, "event2");
    EXPECT_EQ(n[0].name, "gpio-keys-control");
    EXPECT_EQ(n[0].major, 13u);
    EXPECT_EQ(n[0].minor, 66u);
    EXPECT_EQ(n[1].event, "event10");  // numeric order, not text order
    EXPECT_EQ(n[1].name, "adc-keys-home");
    EXPECT_TRUE(listNodes(d + "/nothing").empty());
    removeTree(d);
}

TEST(Io, OpenRefusesWhatIsNotEvdev) {
    std::string d = scratch();
    writeFile(d + "/input/event3", "plain file");
    NodeEntry e;
    e.event = "event3";
    EvdevSource::Paths p;
    p.devInputDir = d + "/input";
    p.privateDir = d + "/private";
    std::string err;
    EXPECT_EQ(EvdevSource::open(e, p, &err), nullptr);
    EXPECT_NE(err.find("not an evdev node"), std::string::npos) << err;
    e.event = "event4";  // missing, and no device number to recreate it with
    EXPECT_EQ(EvdevSource::open(e, p, &err), nullptr);
    EXPECT_NE(err.find("No such file"), std::string::npos) << err;
    removeTree(d);
}

TEST(Io, ReadEventsFromAStream) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    std::unique_ptr<EvdevSource> s = EvdevSource::adopt(sv[0], "test", NodeInfo{}, SourceCaps{});
    std::vector<input_event> in = {ev(EV_KEY, BTN_SOUTH, 1), ev(EV_SYN, SYN_REPORT, 0)};
    in[0].input_event_sec = 3;
    in[0].input_event_usec = 250;
    ASSERT_EQ(write(sv[1], in.data(), in.size() * sizeof in[0]), static_cast<ssize_t>(in.size() * sizeof in[0]));
    std::vector<SourceEvent> out;
    EXPECT_EQ(s->readEvents(2, &out), EvdevSource::Read::Ok);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].source, 2);
    EXPECT_EQ(out[0].type, EV_KEY);
    EXPECT_EQ(out[0].code, BTN_SOUTH);
    EXPECT_EQ(out[0].value, 1);
    EXPECT_EQ(out[0].timeNs, 3000250000LL);
    out.clear();
    EXPECT_EQ(s->readEvents(2, &out), EvdevSource::Read::Ok);  // nothing queued
    EXPECT_TRUE(out.empty());
    close(sv[1]);
    EXPECT_EQ(s->readEvents(2, &out), EvdevSource::Read::Gone);
    EXPECT_TRUE(s->hide());  // adopted: nothing to unlink
}

namespace {

struct Recorder {
    std::vector<std::string> calls;
    uinput_ff_upload upload;
    uinput_ff_erase erase;
    int fail = 0;  // the request that fails, if any
    UinputSink::IoctlFn fn() {
        return [this](int, unsigned long req, void* arg) -> int {
            if (fail != 0 && req == static_cast<unsigned long>(fail)) {
                errno = EINVAL;
                return -1;
            }
            uintptr_t v = reinterpret_cast<uintptr_t>(arg);
            if (req == UI_GET_VERSION) {
                *static_cast<unsigned*>(arg) = 5;
                calls.push_back("version");
            } else if (req == UI_SET_EVBIT) {
                calls.push_back("evbit " + eventTypeName(static_cast<int>(v)));
            } else if (req == UI_SET_KEYBIT) {
                calls.push_back("keybit " + eventCodeName(EV_KEY, static_cast<int>(v)));
            } else if (req == UI_SET_ABSBIT) {
                calls.push_back("absbit " + eventCodeName(EV_ABS, static_cast<int>(v)));
            } else if (req == UI_SET_FFBIT) {
                calls.push_back("ffbit " + std::to_string(v));
            } else if (req == UI_ABS_SETUP) {
                auto* a = static_cast<uinput_abs_setup*>(arg);
                calls.push_back("abs " + eventCodeName(EV_ABS, a->code) + " " + std::to_string(a->absinfo.minimum) +
                                " " + std::to_string(a->absinfo.maximum));
            } else if (req == UI_DEV_SETUP) {
                auto* u = static_cast<uinput_setup*>(arg);
                char buf[160];
                snprintf(buf, sizeof buf, "setup %s %04x:%04x:%04x v%u ff%u", u->name, u->id.bustype, u->id.vendor,
                         u->id.product, u->id.version, u->ff_effects_max);
                calls.push_back(buf);
            } else if (req == UI_DEV_CREATE) {
                calls.push_back("create");
            } else if (req == UI_DEV_DESTROY) {
                calls.push_back("destroy");
            } else if (req == UI_BEGIN_FF_UPLOAD) {
                auto* u = static_cast<uinput_ff_upload*>(arg);
                uint32_t id = u->request_id;
                *u = upload;
                u->request_id = id;
                calls.push_back("begin-upload " + std::to_string(id));
            } else if (req == UI_END_FF_UPLOAD) {
                auto* u = static_cast<uinput_ff_upload*>(arg);
                calls.push_back("end-upload " + std::to_string(u->request_id) + " " + std::to_string(u->retval));
            } else if (req == UI_BEGIN_FF_ERASE) {
                auto* u = static_cast<uinput_ff_erase*>(arg);
                uint32_t id = u->request_id;
                *u = erase;
                u->request_id = id;
                calls.push_back("begin-erase " + std::to_string(id));
            } else if (req == UI_END_FF_ERASE) {
                auto* u = static_cast<uinput_ff_erase*>(arg);
                calls.push_back("end-erase " + std::to_string(u->request_id) + " " + std::to_string(u->retval));
            } else {
                calls.push_back("other");
            }
            return 0;
        };
    }
};

struct Handler : UinputSink::FfHandler {
    std::vector<std::string> log;
    int upload(const RumbleEffect& e) override {
        log.push_back("upload " + std::to_string(e.id) + " " + std::to_string(e.strong) + " " +
                      std::to_string(e.weak) + " " + std::to_string(e.lengthMs));
        return 0;
    }
    int erase(int id) override {
        log.push_back("erase " + std::to_string(id));
        return -EINVAL;
    }
    void play(int id, int count) override { log.push_back("play " + std::to_string(id) + " " + std::to_string(count)); }
    void gain(uint16_t g) override { log.push_back("gain " + std::to_string(g)); }
};

Layout arcLikeLayout() {
    Layout l;
    l.padKeys = {BTN_SOUTH, BTN_MODE};
    l.padAbs[ABS_X] = AbsInfo{0, kStickMin, kStickMax, 0, 0, 0};
    l.padAbs[ABS_HAT0X] = AbsInfo{0, -1, 1, 0, 0, 0};
    l.ff = true;
    return l;
}

}  // namespace

TEST(Io, UinputSetupSequence) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    Recorder rec;
    std::string err;
    {
        std::unique_ptr<UinputSink> s = UinputSink::createOnFd(sv[0], padSetup(arcLikeLayout(), 16), rec.fn(), &err);
        ASSERT_NE(s, nullptr) << err;
    }
    EXPECT_EQ(rec.calls, (std::vector<std::string>{
                             "version", "evbit EV_KEY", "keybit BTN_SOUTH", "keybit BTN_MODE", "evbit EV_ABS",
                             "absbit ABS_X", "abs ABS_X -32768 32767", "absbit ABS_HAT0X", "abs ABS_HAT0X -1 1",
                             "evbit EV_FF", "ffbit 80", "ffbit 96",
                             "setup Thimsy Gamepad 0006:5448:0001 v1 ff16", "create", "destroy"}));
    close(sv[1]);
}

TEST(Io, UinputKeyboardHasNoAxesOrFf) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    Layout l;
    l.keysKeys = {KEY_VOLUMEUP};
    Recorder rec;
    std::string err;
    std::unique_ptr<UinputSink> s = UinputSink::createOnFd(sv[0], keysSetup(l), rec.fn(), &err);
    ASSERT_NE(s, nullptr) << err;
    EXPECT_EQ(rec.calls, (std::vector<std::string>{"version", "evbit EV_KEY", "keybit KEY_VOLUMEUP",
                                                   "setup Thimsy Keys 0006:5448:0002 v1 ff0", "create"}));
    close(sv[1]);
}

TEST(Io, UinputSetupFailureIsReported) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    Recorder rec;
    rec.fail = static_cast<int>(UI_DEV_CREATE);
    std::string err;
    EXPECT_EQ(UinputSink::createOnFd(sv[0], padSetup(arcLikeLayout(), 16), rec.fn(), &err), nullptr);
    EXPECT_NE(err.find("UI_DEV_CREATE"), std::string::npos) << err;
    close(sv[1]);
}

TEST(Io, UinputWritesOnlyItsDevice) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    Recorder rec;
    std::string err;
    std::unique_ptr<UinputSink> s = UinputSink::createOnFd(sv[0], padSetup(arcLikeLayout(), 16), rec.fn(), &err);
    ASSERT_NE(s, nullptr);
    std::vector<OutEvent> evs = {{Dev::Pad, EV_KEY, BTN_SOUTH, 1}, {Dev::Keys, EV_KEY, KEY_VOLUMEUP, 1},
                                 {Dev::Tap, EV_KEY, BTN_MODE, 1}, {Dev::Pad, EV_SYN, SYN_REPORT, 0}};
    ASSERT_TRUE(s->write(evs, Dev::Pad));
    input_event got[4];
    ssize_t n = read(sv[1], got, sizeof got);
    ASSERT_EQ(n, static_cast<ssize_t>(2 * sizeof(input_event)));
    EXPECT_EQ(got[0].code, BTN_SOUTH);
    EXPECT_EQ(got[0].value, 1);
    EXPECT_EQ(got[1].type, EV_SYN);
    close(sv[1]);
}

TEST(Io, UinputForceFeedbackHandshake) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    Recorder rec;
    std::string err;
    std::unique_ptr<UinputSink> s = UinputSink::createOnFd(sv[0], padSetup(arcLikeLayout(), 16), rec.fn(), &err);
    ASSERT_NE(s, nullptr);
    rec.calls.clear();
    memset(&rec.upload, 0, sizeof rec.upload);
    rec.upload.effect.type = FF_RUMBLE;
    rec.upload.effect.id = 3;
    rec.upload.effect.u.rumble.strong_magnitude = 0x8000;
    rec.upload.effect.u.rumble.weak_magnitude = 0x100;
    rec.upload.effect.replay.length = 250;
    memset(&rec.erase, 0, sizeof rec.erase);
    rec.erase.effect_id = 3;
    std::vector<input_event> in = {ev(EV_UINPUT, UI_FF_UPLOAD, 7), ev(EV_FF, 3, 1), ev(EV_FF, FF_GAIN, 0x4000),
                                   ev(EV_FF, FF_AUTOCENTER, 1), ev(EV_UINPUT, UI_FF_ERASE, 8)};
    ASSERT_EQ(write(sv[1], in.data(), in.size() * sizeof in[0]), static_cast<ssize_t>(in.size() * sizeof in[0]));
    Handler h;
    EXPECT_TRUE(s->handleRequests(&h));
    EXPECT_EQ(h.log, (std::vector<std::string>{"upload 3 32768 256 250", "play 3 1", "gain 16384", "erase 3"}));
    EXPECT_EQ(rec.calls, (std::vector<std::string>{"begin-upload 7", "end-upload 7 0", "begin-erase 8",
                                                   "end-erase 8 -22"}));
    // A periodic effect is refused: the pad only advertises FF_RUMBLE.
    rec.calls.clear();
    rec.upload.effect.type = FF_PERIODIC;
    in = {ev(EV_UINPUT, UI_FF_UPLOAD, 9)};
    ASSERT_EQ(write(sv[1], in.data(), sizeof in[0]), static_cast<ssize_t>(sizeof in[0]));
    EXPECT_TRUE(s->handleRequests(&h));
    EXPECT_EQ(rec.calls, (std::vector<std::string>{"begin-upload 9", "end-upload 9 -22"}));
    close(sv[1]);
}

TEST(Io, ControlSeesAnotherWritersIntercept) {
    std::string d = scratch();
    thimsy::ConfigStore::Roots roots;
    roots.persistFile = d + "/persist.props";
    roots.runDir = d + "/run";
    std::string err;
    std::unique_ptr<Control> c = Control::create(roots, nullptr, &err);
    ASSERT_NE(c, nullptr) << err;
    EXPECT_FALSE(c->intercept());
    c->setReady(true);
    std::unique_ptr<thimsy::ConfigStore> other = thimsy::ConfigStore::open(roots, &err);
    ASSERT_NE(other, nullptr) << err;
    EXPECT_EQ(other->get(kReadyKey), "1");
    ASSERT_TRUE(other->set(kInterceptKey, "1"));
    pollfd p = {c->fd(), POLLIN, 0};
    ASSERT_EQ(poll(&p, 1, 5000), 1);  // the watcher thread woke the loop
    c->drain();
    EXPECT_TRUE(c->intercept());
    EXPECT_EQ(poll(&p, 1, 0), 0);  // drained
    ASSERT_TRUE(other->set(kInterceptKey, "0"));
    ASSERT_EQ(poll(&p, 1, 5000), 1);
    c->drain();
    EXPECT_FALSE(c->intercept());
    c->setReady(false);
    EXPECT_EQ(other->get(kReadyKey), "");
    c.reset();
    other.reset();
    removeTree(d);
}

TEST(Io, TapSendsOnePacketPerBatch) {
    std::string d = scratch();
    std::string path = d + "/tap.sock";
    std::string err;
    std::unique_ptr<TapServer> t = TapServer::create(path, &err);
    ASSERT_NE(t, nullptr) << err;
    int c = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    ASSERT_EQ(connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    t->acceptClients();
    EXPECT_EQ(t->clients(), 1u);
    t->send({{Dev::Pad, EV_KEY, BTN_SOUTH, 1}, {Dev::Tap, EV_KEY, BTN_SOUTH, 1}, {Dev::Tap, EV_SYN, SYN_REPORT, 0},
             {Dev::Tap, EV_ABS, ABS_HAT0Y, -1}, {Dev::Tap, EV_SYN, SYN_REPORT, 0}});
    TapRecord r[8];
    ssize_t n = recv(c, r, sizeof r, 0);
    ASSERT_EQ(n, 16);
    EXPECT_EQ(r[0].code, BTN_SOUTH);
    EXPECT_EQ(r[0].value, 1);
    EXPECT_EQ(r[1].code, SYN_REPORT);
    n = recv(c, r, sizeof r, 0);
    ASSERT_EQ(n, 16);
    EXPECT_EQ(r[0].code, ABS_HAT0Y);
    EXPECT_EQ(r[0].value, -1);
    close(c);
    t->send({{Dev::Tap, EV_KEY, BTN_SOUTH, 0}, {Dev::Tap, EV_SYN, SYN_REPORT, 0}});
    EXPECT_EQ(t->clients(), 0u);  // the closed client is dropped
    t.reset();
    struct stat st;
    EXPECT_NE(stat(path.c_str(), &st), 0);  // the socket file is removed
    removeTree(d);
}

TEST(Io, NodeWatchSeesNewNodes) {
    std::string d = scratch();
    std::unique_ptr<NodeWatch> w = NodeWatch::create(d);
    ASSERT_NE(w, nullptr);
    EXPECT_FALSE(w->drain());
    writeFile(d + "/js0", "x");
    EXPECT_FALSE(w->drain());
    writeFile(d + "/event5", "x");
    EXPECT_TRUE(w->drain());
    w.reset();
    removeTree(d);
}
