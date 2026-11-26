// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// thimsy-padd: merges the device's input nodes (device.json's pad section) into one virtual pad,
// "Thimsy Gamepad" (IMPLEMENTATION.md F.2; decisions D23, I12, I13). One epoll loop, one thread
// (plus the config store's watcher thread, which only wakes the loop).
//
//   thimsy-padd [--device FILE] [--wait-ms N] [--no-hide] [--verbose]
//   thimsy-padd --check [--device FILE]     read device.json's pad section and exit
//
// S02padd starts it in the background at boot; it runs until SIGTERM or SIGINT.

#include <errno.h>
#include <linux/input.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/Merger.h"
#include "core/PadSpec.h"
#include "core/Rumble.h"
#include "io/Control.h"
#include "io/EvdevSource.h"
#include "io/UinputSink.h"
#include "thimsy/watch.h"

namespace padd {
namespace {

struct Options {
    std::string device;
    std::string sysDir = "/sys";
    std::string devInputDir = "/dev/input";
    std::string uinput = "/dev/uinput";
    std::string privateDir = "/run/thimsy/padd";
    std::string tapPath = kDefaultTapPath;
    int waitMs = 2000;
    bool hide = true;
    bool verbose = false;
    bool check = false;
};

void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "thimsy-padd: %s\n", buf);
}

int64_t nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void armTimer(int fd, int64_t atNs) {
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    if (atNs > 0) {
        its.it_value.tv_sec = atNs / 1000000000;
        its.it_value.tv_nsec = atNs % 1000000000;
    }
    timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, nullptr);
}

// epoll tags: the kind in the high half, an index in the low half.
enum Kind : uint32_t { kSignal = 1, kWatch, kControl, kTapListen, kPad, kWaitTimer, kRumbleTimer, kSource };
uint64_t tag(Kind k, uint32_t i = 0) { return (static_cast<uint64_t>(k) << 32) | i; }

class Daemon : public UinputSink::FfHandler {
 public:
    Daemon(const Options& o, PadSpec spec) : opt_(o), spec_(std::move(spec)), open_(spec_.sources.size()) {}
    int run();

    // FfHandler: a game's force feedback on the pad.
    int upload(const RumbleEffect& e) override {
        std::vector<VibCommand> c;
        int rc = rumble_.upload(e, nowNs(), &c);
        vibrate(c);
        return rc;
    }
    int erase(int id) override {
        std::vector<VibCommand> c;
        int rc = rumble_.erase(id, nowNs(), &c);
        vibrate(c);
        return rc;
    }
    void play(int id, int count) override {
        std::vector<VibCommand> c;
        rumble_.play(id, count, nowNs(), &c);
        vibrate(c);
    }
    void gain(uint16_t g) override {
        std::vector<VibCommand> c;
        rumble_.setGain(g, nowNs(), &c);
        vibrate(c);
    }

 private:
    bool add(int fd, uint64_t t) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        ev.data.u64 = t;
        return epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }
    void scan();
    void present(bool timedOut);
    void syncFrom(int i);
    void closeSource(int i, const char* why);
    void emit(const std::vector<OutEvent>& evs);
    void vibrate(const std::vector<VibCommand>& cmds);
    void rearmRumble() { armTimer(rumbleTimer_, rumble_.nextDeadlineNs()); }

    Options opt_;
    PadSpec spec_;
    std::vector<std::unique_ptr<EvdevSource>> open_;
    std::map<std::string, int> openEvents_;  // eventN -> source
    std::set<std::string> rejected_;         // nodes already reported as not ours
    std::unique_ptr<Merger> merger_;
    std::unique_ptr<UinputSink> pad_, keys_;
    Rumble rumble_;
    std::unique_ptr<Control> control_;
    std::unique_ptr<TapServer> tap_;
    std::unique_ptr<NodeWatch> watch_;
    int ep_ = -1, sig_ = -1, waitTimer_ = -1, rumbleTimer_ = -1;
};

