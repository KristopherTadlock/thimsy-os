// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "Session.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/GpuEnv.h"
#include "core/KeyMachine.h"
#include "core/NanoJail.h"
#include "core/Requests.h"
#include "core/SleepPlan.h"
#include "core/Supervisor.h"
#include "core/VolumePolicy.h"
#include "io/Blkid.h"
#include "io/Evdev.h"
#include "io/Mounter.h"
#include "io/Netlink.h"
#include "io/Proc.h"
#include "io/Sysfs.h"
#include "thimsy/atomic_file.h"
#include "thimsy/config.h"
#include "thimsy/json.h"

namespace thimsy {
namespace session {
namespace {

// Paths on the device; every one is read through the root prefix.
constexpr const char* kRunDir = "/run/thimsy";
constexpr const char* kPropsDir = "/run/thimsy/props";
constexpr const char* kPersistFile = "/storage/.config/thimsy/persist.props";
constexpr const char* kStateDir = "/run/thimsy/session";
constexpr const char* kLogDir = "/run/thimsy/log";
constexpr const char* kVolumesJson = "/run/thimsy/volumes.json";
constexpr const char* kMediaRwDir = "/run/thimsy/media_rw";
constexpr const char* kDeviceJson = "/usr/share/thimsy/device.json";
constexpr const char* kGpuJson = "/usr/share/thimsy/gpu.json";
constexpr const char* kDeferredMarker = "/run/thimsy/session/deferred-started";

constexpr const char* kKeyFirstFrame = "sys.thimsy.nano.first_frame";
constexpr const char* kKeyRequest = "sys.thimsy.session.request";
constexpr const char* kKeyDone = "sys.thimsy.session.done";
constexpr const char* kKeyPowerctl = "sys.powerctl";
constexpr const char* kKeyScrapeRun = "sys.gammaos.nano.scrape_run";
constexpr const char* kKeyGamePid = "sys.thimsy.game.pid";
constexpr const char* kKeyVolumesSerial = "sys.thimsy.volumes.serial";
constexpr const char* kKeyHudVolume = "sys.thimsy.hud.volume";
constexpr const char* kKeyPowerLong = "sys.thimsy.key.power_long";
constexpr const char* kKeyAudioVolume = "persist.thimsy.audio.volume";

enum Tag : uint64_t { kTagSignal = 1, kTagInotify, kTagUevent, kTagEvdev, kTagPaddPidfd, kTagGamePidfd };
uint64_t tag(Tag t, int fd) { return (static_cast<uint64_t>(t) << 32) | static_cast<uint32_t>(fd); }

Ms monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<Ms>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string baseName(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

class Session {
 public:
    explicit Session(const Options& o)
        : opts_(o), sysfs_(o.root), evdev_(sysfs_, o.fakeHw) {
        PolicyConfig pc;
        pc.mediaRwDir = "/mnt/media_rw";
        vols_ = VolumeManager(pc);
    }
    ~Session();

    bool init();
    int loop();

 private:
    enum class Role { Nano, Padd, Scrape, Fsck, Deferred, Tool };
    struct Child {
        Role role;
        int job;
    };

    std::string P(const std::string& abs) const { return opts_.root + abs; }
    std::string tool(const std::string& nameOrPath) const;
    std::vector<std::string> childEnv() const;
    void log(const std::string& msg);
    void kmsg(const std::string& msg);

    void loadDeviceFacts();
    void loadGpuEnv();
    void loadBootDisk();
    std::vector<std::string> jailPlan() const;
    void killOrphanNano();
    void addFd(int fd, Tag t);
    void delFd(int fd);
    void registerEvdev();
    void adoptMounts();
    void coldplug();

    void runSup(const std::vector<SupAction>& acts);
    void runVol(const std::vector<VolAction>& acts);
    void publishVolumes();
    void onKeyActions(const std::vector<KeyAction>& acts);
    void handleUevent(const Uevent& ev);
    void handleBlock(const std::string& name, const Uevent* ev);
    void onPropChanged(const std::string& key);
    void handleRequest(const Request& r, bool answer);
    void reap();
    void onSignal();
    void tick();

    int gamePid() const;
    int gamePgid() const;
    void doSleep();
    void execStep(const Step& s);
    void stepVolume(int dir);
    std::string setBrightness(int percent);
    void power(Verb v);
    pid_t spawnChild(SpawnSpec spec, Role role, int job = 0);

    Options opts_;
    Sysfs sysfs_;
    std::unique_ptr<ConfigStore> store_;
    KeyMachine keys_;
    Supervisor sup_;
    VolumeManager vols_;
    RequestFilter reqFilter_;
    Evdev evdev_;
    std::unique_ptr<UeventSource> uevents_;
    std::unique_ptr<Mounter> mounter_;

    int epfd_ = -1;
    int sigfd_ = -1;
    int inofd_ = -1;
    std::set<int> evdevFds_;
    int paddPidfd_ = -1;
    int gamePidfd_ = -1;
    int gamePollPid_ = 0;
    Ms gamePollAt_ = kNever;
    int logFd_ = -1;

    std::map<pid_t, Child> children_;
    std::map<int, pid_t> fsckPids_;
    BootDisk boot_;
    std::string family_;
    std::string memSleepPref_;
    std::vector<std::string> gpuEnv_;   // gpu.json's "env", for Nano (and so its games)
    std::string audioCard_ = "0";
    KeyConfig keyConfig_;
    int volumeLevel_ = 60;
    Ms persistVolumeAt_ = kNever;
    uint64_t volumesSerial_ = 0;
    uint64_t powerLongCount_ = 0;
    std::string savedBrightness_;
    bool quit_ = false;
    bool sleeping_ = false;
};

Session::~Session() {
    for (int fd : {epfd_, sigfd_, inofd_, paddPidfd_, gamePidfd_, logFd_}) {
        if (fd >= 0) close(fd);
    }
}

std::string Session::tool(const std::string& nameOrPath) const {
    if (!opts_.toolDir.empty()) {
        std::string t = opts_.toolDir + "/" + baseName(nameOrPath);
        if (access(t.c_str(), X_OK) == 0) return t;
    }
    if (!nameOrPath.empty() && nameOrPath[0] == '/') return P(nameOrPath);
    return nameOrPath;
}

std::vector<std::string> Session::childEnv() const {
    std::vector<std::string> env;
    if (!opts_.root.empty()) env.push_back("THIMSY_ROOT=" + opts_.root);
    if (!opts_.toolDir.empty()) env.push_back("THIMSY_TOOLDIR=" + opts_.toolDir);
    return env;
}

void Session::log(const std::string& msg) {
    Ms t = monoNow();
    char head[32];
    snprintf(head, sizeof head, "%lld.%03lld ", static_cast<long long>(t / 1000), static_cast<long long>(t % 1000));
    std::string line = head + msg + "\n";
    if (logFd_ >= 0) {
        ssize_t n = write(logFd_, line.data(), line.size());
        (void)n;
    }
    if (opts_.verbose) {
        ssize_t n = write(2, line.data(), line.size());
        (void)n;
    }
}

void Session::kmsg(const std::string& msg) {
    log(msg);
    int fd = open(P("/dev/kmsg").c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) return;
    std::string line = "thimsy-session: " + msg + "\n";
    ssize_t n = write(fd, line.data(), line.size());
    (void)n;
    close(fd);
}

pid_t Session::spawnChild(SpawnSpec spec, Role role, int job) {
    std::vector<std::string> env = childEnv();
    spec.env.insert(spec.env.end(), env.begin(), env.end());
    std::string err;
    pid_t pid = spawn(spec, &err);
    if (pid > 0) {
        children_[pid] = Child{role, job};
    } else {
        log("cannot start " + spec.argv[0] + ": " + err);
    }
    return pid;
}

void Session::loadDeviceFacts() {
    std::string text;
    json::Value dj;
    if (readFile(P(kDeviceJson), &text)) {
        std::string err;
        if (!json::parse(text, &dj, &err)) log("device.json: " + err);
    }
    memSleepPref_ = dj["power"]["mem_sleep"].asString("");
    for (const json::Value& b : dj["pad"]["hotkey"]["quit_combo"].elements()) {
        InputCode c;
        if (b.isString() && inputCodeByName(b.asString(), &c)) keyConfig_.quitCombo.push_back(c);
    }
    if (dj["audio"]["card_index"].isInt()) {
        audioCard_ = std::to_string(dj["audio"]["card_index"].asInt());
    }
    // The family: S01props' file, else ro.thimsy.family, else the only family on the root.
    std::string f;
    if (sysfs_.readLine("/run/thimsy/family", &f) && !f.empty()) {
        family_ = f;
    } else if (!(f = store_->get("ro.thimsy.family")).empty()) {
        family_ = f;
    } else {
        std::vector<std::string> fams = sysfs_.list("/usr/share/thimsy/family");
        if (fams.size() == 1) family_ = fams.front();
    }
    int64_t v = 0;
    if (parseInt64(store_->get(kKeyAudioVolume), &v) && v >= 0 && v <= 100) volumeLevel_ = static_cast<int>(v);
}

// The GPU pack's environment (core/GpuEnv.h): read once, as the root is read-only.
void Session::loadGpuEnv() {
    std::string text;
    if (!readFile(P(kGpuJson), &text)) {
        log("no gpu.json: Nano gets no GPU environment");
        return;
    }
    std::string err;
    std::vector<std::string> skipped;
    if (!gpuEnvFromJson(text, &gpuEnv_, &err, &skipped)) {
        log(err + ": Nano gets no GPU environment");
        gpuEnv_.clear();
        return;
    }
    for (const std::string& k : skipped) log("gpu.json: env \"" + k + "\" left out (bad name or value)");
    std::string all;
    for (const std::string& kv : gpuEnv_) all += " " + kv;
    log("GPU environment for Nano:" + (all.empty() ? std::string(" none") : all));
}

void Session::loadBootDisk() {
    std::map<std::string, std::string> kv = sysfs_.readUevent("/run/thimsy/bootdisk");
    boot_.disk = kv["BOOT_DISK"];
    boot_.emmc = kv["BOOT_EMMC"] == "1";
    if (boot_.disk.empty()) {
        log("no boot disk in /run/thimsy/bootdisk: drives are told apart by partition type only");
    } else {
        log("boot disk " + boot_.disk + (boot_.emmc ? " (eMMC)" : ""));
    }
}

std::vector<std::string> Session::jailPlan() const {
    std::vector<std::string> plan = planJail("/dev", sysfs_.list("/dev"), sysfs_.emmcDisks());
    return plan;
}

void Session::killOrphanNano() {
    pid_t pid = readPidFile(P("/run/thimsy/nano.pid"));
    if (pid <= 1 || !alive(pid)) return;
    if (cmdline(pid).find(baseName(opts_.nanoPath)) == std::string::npos) return;
    log("stopping Nano " + std::to_string(pid) + " left by an earlier session");
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    for (int i = 0; i < 40 && alive(pid); ++i) usleep(50000);
    if (alive(pid)) kill(-pid, SIGKILL);
}

void Session::addFd(int fd, Tag t) {
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = EPOLLIN;
    e.data.u64 = tag(t, fd);
    epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e);
}

void Session::delFd(int fd) { epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr); }

void Session::registerEvdev() {
    evdev_.scan();
    std::vector<int> now = evdev_.fds();
    std::set<int> nowSet(now.begin(), now.end());
    for (int fd : evdevFds_) {
        if (!nowSet.count(fd)) delFd(fd);
    }
    for (int fd : now) {
        if (!evdevFds_.count(fd)) {
            addFd(fd, kTagEvdev);
            log("reading input " + evdev_.nameOf(fd));
        }
    }
    evdevFds_ = nowSet;
}

void Session::adoptMounts() {
    std::string text;
    if (!readFile(P("/proc/mounts"), &text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        size_t a = line.find(' ');
        if (a == std::string::npos) continue;
        size_t b = line.find(' ', a + 1);
        std::string dev = line.substr(0, a);
        std::string mnt = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        if (mnt.compare(0, 9, "/storage/") != 0 || dev.compare(0, 5, "/dev/") != 0) continue;
        std::string id = mnt.substr(9);
        if (id.empty() || id.find('/') != std::string::npos) continue;
        vols_.adoptMounted(baseName(dev), id);
        log("found " + dev + " mounted at " + mnt);
    }
}

void Session::coldplug() {
    for (const std::string& name : sysfs_.blockNames()) handleBlock(name, nullptr);
}

bool Session::init() {
    sigset_t mask;
    sigemptyset(&mask);
    for (int s : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&mask, s);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    sigfd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    for (const char* d : {kRunDir, kPropsDir, kStateDir, kLogDir, kMediaRwDir}) makeDirs(P(d));
    logFd_ = open(P(std::string(kLogDir) + "/session.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    log("thimsy-session starting" + std::string(opts_.fakeHw ? " (fake hardware)" : "") +
        (opts_.root.empty() ? "" : ", root " + opts_.root));

    ConfigStore::Roots roots;
    roots.persistFile = P(kPersistFile);
    roots.runDir = P(kPropsDir);
    std::string err;
    store_ = ConfigStore::open(roots, &err);
    if (!store_) {
        log("cannot open the config store: " + err);
        return false;
    }
    loadDeviceFacts();
    loadGpuEnv();
    keys_ = KeyMachine(keyConfig_);
    loadBootDisk();
    if (!family_.empty()) log("family " + family_);

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0 || sigfd_ < 0) {
        log("epoll or signalfd failed");
        return false;
    }
    addFd(sigfd_, kTagSignal);
    inofd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inofd_ < 0 || inotify_add_watch(inofd_, P(kPropsDir).c_str(), IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE) < 0) {
        log("cannot watch the config store: " + std::string(strerror(errno)));
        return false;
    }
    addFd(inofd_, kTagInotify);

    if (!opts_.fakeHw) {
        // Mounts made later must reach Nano's namespace, a slave of this one.
        if (mount(nullptr, "/", nullptr, MS_REC | MS_SHARED, nullptr) != 0) {
            log("making / shared failed: " + std::string(strerror(errno)));
        }
        mounter_ = makeSyscallMounter();
    } else {
        mounter_ = makeCommandMounter(tool("mount"), tool("umount"), P(std::string(kLogDir) + "/mount.log"), childEnv());
    }

    // Nano first: everything else can wait for it.
    killOrphanNano();
    struct stat st;
    bool deferredRan = stat(P(kDeferredMarker).c_str(), &st) == 0;
    pid_t padd = readPidFile(P("/run/thimsy/padd.pid"));
    bool paddAlive = padd > 1 && alive(padd);
    runSup(sup_.start(monoNow(), deferredRan, paddAlive));
    if (paddAlive) {
        paddPidfd_ = pidfdOpen(padd);
        if (paddPidfd_ >= 0) addFd(paddPidfd_, kTagPaddPidfd);
        log("thimsy-padd runs as " + std::to_string(padd));
    }

    std::string uerr;
    if (opts_.fakeHw) {
        makeDirs(P("/run/fake-hw"));
        uevents_ = openUeventFifo(P("/run/fake-hw/uevent"), &uerr);
    } else {
        uevents_ = openNetlink(&uerr);
    }
    if (uevents_) {
        addFd(uevents_->fd(), kTagUevent);
    } else {
        log("no uevents: " + uerr);
    }
    registerEvdev();

    // The volumes file exists from the start, so Nano can read it at once.
    int64_t serial = 0;
    if (parseInt64(store_->get(kKeyVolumesSerial), &serial) && serial > 0) volumesSerial_ = static_cast<uint64_t>(serial);
    adoptMounts();
    publishVolumes();

    if (store_->get(kKeyFirstFrame) == "1") runSup(sup_.firstFrame(monoNow()));
    if (deferredRan) {
        // A respawned session: D10cards' coldplug has happened already, so look for drives here.
        log("deferred services ran before this session; scanning drives");
        coldplug();
    }
    if (opts_.fakeHw) {
        std::string replay;
        if (readFile(P("/run/fake-hw/uevent.replay"), &replay)) {
            std::vector<Uevent> evs;
            parseTextUevents(&replay, &evs, true);
            for (const Uevent& ev : evs) handleUevent(ev);
        }
    }
    kmsg("started");
    return true;
}

void Session::runSup(const std::vector<SupAction>& acts) {
    for (const SupAction& a : acts) {
        Ms now = monoNow();
        switch (a.kind) {
            case SupKind::SpawnNano: {
                SpawnSpec s;
                s.argv = {tool(opts_.nanoPath)};
                s.env = gpuEnv_;   // the games Nano starts inherit it
                s.logFile = P(std::string(kLogDir) + "/nano.log");
                std::vector<std::string> plan = jailPlan();
                if (opts_.fakeHw) {
                    std::string text;
                    for (const std::string& p : plan) text += p + "\n";
                    writeFileAtomic(P(std::string(kStateDir) + "/jail.plan"), text, {0644, false});
                } else if (!plan.empty()) {
                    s.preExec = [plan](std::string* err) { return applyJail(plan, err); };
                }
                pid_t pid = spawnChild(s, Role::Nano);
                if (pid > 0) {
                    writeFileAtomic(P("/run/thimsy/nano.pid"), std::to_string(pid) + "\n", {0644, false});
                    kmsg("nano started (" + std::to_string(pid) + ", " + std::to_string(plan.size()) +
                         " eMMC nodes masked)");
                }
                runSup(sup_.nanoStarted(now, pid));
                break;
            }
            case SupKind::SpawnPadd: {
                SpawnSpec s;
                s.argv = {tool(opts_.paddPath)};
                s.logFile = P(std::string(kLogDir) + "/padd.log");
                pid_t pid = spawnChild(s, Role::Padd);
                if (pid > 0) {
                    writeFileAtomic(P("/run/thimsy/padd.pid"), std::to_string(pid) + "\n", {0644, false});
                    log("thimsy-padd started (" + std::to_string(pid) + ")");
                }
                runSup(sup_.paddStarted(now, pid));
                break;
            }
            case SupKind::SpawnScrape: {
                SpawnSpec s;
                s.argv = {tool(opts_.scrapePath)};
                s.logFile = P(std::string(kLogDir) + "/scrape.log");
                s.nice = 10;
                s.oomScoreAdj = 800;
                s.ioprioIdle = true;
                pid_t pid = spawnChild(s, Role::Scrape);
                if (pid > 0) log("scraper started (" + std::to_string(pid) + ")");
                sup_.scrapeStarted(pid);
                break;
            }
            case SupKind::RunDeferred: {
                writeFileAtomic(P(kDeferredMarker), "1\n", {0644, false});
                SpawnSpec s;
                s.argv = {P("/etc/init.d/rcD"), "start"};
                s.logFile = P(std::string(kLogDir) + "/deferred.log");
                spawnChild(s, Role::Deferred);
                kmsg("deferred services starting");
                break;
            }
            case SupKind::SignalGroup:
                if (a.pid > 1) {
                    log("signal " + std::to_string(a.sig) + " to group " + std::to_string(a.pid));
                    kill(-a.pid, a.sig);
                }
                break;
            case SupKind::WatchGame: {
                if (gamePidfd_ >= 0) {
                    delFd(gamePidfd_);
                    close(gamePidfd_);
                    gamePidfd_ = -1;
                }
                gamePollPid_ = 0;
                gamePollAt_ = kNever;
                log("Nano left game " + std::to_string(a.pid) + " running; it starts again when the game ends");
                gamePidfd_ = pidfdOpen(a.pid);
                if (gamePidfd_ >= 0) {
                    addFd(gamePidfd_, kTagGamePidfd);
                } else if (alive(a.pid)) {
                    gamePollPid_ = a.pid;
                    gamePollAt_ = now + 1000;
                } else {
                    runSup(sup_.gameExited(now));
                }
                break;
            }
        }
    }
}

void Session::publishVolumes() {
    json::WriteOptions wo;
    wo.indent = 2;
    std::string text = json::write(vols_.toJson(), wo) + "\n";
    std::string err;
    if (!writeFileAtomic(P(kVolumesJson), text, {0644, false}, &err)) {
        log("cannot write volumes.json: " + err);
        return;
    }
    ++volumesSerial_;
    store_->set(kKeyVolumesSerial, std::to_string(volumesSerial_));
}

void Session::runVol(const std::vector<VolAction>& acts) {
    for (const VolAction& a : acts) {
        switch (a.kind) {
            case VolActKind::Fsck: {
                std::string node = P("/dev/" + a.dev);
                SpawnSpec s;
                if (a.fs == "exfat") {
                    s.argv = {tool("fsck.exfat"), "-p", node};
                } else if (a.fs == "vfat") {
                    s.argv = {tool("fsck.fat"), "-a", node};
                } else {
                    s.argv = {tool("e2fsck"), "-p", node};
                }
                s.logFile = P(std::string(kLogDir) + "/fsck.log");
                pid_t pid = spawnChild(s, Role::Fsck, a.job);
                log("checking " + a.dev + " (" + a.fs + ", " + a.id + ")");
                if (pid > 0) {
                    fsckPids_[a.job] = pid;
                } else {
                    runVol(vols_.onFsckDone(a.job, -1));
                }
                break;
            }
            case VolActKind::KillFsck: {
                auto it = fsckPids_.find(a.job);
                if (it != fsckPids_.end()) {
                    log("stopping the check of " + a.dev);
                    kill(-it->second, SIGTERM);
                    fsckPids_.erase(it);
                }
                break;
            }
            case VolActKind::Mount: {
                std::string mp = P(a.mountPoint);
                std::string err;
                bool ok = makeDirs(mp);
                if (ok && !opts_.fakeHw && !setImmutable(mp, &err)) log("immutable flag on " + a.mountPoint + ": " + err);
                err.clear();
                ok = ok && mounter_->mount(P("/dev/" + a.dev), mp, a.fs, a.options, &err);
                if (ok) {
                    std::string link = P(std::string(kMediaRwDir) + "/" + a.id);
                    unlink(link.c_str());
                    if (symlink(a.mountPoint.c_str(), link.c_str()) != 0) log("link " + link + ": " + strerror(errno));
                    log("mounted " + a.dev + " at " + a.mountPoint + " (" + a.options + ")");
                } else {
                    if (err.empty()) err = "cannot make " + a.mountPoint;
                    log("mounting " + a.dev + " failed: " + err);
                }
                runVol(vols_.onMountDone(a.dev, ok, err));
                break;
            }
            case VolActKind::Unmount: {
                std::string err;
                bool ok = mounter_->unmount(P(a.mountPoint), a.lazy, &err);
                if (ok || a.lazy) unlink(P(std::string(kMediaRwDir) + "/" + a.id).c_str());
                log(std::string(ok ? "unmounted " : "unmounting failed: ") + a.mountPoint + (a.lazy ? " (detached)" : "") +
                    (ok ? "" : ": " + err));
                runVol(vols_.onUnmountDone(a.dev, ok, err));
                break;
            }
        }
    }
    if (vols_.takeChanged()) publishVolumes();
}

void Session::handleBlock(const std::string& name, const Uevent* ev) {
    BlockDev d;
    if (!sysfs_.blockDev(name, &d)) {
        log("block device " + name + " has no sysfs entry");
        return;
    }
    if (ev) {
        int64_t v = 0;
        if (d.partn == 0 && parseInt64(ev->get("PARTN"), &v)) d.partn = static_cast<int>(v);
        if (d.partName.empty()) d.partName = ev->get("PARTNAME");
    }
    if (d.partition) {
        PartTable t;
        std::string err;
        if (readPartTableFile(P("/dev/" + d.disk), sysfs_.logicalBlockSize(d.disk), &t, &err)) {
            d.table = t.scheme;
            if (const PartEntry* e = t.entry(d.partn)) {
                d.partType = e->type;
                if (d.partName.empty()) d.partName = e->name;
            }
        }
    }
    Classification c = classify(d, boot_, vols_.config());
    if (c.disposition == Disposition::Drive) {
        probeFsFile(P("/dev/" + name), &d.fs);
        log("drive " + name + ": " + (d.fs.type.empty() ? std::string("no known filesystem") : d.fs.type + " " + d.fs.serial) +
            (d.fs.label.empty() ? "" : " \"" + d.fs.label + "\""));
    } else if (d.partition || !d.diskHasPartitions) {
        log("not a drive: " + name + ", " + c.reason);
    }
    runVol(vols_.onDevice(d, boot_));
}

void Session::handleUevent(const Uevent& ev) {
    if (ev.subsystem == "block") {
        std::string name = ev.devname.empty() ? baseName(ev.devpath) : baseName(ev.devname);
        if (ev.action == "remove") {
            log("removed " + name);
            runVol(vols_.onRemoved(name));
        } else if (ev.action == "add" || ev.action == "change") {
            handleBlock(name, &ev);
            if (ev.action == "change" && ev.devtype == "disk") {
                // The partition table may have changed: look at the disk's partitions again.
                for (const std::string& n : sysfs_.blockNames()) {
                    if (n != name && sysfs_.diskOf(n) == name) handleBlock(n, nullptr);
                }
            }
        }
    } else if (ev.subsystem == "input") {
        registerEvdev();
    }
}

int Session::gamePid() const {
    int64_t v = 0;
    if (!parseInt64(store_->get(kKeyGamePid), &v) || v <= 1) return 0;
    pid_t pid = static_cast<pid_t>(v);
    return alive(pid) ? pid : 0;
}

int Session::gamePgid() const {
    pid_t pid = gamePid();
    if (pid <= 0) return 0;
    pid_t pg = getpgid(pid);
    if (pg <= 0) pg = pid;
    if (pg <= 1 || pg == getpgrp() || pg == sup_.nanoPid()) return 0;  // never Nano's or our own group
    return pg;
}

void Session::onKeyActions(const std::vector<KeyAction>& acts) {
    for (KeyAction a : acts) {
        log(std::string("key: ") + toString(a));
        switch (a) {
            case KeyAction::Sleep: doSleep(); break;
            case KeyAction::PowerOff: power(Verb::PowerOff); break;
            case KeyAction::PowerLongPress:
                store_->set(kKeyPowerLong, std::to_string(++powerLongCount_));
                break;
            case KeyAction::VolumeUp: stepVolume(+1); break;
            case KeyAction::VolumeDown: stepVolume(-1); break;
            case KeyAction::QuitGame: runSup(sup_.quitGame(monoNow(), gamePgid())); break;
        }
    }
}

void Session::stepVolume(int dir) {
    volumeLevel_ = std::max(0, std::min(100, volumeLevel_ + dir * 5));
    store_->set(kKeyHudVolume, std::to_string(volumeLevel_));
    SpawnSpec s;
    std::string control = store_->get("ro.thimsy.audio.volume_control", "Master");
    s.argv = {tool("amixer"), "-q", "-c", audioCard_, "sset", control, std::to_string(volumeLevel_) + "%"};
    s.logFile = P(std::string(kLogDir) + "/session-tools.log");
    s.newSession = false;
    spawnChild(s, Role::Tool);
    persistVolumeAt_ = monoNow() + 1500;  // persisted once the keys rest: the file is fsynced
}

std::string Session::setBrightness(int percent) {
    std::string bl = sysfs_.backlight();
    if (bl.empty()) return "error:no backlight";
    std::string maxText;
    int64_t max = 0;
    if (!sysfs_.readLine(bl + "/max_brightness", &maxText) || !parseInt64(maxText, &max) || max <= 0) {
        return "error:no max_brightness";
    }
    int64_t raw = percent == 0 ? 0 : std::max<int64_t>(1, (max * percent + 50) / 100);
    std::string err;
    if (!sysfs_.write(bl + "/brightness", std::to_string(raw), &err)) return "error:" + err;
    log("brightness " + std::to_string(percent) + "% (" + std::to_string(raw) + ")");
    return "ok";
}

void Session::power(Verb v) {
    const char* what = v == Verb::Reboot ? "reboot" : "poweroff";
    if (sup_.shuttingDown()) return;
    sup_.shutdown();
    kmsg(std::string(what) + " requested");
    SpawnSpec s;
    s.argv = {tool(what)};
    s.logFile = P(std::string(kLogDir) + "/session-tools.log");
    spawnChild(s, Role::Tool);
}

void Session::execStep(const Step& s) {
    switch (s.kind) {
        case StepKind::SetProp: store_->set(s.target, s.value); break;
        case StepKind::SignalGroup: kill(-s.pgid, s.signal); break;
        case StepKind::Script: {
            SpawnSpec sp;
            sp.argv = {P("/etc/init.d/deferred/" + s.target), s.value};
            sp.env = childEnv();
            sp.logFile = P(std::string(kLogDir) + "/deferred.log");
            runWait(sp, 10000);
            break;
        }
        case StepKind::Hook: {
            SpawnSpec sp;
            sp.argv = {"sh", P("/usr/share/thimsy/family/" + family_ + "/hook"), s.value};
            sp.env = childEnv();
            sp.logFile = P(std::string(kLogDir) + "/session-tools.log");
            runWait(sp, 5000);
            break;
        }
        case StepKind::BacklightOff: {
            std::string bl = sysfs_.backlight();
            savedBrightness_.clear();
            if (!bl.empty() && sysfs_.readLine(bl + "/brightness", &savedBrightness_)) sysfs_.write(bl + "/brightness", "0");
            break;
        }
        case StepKind::BacklightRestore: {
            std::string bl = sysfs_.backlight();
            if (!bl.empty() && !savedBrightness_.empty()) sysfs_.write(bl + "/brightness", savedBrightness_);
            break;
        }
        case StepKind::Sync: sync(); break;
        case StepKind::WriteSysfs:
        case StepKind::Suspend: {
            std::string err;
            if (!sysfs_.write(s.target, s.value, &err)) log("  " + s.target + ": " + err);
            break;
        }
    }
}

void Session::doSleep() {
    if (sleeping_) return;
    sleeping_ = true;
    SleepContext ctx;
    ctx.gamePgid = gamePgid();
    pid_t wpa = readPidFile(P("/run/wpa_supplicant.pid"));
    ctx.wifiRunning = wpa > 1 && (opts_.fakeHw || alive(wpa));
    ctx.hasFamilyHook = !family_.empty() && sysfs_.exists("/usr/share/thimsy/family/" + family_ + "/hook");
    ctx.hasBacklight = !sysfs_.backlight().empty();
    sysfs_.readLine("/sys/power/mem_sleep", &ctx.memSleepOffered);
    sysfs_.readLine("/sys/power/state", &ctx.stateOffered);
    ctx.preferredMemSleep = memSleepPref_;
    SleepPlan plan = planSleep(ctx);
    bool suspends = false;
    kmsg("sleep");
    for (const Step& s : plan.sleep) {
        log("sleep: " + describe(s));
        execStep(s);
        if (s.kind == StepKind::Suspend) suspends = true;
    }
    if (!suspends) log("sleep: the kernel offers no sleep state; waking at once");
    keys_.noteResumed(monoNow());
    for (const Step& s : plan.wake) {
        log("wake: " + describe(s));
        execStep(s);
    }
    kmsg("awake");
    sleeping_ = false;
}

void Session::handleRequest(const Request& r, bool answer) {
    std::string result = "ok";
    log(std::string("request: ") + toString(r.verb) + (r.arg.empty() ? "" : " " + r.arg) +
        (r.seq.empty() ? "" : " (#" + r.seq + ")"));
    switch (r.verb) {
        case Verb::Sleep:
            if (answer) store_->set(kKeyDone, doneValue(r, "ok"));  // answered before the device sleeps
            doSleep();
            return;
        case Verb::PowerOff:
        case Verb::Reboot:
            if (answer) store_->set(kKeyDone, doneValue(r, "ok"));
            power(r.verb);
            return;
        case Verb::QuitGame: {
            int pg = gamePgid();
            if (pg <= 0) {
                result = "error:no game";
            } else {
                runSup(sup_.quitGame(monoNow(), pg));
            }
            break;
        }
        case Verb::Eject: {
            std::vector<VolAction> acts;
            std::string err;
            if (!vols_.eject(r.arg, &acts, &err)) {
                result = "error:" + err;
            } else {
                runVol(acts);
                const Volume* v = vols_.byId(r.arg);
                if (v && v->state != VolState::Ejected) result = "error:" + v->error;
            }
            break;
        }
        case Verb::Scrape: runSup(sup_.scrapeRequested(monoNow())); break;
        case Verb::RestartNano: runSup(sup_.restartNano(monoNow())); break;
        case Verb::Brightness: result = setBrightness(std::stoi(r.arg)); break;
    }
    if (answer) store_->set(kKeyDone, doneValue(r, result));
}

void Session::onPropChanged(const std::string& key) {
    if (key == kKeyFirstFrame) {
        if (store_->get(key) == "1") runSup(sup_.firstFrame(monoNow()));
    } else if (key == kKeyRequest) {
        std::string v = store_->get(key);
        if (v.empty()) return;
        Request r;
        std::string err;
        if (!parseRequest(v, &r, &err)) {
            log("bad request '" + v + "': " + err);
            Request bad;
            size_t colon = v.find(':');
            if (colon != std::string::npos && colon > 0 && v.find_first_not_of("0123456789") == colon) {
                bad.seq = v.substr(0, colon);
                store_->set(kKeyDone, doneValue(bad, "error:" + err));
            }
            return;
        }
        if (!reqFilter_.accept(r)) return;
        handleRequest(r, true);
    } else if (key == kKeyPowerctl) {
        Request r;
        if (parsePowerctl(store_->get(key), &r)) handleRequest(r, false);
    } else if (key == kKeyScrapeRun) {
        if (!store_->get(key).empty()) runSup(sup_.scrapeRequested(monoNow()));
    }
}

void Session::reap() {
    for (;;) {
        int st = 0;
        pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0) return;
        auto it = children_.find(pid);
        if (it == children_.end()) continue;
        Child c = it->second;
        children_.erase(it);
        Ms now = monoNow();
        std::string how = WIFEXITED(st) ? "exit " + std::to_string(WEXITSTATUS(st))
                                        : "signal " + std::to_string(WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        switch (c.role) {
            case Role::Nano: {
                if (pid != sup_.nanoPid()) break;
                unlink(P("/run/thimsy/nano.pid").c_str());
                int game = gamePid();
                if (game == pid) game = 0;
                kmsg("nano exited (" + how + ")");
                runSup(sup_.nanoExited(now, game));
                break;
            }
            case Role::Padd:
                log("thimsy-padd exited (" + how + ")");
                runSup(sup_.paddExited(now));
                break;
            case Role::Scrape:
                log("scraper exited (" + how + ")");
                sup_.scrapeExited();
                break;
            case Role::Fsck: {
                auto f = fsckPids_.find(c.job);
                if (f != fsckPids_.end() && f->second == pid) fsckPids_.erase(f);
                log("check " + std::to_string(c.job) + " finished (" + how + ")");
                runVol(vols_.onFsckDone(c.job, exitCodeOf(st)));
                break;
            }
            case Role::Deferred:
                log("deferred services: rcD finished (" + how + ")");
                break;
            case Role::Tool:
                break;
        }
    }
}

void Session::onSignal() {
    struct signalfd_siginfo si;
    bool child = false;
    while (read(sigfd_, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
        if (si.ssi_signo == SIGCHLD) {
            child = true;
        } else {
            log("signal " + std::to_string(si.ssi_signo) + ": stopping");
            quit_ = true;
        }
    }
    if (child) reap();
}

void Session::tick() {
    Ms now = monoNow();
    KeyContext ctx;
    ctx.nanoRunning = sup_.nanoRunning();
    ctx.gameRunning = gamePgid() > 0;
    onKeyActions(keys_.onTick(now, ctx));
    runSup(sup_.onTick(now));
    if (now >= persistVolumeAt_) {
        persistVolumeAt_ = kNever;
        store_->set(kKeyAudioVolume, std::to_string(volumeLevel_));
    }
    if (gamePollPid_ > 0 && now >= gamePollAt_) {
        if (alive(gamePollPid_)) {
            gamePollAt_ = now + 1000;
        } else {
            gamePollPid_ = 0;
            gamePollAt_ = kNever;
            runSup(sup_.gameExited(now));
        }
    }
}

int Session::loop() {
    while (!quit_) {
        tick();
        Ms now = monoNow();
        Ms deadline = earliest(earliest(keys_.nextDeadline(), sup_.nextDeadline()),
                               earliest(persistVolumeAt_, gamePollAt_));
        int timeout = deadline == kNever ? 60000 : static_cast<int>(std::max<Ms>(0, std::min<Ms>(deadline - now, 60000)));
        struct epoll_event evs[16];
        int n = epoll_wait(epfd_, evs, 16, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            log("epoll_wait: " + std::string(strerror(errno)));
            return 1;
        }
        for (int i = 0; i < n; ++i) {
            Tag t = static_cast<Tag>(evs[i].data.u64 >> 32);
            int fd = static_cast<int>(evs[i].data.u64 & 0xffffffffu);
            switch (t) {
                case kTagSignal: onSignal(); break;
                case kTagInotify: {
                    alignas(struct inotify_event) char buf[4096];
                    std::set<std::string> keys;
                    ssize_t len;
                    while ((len = read(inofd_, buf, sizeof buf)) > 0) {
                        for (char* p = buf; p < buf + len;) {
                            struct inotify_event* e = reinterpret_cast<struct inotify_event*>(p);
                            if (e->len && e->name[0] != '.') keys.insert(e->name);
                            p += sizeof(struct inotify_event) + e->len;
                        }
                    }
                    for (const std::string& k : keys) onPropChanged(k);
                    break;
                }
                case kTagUevent: {
                    std::vector<Uevent> list;
                    if (uevents_ && !uevents_->read(&list)) log("reading uevents failed");
                    for (const Uevent& ev : list) handleUevent(ev);
                    break;
                }
                case kTagEvdev: {
                    std::vector<KeyEvent> list;
                    if (!evdev_.read(fd, monoNow(), &list)) {
                        log("input " + evdev_.nameOf(fd) + " went away");
                        delFd(fd);
                        evdevFds_.erase(fd);
                        evdev_.close(fd);
                    }
                    for (const KeyEvent& ev : list) {
                        KeyContext ctx;
                        ctx.nanoRunning = sup_.nanoRunning();
                        ctx.gameRunning = gamePgid() > 0;
                        onKeyActions(keys_.onEvent(ev, ctx));
                    }
                    break;
                }
                case kTagPaddPidfd:
                    delFd(paddPidfd_);
                    close(paddPidfd_);
                    paddPidfd_ = -1;
                    log("thimsy-padd (started by S02padd) exited");
                    runSup(sup_.paddExited(monoNow()));
                    break;
                case kTagGamePidfd:
                    delFd(gamePidfd_);
                    close(gamePidfd_);
                    gamePidfd_ = -1;
                    log("the game Nano left running ended");
                    runSup(sup_.gameExited(monoNow()));
                    break;
            }
        }
    }
    // Stopping: Nano and the scraper get SIGTERM; Nano has 2 s.
    sup_.shutdown();
    pid_t nano = sup_.nanoPid();
    for (const auto& kv : children_) {
        if (kv.second.role == Role::Nano || kv.second.role == Role::Scrape || kv.second.role == Role::Fsck) {
            kill(-kv.first, SIGTERM);
        }
    }
    for (int i = 0; i < 40 && nano > 0; ++i) {
        int st;
        if (waitpid(nano, &st, WNOHANG) == nano) break;
        usleep(50000);
    }
    log("thimsy-session stopped");
    return 0;
}

}  // namespace

int runSession(const Options& opts) {
    Session s(opts);
    if (!s.init()) return 1;
    return s.loop();
}

}  // namespace session
}  // namespace thimsy
