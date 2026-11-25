// GammaOS Nano: the USB mode, kept up in the background. Thimsy always uses File Transfer (MTP); the
// mode setters stay for the other modes the code supports (usbModeProp picks the mode).
//
// Whichever nano process is running (the home, or the resident overlay while an app
// is up) applies it every time a computer is connected and keeps it up while the cable stays in;
// Android resets the gadget on every unplug, so each plug-in applies it again.
//
// The mode applied since the cable went in is kept in sys.gammaos.nano.usb_applied, so the overlay
// that takes over from the home when a game starts does not apply it again (re-enumerating USB under
// a transfer in progress).
//
// File Transfer is applied as a "rejig" (functions none, then mtp). A plain switch to mtp links the
// function but does not reliably start MtpService (measured on the RG DS Plus: the function appeared,
// the service count stayed 0); the none -> mtp toggle re-fires the USB_STATE broadcast that makes
// MediaProvider's MtpReceiver start MtpService, and it does so even when the media module process was
// reaped earlier (the broadcast re-launches it). After each rejig the media module process is pinned
// at oom_score_adj -1000 so the low memory killer never reaps it while MTP is on, and a watchdog
// re-rejigs if the process or the function disappears anyway.
//
// USB presence is read from /sys/class/power_supply/usb/online; sys.usb.state is empty on this
// device so it is never used for that.

#define LOG_TAG "GammaOSNano"

#include "NanoMenu.h"
#include "NanoI18n.h"    // trDyn() runtime translation of the messages
#include "NanoTypec.h"

#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sched.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <utils/String16.h>

namespace android {

namespace {

int64_t mtpNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool readSmallFile(const char* path, std::string& out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[256]; ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if (n <= 0) return false;
    buf[n] = 0; out.assign(buf);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return true;
}

bool writeSmallFile(const char* path, const char* val) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = write(fd, val, strlen(val)); close(fd);
    return n == (ssize_t)strlen(val);
}

// The gadget controller's state is one of states ("configured", "suspended", ...).
bool udcStateIs(std::initializer_list<const char*> states) {
    DIR* d = opendir("/sys/class/udc"); if (!d) return false;
    bool on = false;
    std::string s;
    while (dirent* e = on ? nullptr : readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = std::string("/sys/class/udc/") + e->d_name + "/state";
        if (readSmallFile(p.c_str(), s))
            for (const char* st : states) if (s == st) on = true;
    }
    closedir(d);
    return on;
}

// A USB host is connected. The charger supply's online flag flips with the cable on this device;
// the UDC state ("configured") is the second opinion so a dumb charger does not read as a PC.
bool mtpUsbOnline() {
    std::string s;
    if (readSmallFile("/sys/class/power_supply/usb/online", s) && s == "1") return true;
    // Fall back to the gadget controller's own state when the supply node is missing.
    return udcStateIs({"configured"});
}

// The ffs.mtp function is linked into the active gadget configuration. The link's name depends on the
// device's USB init script (AOSP "function0", Unisoc "f1"), so any link pointing at ffs.mtp counts.
bool mtpFunctionLinked() {
    DIR* d = opendir("/config/usb_gadget/g1/configs/b.1"); if (!d) return false;
    bool linked = false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = std::string("/config/usb_gadget/g1/configs/b.1/") + e->d_name;
        char tgt[256]; ssize_t n = readlink(p.c_str(), tgt, sizeof(tgt) - 1);
        if (n > 0) { tgt[n] = 0; if (strstr(tgt, "ffs.mtp")) { linked = true; break; } }
    }
    closedir(d);
    return linked;
}

// pid of the MediaProvider module process that hosts MtpService, 0 when it is not running.
int mtpMediaModulePid() {
    DIR* d = opendir("/proc"); if (!d) return 0;
    int found = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string p = std::string("/proc/") + e->d_name + "/cmdline";
        int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) continue;
        char buf[128] = {}; ssize_t n = read(fd, buf, sizeof(buf) - 1); close(fd);
        if (n > 0 && strcmp(buf, "com.android.providers.media.module") == 0) { found = atoi(e->d_name); break; }
    }
    closedir(d);
    return found;
}