void Daemon::scan() {
    std::vector<bool> taken(spec_.sources.size());
    for (size_t i = 0; i < open_.size(); ++i) taken[i] = open_[i] != nullptr;
    EvdevSource::Paths paths;
    paths.devInputDir = opt_.devInputDir;
    paths.privateDir = opt_.privateDir;
    for (const NodeEntry& n : listNodes(opt_.sysDir)) {
        if (openEvents_.count(n.event)) continue;
        // Cheap first filter on the sysfs name, so other devices (our own pad, an external pad,
        // the power key) are never opened.
        bool candidate = false;
        for (size_t i = 0; i < spec_.sources.size(); ++i) {
            if (!taken[i] && spec_.sources[i].match.name == n.name) candidate = true;
        }
        if (!candidate) continue;
        std::string err;
        std::unique_ptr<EvdevSource> s = EvdevSource::open(n, paths, &err);
        if (!s) {
            if (rejected_.insert(n.event + err).second) say("%s: %s", n.event.c_str(), err.c_str());
            continue;
        }
        std::string why;
        int i = pickSource(spec_, s->info(), s->caps(), taken, &why);
        if (i < 0) {
            if (rejected_.insert(n.event + why).second) {
                say("%s (%s): not used: %s", n.event.c_str(), n.name.c_str(), why.c_str());
            }
            continue;
        }
        const Source& src = spec_.sources[i];
        if (src.grab && src.role != Role::Rumble && !s->grab()) {
            say("%s: EVIOCGRAB failed: %s", n.event.c_str(), strerror(errno));
        }
        if (src.hide && opt_.hide && !s->hide()) {
            say("%s: can't hide: %s", s->path().c_str(), strerror(errno));
        }
        if (!add(s->fd(), tag(kSource, static_cast<uint32_t>(i)))) {
            say("%s: epoll: %s", n.event.c_str(), strerror(errno));
            continue;
        }
        if (opt_.verbose) say("%s -> %s", n.event.c_str(), src.id.c_str());
        taken[i] = true;
        openEvents_[n.event] = i;
        open_[i] = std::move(s);
        if (merger_) {
            merger_->attachSource(i, open_[i]->caps());
            syncFrom(i);
        }
    }
    if (!merger_) {
        bool all = true;
        for (size_t i = 0; i < spec_.sources.size(); ++i) {
            if (spec_.sources[i].required && !open_[i]) all = false;
        }
        if (all) present(false);
    }
}

void Daemon::present(bool timedOut) {
    if (merger_) return;
    std::vector<std::optional<SourceCaps>> caps(spec_.sources.size());
    for (size_t i = 0; i < open_.size(); ++i) {
        if (open_[i]) caps[i] = open_[i]->caps();
        else if (timedOut && spec_.sources[i].required) say("%s: not found; the pad starts without it",
                                                              spec_.sources[i].id.c_str());
    }
    merger_.reset(new Merger(spec_, caps));
    std::string err;
    pad_ = UinputSink::create(opt_.uinput, padSetup(merger_->layout(), rumble_.maxEffects()), &err);
    if (!pad_) {
        say("can't create the pad: %s", err.c_str());
        exit(1);
    }
    add(pad_->fd(), tag(kPad));
    if (!merger_->layout().keysKeys.empty()) {
        keys_ = UinputSink::create(opt_.uinput, keysSetup(merger_->layout()), &err);
        if (!keys_) say("can't create the keyboard: %s", err.c_str());
    }
    armTimer(waitTimer_, 0);
    std::vector<OutEvent> evs;
    if (control_ && control_->intercept()) merger_->setIntercept(true, &evs);
    emit(evs);
    for (size_t i = 0; i < open_.size(); ++i) {
        if (open_[i]) syncFrom(static_cast<int>(i));
    }
    if (control_) control_->setReady(true);
    say("%s ready", kPadName);
}

void Daemon::syncFrom(int i) {
    std::set<uint16_t> keys;
    std::map<uint16_t, int32_t> abs;
    if (!open_[i]->currentState(&keys, &abs)) return;
    std::vector<OutEvent> evs;
    merger_->syncSource(i, keys, abs, &evs);
    emit(evs);
}

void Daemon::closeSource(int i, const char* why) {
    say("%s: %s", spec_.sources[i].id.c_str(), why);
    epoll_ctl(ep_, EPOLL_CTL_DEL, open_[i]->fd(), nullptr);
    for (auto it = openEvents_.begin(); it != openEvents_.end();) {
        it = it->second == i ? openEvents_.erase(it) : std::next(it);
    }
    open_[i].reset();
    rejected_.clear();
    if (merger_) {
        std::vector<OutEvent> evs;
        merger_->dropSource(i, &evs);
        emit(evs);
    }
}

void Daemon::emit(const std::vector<OutEvent>& evs) {
    if (evs.empty()) return;
    if (pad_ && !pad_->write(evs, Dev::Pad)) say("pad write: %s", strerror(errno));
    if (keys_ && !keys_->write(evs, Dev::Keys)) say("keyboard write: %s", strerror(errno));
    if (tap_) tap_->send(evs);
}

void Daemon::vibrate(const std::vector<VibCommand>& cmds) {
    int s = spec_.rumbleSource;
    for (const VibCommand& c : cmds) {
        if (s >= 0 && open_[s] && !open_[s]->rumble(c) && opt_.verbose) {
            say("%s: rumble: %s", spec_.sources[s].id.c_str(), strerror(errno));
        }
    }
    rearmRumble();
}