// Pin (or restore) the media module's OOM priority. -1000 is OOM_SCORE_ADJ_MIN: neither the kernel
// OOM killer nor lmkd will reap it. -700 is what the framework gives it (a persistent process).
void mtpSetMediaModuleAdj(int pid, int adj) {
    if (pid <= 0) return;
    char p[64], v[16];
    snprintf(p, sizeof p, "/proc/%d/oom_score_adj", pid);
    snprintf(v, sizeof v, "%d", adj);
    writeSmallFile(p, v);
}

// File Transfer whenever a computer is connected. Thimsy has no USB Settings row, so a mode stored by
// an older build (persist.gammaos.nano.usb_mode) is not kept.
std::string usbModeProp() {
    return "mtp";
}

std::string usbAppliedProp() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get("sys.gammaos.nano.usb_applied", v, "");
    return v;
}

}  // namespace

std::string NanoMenu::usbMode() const { return usbModeProp(); }

// USB Settings: store the mode (usbModeTick applies it at once when a computer is connected).
void NanoMenu::setUsbMode(const std::string& mode) {
    const std::string m = mode.empty() ? std::string("none") : mode;
    property_set("persist.gammaos.nano.usb_mode", m.c_str());
    mUsbLastApplyMs = 0;   // apply it now, not after a failed attempt's back-off
    if (m == "mtp")
        showXmbMessage(trDyn("USB: File Transfer (MTP)"), trDyn("A connected computer sees the storage"), 240);
    else if (m == "ptp")
        showXmbMessage(trDyn("USB: Photo Transfer (PTP)"), trDyn("A connected computer sees the photos"), 240);
    else if (m == "rndis")
        showXmbMessage(trDyn("USB: USB Ethernet (RNDIS)"), trDyn("Shares the network with a connected computer"), 240);
    else
        showXmbMessage(trDyn("USB: Charging Only"), trDyn("File transfer is switched off"), 240);
    mDisplayDirty = true;
}

// Apply a mode on a detached worker; never two at once.
void NanoMenu::usbApplyAsync(const std::string& mode) {
    if (mUsbApplying.exchange(true)) return;
    ALOGI("usb: applying %s", mode.c_str());
    const int plugGen = mUsbPlugGen;
    std::thread([this, mode, plugGen] {
        const int prevPid = mMtpMediaPid.exchange(0);
        bool ok = true;
        if (mode == "mtp") {
            ok = false;
            for (int attempt = 0; attempt < 3 && !ok && mUsbOnline && usbModeProp() == mode; attempt++) {
                // Drop the transfer function first so the switch back to mtp is a real state change
                // (a switch that only adds mtp on top of the same state does not start MtpService).
                (void)system("gammaos-net usb none 2>/dev/null");
                usleep(1500 * 1000);
                (void)system("gammaos-net usb mtp 2>/dev/null");
                // The gadget re-enumerates and MediaProvider's receiver launches MtpService; give it
                // up to ~10 s, polling for both the linked function and a live host process.
                for (int i = 0; i < 40 && mUsbOnline; i++) {
                    usleep(250 * 1000);
                    const int pid = mtpMediaModulePid();
                    if (pid > 0 && mtpFunctionLinked()) {
                        mtpSetMediaModuleAdj(pid, -1000);
                        mMtpMediaPid = pid; ok = true; break;
                    }
                }
                if (!ok) ALOGW("usb: mtp attempt %d did not bring MTP up", attempt + 1);
            }
        } else {
            ok = system(("gammaos-net usb " + mode + " 2>/dev/null").c_str()) == 0;
        }
        // Hand a host this pinned back to the framework's own priority once MTP no longer needs it,
        // but only if it is still the one alive at -1000 (a restarted host is already the
        // framework's to manage).
        if (prevPid > 0 && prevPid != mMtpMediaPid) {
            char p[64]; snprintf(p, sizeof p, "/proc/%d/oom_score_adj", prevPid);
            std::string cur; if (readSmallFile(p, cur) && cur == "-1000") mtpSetMediaModuleAdj(prevPid, -700);
        }
        // Only a mode that took, and only for the connection it was applied on: an unplug while this
        // ran reset the gadget again.
        if (ok && plugGen == mUsbPlugGen) property_set("sys.gammaos.nano.usb_applied", mode.c_str());
        mUsbLastApplyMs = mtpNowMs();
        mUsbApplying = false;
        ALOGI("usb: %s %s", mode.c_str(), ok ? "applied" : "could not be applied");
    }).detach();
}