int Daemon::run() {
    ep_ = epoll_create1(EPOLL_CLOEXEC);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    sig_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    waitTimer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    rumbleTimer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (ep_ < 0 || sig_ < 0 || waitTimer_ < 0 || rumbleTimer_ < 0) {
        say("setup: %s", strerror(errno));
        return 1;
    }
    add(sig_, tag(kSignal));
    add(waitTimer_, tag(kWaitTimer));
    add(rumbleTimer_, tag(kRumbleTimer));

    std::string err;
    control_ = Control::create(thimsy::ConfigStore::defaultRoots(), nullptr, &err);
    if (control_) add(control_->fd(), tag(kControl));
    else say("no config store (%s): intercept is unavailable", err.c_str());
    tap_ = TapServer::create(opt_.tapPath, &err);
    if (tap_) add(tap_->listenFd(), tag(kTapListen));
    else say("no intercept tap: %s", err.c_str());
    watch_ = NodeWatch::create(opt_.devInputDir);
    if (watch_) add(watch_->fd(), tag(kWatch));
    else say("%s: can't watch: hotplug is off", opt_.devInputDir.c_str());

    armTimer(waitTimer_, nowNs() + static_cast<int64_t>(opt_.waitMs) * 1000000);
    scan();

    bool running = true;
    while (running) {
        struct epoll_event evs[16];
        int n = epoll_wait(ep_, evs, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            say("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int k = 0; k < n; ++k) {
            Kind kind = static_cast<Kind>(evs[k].data.u64 >> 32);
            uint32_t idx = static_cast<uint32_t>(evs[k].data.u64);
            uint64_t ticks;
            switch (kind) {
                case kSignal: {
                    struct signalfd_siginfo si;
                    while (read(sig_, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
                        say("signal %u: stopping", si.ssi_signo);
                        running = false;
                    }
                    break;
                }
                case kWaitTimer:
                    while (read(waitTimer_, &ticks, sizeof ticks) > 0) {
                    }
                    scan();
                    present(true);
                    break;
                case kRumbleTimer: {
                    while (read(rumbleTimer_, &ticks, sizeof ticks) > 0) {
                    }
                    std::vector<VibCommand> c;
                    rumble_.tick(nowNs(), &c);
                    vibrate(c);
                    break;
                }
                case kWatch:
                    if (watch_->drain()) scan();
                    break;
                case kControl: {
                    control_->drain();
                    if (merger_) {
                        std::vector<OutEvent> out;
                        bool on = control_->intercept();
                        if (on != merger_->intercept()) say("intercept %s", on ? "on" : "off");
                        merger_->setIntercept(on, &out);
                        emit(out);
                    }
                    break;
                }
                case kTapListen:
                    tap_->acceptClients();
                    break;
                case kPad:
                    if (!pad_->handleRequests(this)) say("uinput read: %s", strerror(errno));
                    break;
                case kSource: {
                    int i = static_cast<int>(idx);
                    if (i < 0 || i >= static_cast<int>(open_.size()) || !open_[i]) break;
                    std::vector<SourceEvent> in;
                    EvdevSource::Read r = open_[i]->readEvents(i, &in);
                    if (merger_) {
                        std::vector<OutEvent> out;
                        for (const SourceEvent& e : in) {
                            merger_->process(e, &out);
                            if (merger_->needsSync(i)) {
                                emit(out);
                                out.clear();
                                syncFrom(i);
                            }
                        }
                        emit(out);
                    }
                    if (r == EvdevSource::Read::Gone) closeSource(i, "gone");
                    else if (r == EvdevSource::Read::Error) closeSource(i, strerror(errno));
                    break;
                }
            }
        }
    }

    std::vector<VibCommand> c;
    rumble_.stopAll(nowNs(), &c);
    vibrate(c);
    if (control_) control_->setReady(false);
    return 0;
}

void usage() {
    fprintf(stderr,
            "usage: thimsy-padd [--device FILE] [--wait-ms N] [--no-hide] [--verbose]\n"
            "       thimsy-padd --check [--device FILE]\n"
            "  --device FILE   device.json (default $THIMSY_DEVICE_JSON or /usr/share/thimsy/device.json)\n"
            "  --wait-ms N     how long to wait for required sources before starting without them\n"
            "  --sys DIR, --dev-input DIR, --uinput PATH, --tap PATH   for tests\n");
}

}  // namespace
}  // namespace padd

int main(int argc, char** argv) {
    using namespace padd;
    Options o;
    const char* env = getenv("THIMSY_DEVICE_JSON");
    o.device = env && *env ? env : "/usr/share/thimsy/device.json";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--device") o.device = value();
        else if (a == "--wait-ms") o.waitMs = atoi(value().c_str());
        else if (a == "--sys") o.sysDir = value();
        else if (a == "--dev-input") o.devInputDir = value();
        else if (a == "--uinput") o.uinput = value();
        else if (a == "--tap") o.tapPath = value();
        else if (a == "--no-hide") o.hide = false;
        else if (a == "--verbose" || a == "-v") o.verbose = true;
        else if (a == "--check") o.check = true;
        else {
            usage();
            return 2;
        }
    }
    PadSpec spec;
    std::string err;
    std::vector<std::string> warnings;
    if (!loadPadSpec(o.device, &spec, &err, &warnings)) {
        say("%s", err.c_str());
        return 1;
    }
    for (const std::string& w : warnings) say("warning: %s", w.c_str());
    if (o.check) {
        printf("%s: %zu sources, %zu buttons, %zu axes%s\n", o.device.c_str(), spec.sources.size(),
               spec.buttons.size(), spec.axes.size(), spec.passThrough ? ", pass-through" : "");
        return 0;
    }
    Daemon d(o, std::move(spec));
    return d.run();
}