// The USB-C port's power role for a monitor (NanoTypec.cpp), checked twice a second by whichever
// Nano process holds the lock, so the home and the overlay never both write the port. The state is
// in sys.gammaos.nano.typec, so the process that takes the lock over carries on with it.
namespace {

std::vector<typec::PortInfo> typecPorts() {
    std::vector<typec::PortInfo> out;
    DIR* d = opendir(typec::kClass);
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.compare(0, 4, "port") != 0 || n.find('-') != std::string::npos) continue;   // not a partner or cable
        typec::PortInfo p;
        p.name = n;
        const std::string base = std::string(typec::kClass) + "/" + n;
        struct stat st;
        p.typeWritable = stat((base + "/port_type").c_str(), &st) == 0 && (st.st_mode & S_IWUSR);
        readSmallFile((base + "/usb_power_delivery_revision").c_str(), p.pdRevision);
        out.push_back(p);
    }
    closedir(d);
    return out;
}

// A directory's entries whose names start with prefix.
std::vector<std::string> dirEntries(const std::string& dir, const std::string& prefix) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0) out.push_back(e->d_name);
        closedir(d);
    }
    return out;
}

// One of dir's prefix* entries has a DisplayPort alternate mode (svid ff01); with displayOnly, one
// that can show a picture (its mode VDO says UFP_D: bit 0 of the port capability), not only send one.
bool hasDpAltMode(const std::string& dir, const std::string& prefix, bool displayOnly) {
    std::string s;
    for (const auto& n : dirEntries(dir, prefix)) {
        if (!readSmallFile((dir + "/" + n + "/svid").c_str(), s) || s != "ff01") continue;
        if (!displayOnly) return true;
        if (readSmallFile((dir + "/" + n + "/vdo").c_str(), s) && (strtoul(s.c_str(), nullptr, 16) & 1)) return true;
    }
    return false;
}

// A DisplayPort connector has a monitor (its hot-plug line is up).
bool dpConnected() {
    std::string s;
    for (const auto& n : dirEntries("/sys/class/drm", "card"))
        if (n.find("-DP-") != std::string::npos &&
            readSmallFile(("/sys/class/drm/" + n + "/status").c_str(), s) && s == "connected")
            return true;
    return false;
}

}  // namespace

// On a thread of its own, at normal priority: the render loop parks (a DS session, DRM sleep) and
// runs at real-time priority, neither of which suits sysfs polling and starting a service.
void NanoMenu::typecStart() {
    if (mTypecThreadStarted) return;
    mTypecThreadStarted = true;
    std::thread([this] {
        sched_param sp = {};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
        setpriority(PRIO_PROCESS, gettid(), 10);
        for (;;) {
            typecTick();
            usleep(500 * 1000);
        }
    }).detach();
}

void NanoMenu::typecTick() {
    const int64_t now = mtpNowMs();
    if (!mTypecBootDone) {   // not while the boot is still bringing USB up
        char bc[PROPERTY_VALUE_MAX] = {};
        property_get("sys.boot_completed", bc, "0");
        if (bc[0] != '1') return;
        mTypecBootDone = true;
    }
    if (mTypecLockFd < 0) {
        if (mTypecLockTryMs != 0 && now - mTypecLockTryMs < 5000) return;
        mTypecLockTryMs = now;
        const int fd = open("/data/system/nano_typec.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return; }
        mTypecLockFd = fd;   // held until the process exits
    }
    if (mTypecPort.empty() || now - mTypecPortMs > 60000) {   // looked for again now and then (a late driver)
        const std::vector<typec::PortInfo> ports = typecPorts();
        mTypecPort = typec::pdPort(ports);
        mTypecPortMs = now;
        mTypecOthers.clear();
        for (const auto& p : ports) mTypecOthers.push_back(p.name);
        if (mTypecPort.empty()) return;   // no port that negotiates and can be set
        // DisplayPort over USB-C: the port has the alternate mode, or the Unisoc driver for it is in.
        mTypecDp = hasDpAltMode(std::string(typec::kClass) + "/" + mTypecPort, mTypecPort + ".", false) ||
                   access("/sys/module/sprd_typec_displayport", F_OK) == 0;
        property_set("sys.gammaos.nano.typec_port", (std::string(typec::kClass) + "/" + mTypecPort).c_str());
    }
    const std::string port = std::string(typec::kClass) + "/" + mTypecPort, partner = port + "-partner";
    typec::View v;
    std::string s;
    if (!readSmallFile((port + "/port_type").c_str(), s)) { mTypecPort.clear(); return; }
    v.portType = typec::current(s);
    if (readSmallFile((port + "/power_role").c_str(), s)) v.powerRole = typec::current(s);
    if (readSmallFile((port + "/data_role").c_str(), s)) v.dataRole = typec::current(s);
    v.partner = access(partner.c_str(), F_OK) == 0;
    if (v.partner && readSmallFile((partner + "/supports_usb_power_delivery").c_str(), s)) v.pd = s;
    for (const auto& n : mTypecOthers)
        if (access((std::string(typec::kClass) + "/" + n + "-partner").c_str(), F_OK) == 0) v.plugged = true;
    v.dpCapable = mTypecDp;
    // A DP connector counts only if it came up during the try (not a stale one from before).
    if (v.partner) v.displayPort = hasDpAltMode(partner, mTypecPort + "-partner.", true) || (dpConnected() && !mTypecDpAtFlip);
    v.host = udcStateIs({"configured", "addressed", "default", "suspended"});
    if (v.partner && mTypecPartnerMs == 0) mTypecPartnerMs = now;
    if (!v.partner) mTypecPartnerMs = 0;
    // MTP being set up takes the gadget down for seconds (a computer then looks absent): wait for it,
    // but not past 15 s from the plug-in (it is set up with a charger or a monitor too).
    v.hold = (mUsbApplying || (mUsbLastApplyMs != 0 && now - mUsbLastApplyMs < 3000)) &&
             (mTypecPartnerMs == 0 || now - mTypecPartnerMs < 15000);
    if (readSmallFile("/sys/class/power_supply/battery/capacity", s)) v.batteryLow = atoi(s.c_str()) < 10;
    if (v.plugged) mTypecUnpluggedMs = 0;
    else if (mTypecUnpluggedMs == 0) mTypecUnpluggedMs = now;
    char st[PROPERTY_VALUE_MAX] = {};
    property_get("sys.gammaos.nano.typec", st, "");
    const typec::Step r = typec::step(v, st, now, mTypecUnpluggedMs);
    if (r.write && strcmp(r.write, "dual") != 0) {
        // First the state, a wakelock that lapses on its own (the try must run awake) and the guard
        // (nano-typec-guard.sh): a crash or a stall after the write still ends in dual role.
        property_set("sys.gammaos.nano.typec", r.state.c_str());
        writeSmallFile("/sys/power/wake_lock", "nano_typec 40000000000");
        property_set("ctl.restart", "nano-typec-guard");   // a fresh one follows this try from now
        if (!strcmp(r.write, "source")) mTypecDpAtFlip = dpConnected();
        if (!writeSmallFile((port + "/port_type").c_str(), r.write)) ALOGW("typec: port_type %s not written", r.write);
    } else if (r.write) {
        if (!writeSmallFile((port + "/port_type").c_str(), r.write)) {
            ALOGW("typec: port_type dual not written; trying again");
            return;
        }
        writeSmallFile("/sys/power/wake_unlock", "nano_typec");
    }
    if (r.log) ALOGI("typec: %s (%s)", r.log, mTypecPort.c_str());
    if (r.state != st) property_set("sys.gammaos.nano.typec", r.state.c_str());
}

// Called from the main loop every iteration (wall-clock paced, as the home idles at 10 fps and the
// resident overlay parks behind apps): cable edges at 4 Hz, a failed apply retried every 5 s, and
// for MTP the host watchdog every 3 s.
void NanoMenu::usbModeTick() {
    const int64_t now = mtpNowMs();
    if (now - mUsbLastPollMs < 250) return;
    mUsbLastPollMs = now;
    // During boot the mode is applied (in the background, usbApplyAsync) once system_server's USB
    // service is up: before that the helper, a Java process, only failed and retried, taking CPU
    // from the boot. Looked for only with a cable in.
    if (!mUsbBootDone) {
        char bc[PROPERTY_VALUE_MAX] = {};
        property_get("sys.boot_completed", bc, "0");
        if (bc[0] != '1'
            && !(mtpUsbOnline() && defaultServiceManager()->checkService(String16("usb")) != nullptr))
            return;
        mUsbBootDone = true;
    }
    const bool on = mtpUsbOnline();
    // Applying a mode switches the gadget off and on, which can briefly read as an unplug: cable
    // edges wait until it is done (a real unplug is still there to see then).
    if (mUsbApplying && mUsbSeen) return;
    if (!mUsbSeen) {
        mUsbSeen = true;   // this process's first look: the cable was already there, or not
        mUsbOnline = on;
    } else if (on != mUsbOnline) {
        mUsbOnline = on;
        mUsbPlugGen++;
        mUsbLastApplyMs = 0;
        property_set("sys.gammaos.nano.usb_applied", "");   // the gadget is reset on every plug change
    }
    if (!on || mUsbApplying) return;
    const std::string mode = usbModeProp();
    if (mode != usbAppliedProp()) {
        if (mUsbLastApplyMs == 0 || now - mUsbLastApplyMs > 5000) { mMtpMissCount = 0; usbApplyAsync(mode); }
        return;
    }
    // Watchdog: the host process or the function has gone (reaped under memory pressure, or the
    // gadget reset) -> force it back. Rate limited so a host that keeps failing does not thrash the
    // gadget.
    if (mode != "mtp" || now - mUsbLastWatchMs < 3000 || now - mUsbLastApplyMs <= 6000) return;
    mUsbLastWatchMs = now;
    const int pid = mtpMediaModulePid();
    if (pid <= 0) {
        // Android restarts the persistent host on its own within about a second; only force the
        // gadget when it is still missing on the next check, so a reap the system is already fixing
        // does not also re-enumerate USB.
        if (++mMtpMissCount >= 2) { mMtpMissCount = 0; usbApplyAsync(mode); }
    } else if (!mtpFunctionLinked()) {
        mMtpMissCount = 0; usbApplyAsync(mode);
    } else {
        mMtpMissCount = 0;
        if (pid != mMtpMediaPid) {
            // The system restarted the host on its own (persistent process): pin the new one.
            mtpSetMediaModuleAdj(pid, -1000); mMtpMediaPid = pid;
            ALOGI("usb: mtp host restarted as pid %d, pinned", pid);
        }
    }
}

} // namespace android
