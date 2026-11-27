/*
 * Copyright (C) 2026 GammaOS
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

// Overlay XMB: the power-hold in-game overlay.
//
// This file implements the show/hide lifecycle for the overlay instance of
// gammaos-nano (launched as `gammaos-nano --overlay`, see main.cpp). The
// overlay reuses the exact same PS3 XMB renderer as the home menu, but is
// presented on a TRANSLUCENT, SurfaceFlinger-background-blurred layer that
// sits above the running app. The app keeps running; SurfaceFlinger composites
// the blur of whatever is beneath the overlay layer, so we never capture the
// app's framebuffer (which is why this only works while SF + the app are up,
// i.e. Nano mode).
//
// Surface creation (translucent RGBA, hidden, very high Z) happens in
// NanoMenu::readyToRun()'s SF path, branched on mOverlayMode. Here we:
//   - overlayInitLayer(): apply the background blur radius once and confirm the
//     layer starts hidden;
//   - overlayPoll(): every tick, read sys.gammaos.nano.show_overlay (set by
//     PhoneWindowManager on a power-hold) and raise/dismiss accordingly;
//   - overlayShow()/overlayHide(): flip layer visibility + grab/release the
//     evdev input devices so navigation drives the XMB (not the app) while up.

#define LOG_TAG "GammaOSNano"

#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include "NanoMenu.h"
#include "NanoAbxy.h"
#include "NanoDisplayTargetSf.h"
#include "NanoMenuPS3.h"      // ps3::layoutComputeNative for the boot warm-up
#include "NanoMenuPS3Bg.h"    // ps3bg::init for the boot warm-up
#include "NanoSdMove.h"      // emuMoveRunning for the overlay launch check
#include "NanoMenuUtils.h"    // setDrasticNanoRomPath for the overlay launch route
#include "NanoJson.h"        // per-app orientation override persistence
#include "NanoHotkeys.h"
#include "NanoDeviceKeys.h"
#include "NanoLaunch.h"       // shared launch-intent builder, shell quoting, package checks
#include "NanoLaunchCommit.h" // commitLaunch: the launch handshake's writes
#include "NanoOverlayRaise.h"
#include "NanoScreenChoice.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <sched.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include <cutils/properties.h>
#include <sys/system_properties.h>
// Exported by libc but declared only in the internal <sys/_system_properties.h> (PropEdge).
extern "C" uint32_t __system_property_serial(const prop_info* __pi);
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <ui/DisplayState.h>   // ui::DisplayState for overlayUpdateSurfaceSize
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <inttypes.h>
#include <ui/DisplayMode.h>
#include <gui/SurfaceControl.h>
#include <gui/LayerState.h>

namespace android {

// Defined in NanoMenuDrm.cpp. Forward-declared here (rather than pulling in
// NanoMenuDrm.h, which drags in the DRM/NEON blit machinery) so the overlay
// surface can be recreated at the same EGLConfig it was born with.
EGLConfig getEglConfig(const EGLDisplay& display, bool wantAlpha);
void nanoSetOverlayRenderRotation(int rot);

// Records the PIDs frozen by the overlay so they can always be thawed if the
// overlay dies (graceful stop, crash, or kill) - prevents a wedged device.
static const char* kOverlayFrozenMarker = "/data/local/tmp/.nano_overlay_frozen";
#define NANO_IME "com.gammaos.shadercontrol/.NanoImeService"

static void overlayThawFromMarker();
static void overlayTermHandler(int);
// Defined further down (next to the launch builders) but used earlier by
// overlayQuitToHome.
static std::string overlayShq(const std::string& s);
static int overlaySignalPackage(const char* pkg, int sig);
static bool overlayHasForegroundService(const std::string& pkg);

void NanoMenu::overlayInitLayer() {
    // Crash/kill recovery: if a previous overlay instance died while an app was
    // frozen, thaw it now. Then install a SIGTERM/SIGINT handler so a graceful
    // `stop gammaos-nano-overlay` also thaws (init does not restart a stopped
    // service, so the handler is the only safety net there).
    overlayThawFromMarker();
    {
        struct sigaction sa = {};
        sa.sa_handler = overlayTermHandler;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
    }

    // The overlay is always the PS3 XMB layout. Force it on regardless of the
    // persist.gammaos.nano.ps3xmb home-mode flag so the overlay is always the
    // real XMB and never the legacy carousel / text menu.
    mPs3Xmb = true;
    mXmbMode = false;

    if (mFlingerSurfaceControl == nullptr || mSession == nullptr) {
        // SurfaceFlinger may not have produced a usable surface control yet (the
        // service can start at boot_completed before SF is fully ready). Do NOT
        // mark the layer initialised so the threadLoop gate retries on the next
        // tick; cap the retries so a permanently-null control cannot hot-loop.
        if (++mOverlayInitTries <= 150) {     // ~5s at the 33ms idle tick
            return;
        }
        ALOGW("overlay: giving up SF surface control init after %d tries "
              "(mFlingerSurfaceControl=%p)", mOverlayInitTries,
              mFlingerSurfaceControl.get());
        mOverlayInited = true;   // stop retrying; overlay stays inert this run
        return;
    }

    // TRANSLUCENT layer (no opaque flag, no background blur): the running app
    // shows through live and we paint an 80% dark scrim + the XMB chrome over it
    // (render()). No capture/blur/freeze, so the overlay appears instantly. Just
    // make sure we start hidden.
    SurfaceComposerClient::Transaction t;
    t.hide(mFlingerSurfaceControl);
    t.apply();
    mOverlayShown = false;
    mOverlayInited = true;   // only NOW, after the surface control is confirmed
    // GammaOS Nano: DRM-home XOR overlay. Latch that the overlay has run THIS session,
    // now that its SF layer is confirmed up. The crash-respawn trigger in gammaos-nano.rc
    // is gated on this (sys.gammaos.nano.overlay_ran) so it only re-raises a crashed
    // overlay-home AFTER the overlay has genuinely come up once - it can never fire at
    // the cold-boot home (where the overlay has never started) and hot-loop the DRM home.
    property_set("sys.gammaos.nano.overlay_ran", "1");
    // The home selects Nano Keyboard at boot, but it exits when it starts an app, which can cut
    // that short, and a stay in Boot Android hands text entry back to Android's keyboard. The
    // overlay lives as long as the app: select it again here.
    selectNanoIme();

    ALOGI("overlay: layer initialised (translucent live-app + scrim), waiting on "
          "sys.gammaos.nano.show_overlay");

    // Warm up the PS3 XMB NOW, at boot, so the FIRST show is instant. The overlay
    // renders nothing while hidden, so otherwise the expensive one-time init runs on
    // the first show and costs seconds: initPs3Menu() compiles the glass-icon shader,
    // bakes the console-icon normal maps, builds the categories and scans ROMs/apps,
    // and loads the theme; ps3bg::init() compiles the wave shader and loads its
    // geometry + sequence assets. Call them directly here (idempotent: initPs3Menu
    // gates on mPs3MenuBuilt, ps3bg::init on its own ready flag) while the layer is
    // still hidden - no render, no eglSwapBuffers, no input grab - so it is invisible
    // and safe. The GL context is current on this render thread. The first real show
    // then finds mPs3MenuBuilt=true and skips the rebuild, so it pops up immediately.
    // Not while an app is in front, though: the overlay is started by the app-launch
    // hand-off with app_launched=1, and warming the XMB then only competes with the
    // launching game for CPU, disk and ~100 MB of GPU memory it would drop again the
    // moment it parks (overlayGpuPark). Build it on the first raise instead; when the
    // overlay is (re)started as the home (show_overlay=1) it is about to draw, so warm.
    const bool appInFront = property_get_bool("sys.gammaos.nano.app_launched", false)
                            && !property_get_bool("sys.gammaos.nano.show_overlay", false);
    if (mOverlayMode && !mPs3MenuBuilt && appInFront)
        ALOGI("overlay: app in front, skipping the XMB warm-up (built on first raise)");
    if (mOverlayMode && !mPs3MenuBuilt && !appInFront) {
        ALOGI("overlay: warming PS3 XMB (menu + wave) at boot for instant first show");
        mPs3Xmb = true;
        initPs3Menu();
        ps3::layoutComputeNative(mWidth, mHeight);
        ps3bg::init();
        // In-game overlay perf: FREEZE the offscreen-only wave (the glass-icon
        // refraction source; the dark scrim hides the wave itself). It is rendered
        // ONCE and reused every frame - the animation is imperceptible in the small
        // glass icons on the 90% scrim, so it is visually identical while removing
        // the entire per-frame wave cost. Overlay process only; the home XMB and any
        // visible (composited) wave always render live.
        ps3bg::setScrimWaveFreeze(true);
        ALOGI("overlay: warm-up complete (menu built, glass + wave shaders ready)");
    }
}

// Send sig to every process of this package. The PIDs come from ActivityManager
// via dumpsys (binder) rather than a /proc walk: gammaos-nano runs in init's
// bootstrap mount namespace where readdir("/proc") does not enumerate other
// processes even with the readproc group, so /proc-based discovery found
// nothing. kill() on a known PID still works (we run as root). ProcessRecord
// lines read "<pid>:<pkg>[:tag]/uXXX", so grep that exact shape to get every
// process (main + helpers) of the package. Returns the count signalled.
// Return the PIDs of every process of a package (main + helpers), parsed from the
// ActivityManager binder dump - a /proc walk does not enumerate other processes
// from nano's bootstrap mount namespace, dumpsys does. Anchored on "ProcessRecord{"
// so the LRU "<uid>:<pkg>" lines cannot be mistaken for pids.
static std::set<int> overlayGetPids(const char* pkg) {
    std::set<int> pids;
    if (!nanolaunch::isValidPackageName(pkg)) return pids;
    const size_t pkgLen = strlen(pkg);
    const std::string cmd = "dumpsys activity processes " + overlayShq(pkg) + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) { ALOGW("overlay: popen dumpsys failed"); return pids; }
    std::string out;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, r);
    pclose(f);
    size_t rp = 0;
    const std::string rec = "ProcessRecord{";
    while ((rp = out.find(rec, rp)) != std::string::npos) {
        size_t i = rp + rec.size();
        while (i < out.size() && out[i] != ' ' && out[i] != '}') i++;
        if (i < out.size() && out[i] == ' ') i++;
        size_t ds = i;
        while (i < out.size() && out[i] >= '0' && out[i] <= '9') i++;
        // The whole package, not a prefix of another one ("com.foo" must not match "com.foo.bar").
        const size_t pe = i + 1 + pkgLen;
        if (i > ds && i < out.size() && out[i] == ':' &&
            out.compare(i + 1, pkgLen, pkg) == 0 && pe < out.size() &&
            (out[pe] == '/' || out[pe] == ':')) {
            pids.insert(atoi(out.substr(ds, i - ds).c_str()));
        }
        rp += rec.size();
    }
    return pids;
}

// True while any process of the package is still alive. Uses kill(pid,0) on a
// previously captured pid set - reliable even after dumpsys drops the
// ProcessRecord (which happens seconds before the process actually dies).
static bool overlayAnyAlive(const std::set<int>& pids) {
    for (int pid : pids) if (pid > 1 && kill(pid, 0) == 0) return true;
    return false;
}

static int overlaySignalPackage(const char* pkg, int sig) {
    std::set<int> pids = overlayGetPids(pkg);
    // Persist the frozen PIDs to a marker file BEFORE sending SIGSTOP so a SIGTERM
    // handler (graceful `stop`) or a startup-recovery pass after a crash/kill can
    // always thaw the app - otherwise a crash in the tiny window between freezing
    // and recording would leave the foreground app SIGSTOP'd with no record and
    // the device looks frozen. Writing the marker for a pid we then fail to stop
    // is harmless (a redundant SIGCONT later).
    if (sig == SIGSTOP) {
        int fd = open(kOverlayFrozenMarker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            for (int pid : pids) {
                char line[16];
                int len = snprintf(line, sizeof(line), "%d\n", pid);
                if (len > 0) (void)!write(fd, line, len);
            }
            close(fd);
        }
    }
    int n = 0;
    for (int pid : pids) {
        if (pid > 1 && kill(pid, sig) == 0) n++;
    }
    if (sig == SIGCONT) {
        unlink(kOverlayFrozenMarker);
    }
    ALOGI("overlay: %zu pid(s) for %s, signalled %d", pids.size(), pkg, n);
    return n;
}

// SIGCONT every PID listed in the frozen marker file, then remove it. Used both
// as crash recovery on overlay startup and from the SIGTERM handler. kill() and
// the file syscalls here are async-signal-safe.
static void overlayThawFromMarker() {
    int fd = open(kOverlayFrozenMarker, O_RDONLY);
    if (fd < 0) return;
    char buf[256];
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got > 0) {
        buf[got] = '\0';
        const char* p = buf;
        while (*p) {
            int pid = 0;
            while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
            if (pid > 1) kill(pid, SIGCONT);
            while (*p && (*p < '0' || *p > '9')) p++;
        }
    }
    unlink(kOverlayFrozenMarker);
}

static void overlayTermHandler(int) {
    overlayThawFromMarker();
    _exit(0);
}

std::string NanoMenu::overlayResolveForegroundPkg() {
    // Resolve the top resumed package via ActivityManager (binder, works from
    // nano's restricted namespace). Returns a validated real 3rd-party-looking
    // package, or empty if there is nothing we should act on.
    char pkg[256] = {};
    FILE* f = popen("dumpsys activity activities 2>/dev/null | "
                    "grep -m1 ResumedActivity | "
                    "sed -nE 's#.* ([a-zA-Z0-9_.]+)/[^ }]+.*#\\1#p'", "r");
    if (f) {
        if (fgets(pkg, sizeof(pkg), f)) {
            size_t n = strlen(pkg);
            while (n > 0 && (pkg[n-1] == '\n' || pkg[n-1] == '\r' || pkg[n-1] == ' '))
                pkg[--n] = '\0';
        }
        pclose(f);
    }
    // Never target the system; require a real package name with a dot. Do NOT
    // filter the whole "gammaos" namespace: the overlay is a separate --overlay
    // instance of this cc_binary with no package identity, so it can never be a
    // ResumedActivity, while com.gammaos.* apps such as com.gammaos.drasticsf are
    // real foreground apps that MUST be recognized here (otherwise the launch
    // handoff never sees them come to the front and the overlay only clears via
    // its 12s backstop, leaving the app hidden behind the XMB). quickKillApps
    // keeps its own independent gammaos kill-protection, so nothing gammaos gets
    // force-stopped by dropping the substring here.
    if (pkg[0] == '\0' || strchr(pkg, '.') == nullptr ||
        strcmp(pkg, "android") == 0 ||
        strncmp(pkg, "com.android.systemui", 20) == 0) {
        return std::string();
    }
    return std::string(pkg);
}

void NanoMenu::overlayPauseApp(bool pause) {
    // DEFAULT OFF: do NOT freeze the background app. The opaque overlay layer
    // occludes the app so the compositor scans out only the overlay (60fps) while
    // the app keeps running underneath - freezing (SIGSTOP) was causing apps to
    // fail to resume cleanly. Re-enable with persist.gammaos.nano.overlay.pause=1.
    //
    // NOTE: mOverlayPausedPkg is populated by overlayShow() regardless of this
    // flag (so quit/launch always know the target). This routine only sends the
    // freeze/thaw signals, gated by the flag.
    if (!property_get_bool("persist.gammaos.nano.overlay.pause", false)) {
        return;
    }

    if (pause) {
        if (mOverlayPausedPkg.empty()) {
            ALOGI("overlay: no pausable foreground app");
            return;
        }
        std::string p = mOverlayPausedPkg;
        std::thread([p]() {
            int n = overlaySignalPackage(p.c_str(), SIGSTOP);
            ALOGI("overlay: paused (SIGSTOP) %s -> %d process(es)", p.c_str(), n);
        }).detach();
    } else {
        if (mOverlayPausedPkg.empty()) return;
        std::string p = mOverlayPausedPkg;
        std::thread([p]() {
            int n = overlaySignalPackage(p.c_str(), SIGCONT);
            ALOGI("overlay: resumed (SIGCONT) %s -> %d process(es)", p.c_str(), n);
        }).detach();
    }
}

void NanoMenu::pspClockFreezeApp(bool freeze) {
    // Freeze App Under Clock (persist.gammaos.nano.pspclock.freezeapp). Pause the game via its OWN
    // Android lifecycle - NOT SIGSTOP/cgroup freeze, which left apps like RetroArch deadlocked on
    // resume (the OS keeps sending binder to the frozen process, gets errors, and it never recovers).
    // freeze=true fronts ShaderControl's transparent NanoPauseActivity so the game receives onPause and
    // pauses itself; freeze=false re-launches it with the "finish" extra so it dismisses and the game
    // gets onResume. nano's overlay is a SurfaceFlinger layer far above the activity, so the clock UI
    // is unaffected and the launcher is never brought up. Shelled out on a detached thread so the
    // render thread never stalls on am. Only meaningful when a real foreground app is under the summon.
    if (mOverlayPausedPkg.empty()) return;
    ALOGI("pspclock: %s %s via NanoPauseActivity", freeze ? "pause" : "resume", mOverlayPausedPkg.c_str());
    // One worker sends the requests in order, newest last: a pause still being sent can never land
    // after the finish that followed it and leave the game paused.
    static std::mutex sMutex;
    static std::condition_variable sCv;
    static bool sWant = false, sWorker = false;
    static uint64_t sRequested = 0, sSent = 0;
    std::lock_guard<std::mutex> lk(sMutex);
    sWant = freeze;
    sRequested++;
    sCv.notify_one();
    if (sWorker) return;
    sWorker = true;
    std::thread([]() {
        std::unique_lock<std::mutex> lk(sMutex);
        for (;;) {
            sCv.wait(lk, [] { return sRequested != sSent; });
            const bool fin = !sWant;
            const uint64_t req = sRequested;
            lk.unlock();
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "am start --user 0 -n com.gammaos.shadercontrol/.NanoPauseActivity "
                     "--activity-no-animation%s 2>/dev/null",
                     fin ? " --ez finish true" : "");
            system(cmd);
            lk.lock();
            sSent = req;
        }
    }).detach();
}

void NanoMenu::overlayShow() {
    mPanelRaisePending = false;
    mParkMode.clear();   // the parked loop compares from its next look
    if (!mPanelApp.empty()) panelAppFocus(false);   // dual screen: the controller comes back from the panel's app
    // Raised by the Anbernic button over the app (PhoneWindowManager): 1 opens the Quick Menu, 2 the
    // Applications view (Anbernic + Start), 3 the button map alone (Anbernic + the bottom button). Consumed up front so a raise that does not happen cannot
    // leave it set for the next power-hold.
    const int summon = property_get_int32("sys.gammaos.nano.qs_summon", 0);
    if (summon) property_set("sys.gammaos.nano.qs_summon", "0");
    if (mOverlayShown) return;
    const int64_t raiseStartMs = uptimeMillis();
    mAnbernicArmed = mAnbernicHeld = false;   // read again from the devices below, after the drain
    mSsHeld[0] = mSsHeld[1] = false;
    mBackHeld = false;
    mKeyReplay.clear();
    navRelease(NavDir::None);   // its release may be among the events dropped while hidden
    if (mFlingerSurfaceControl == nullptr) {
        ALOGW("overlay: show requested but no SF surface control; clearing request");
        property_set("sys.gammaos.nano.show_overlay", "0");
        return;
    }
    // The power-button overlay is taking over the top layer: cancel any in-flight focus-ring pulse and
    // hand ownership of mFlingerSurfaceControl to the overlay (do NOT let the ring hide it underneath).
    mCcRingDisp = -1;
    mTopRingShown = false;
    // Raising the full XMB/overlay ends any KEY_ALL_APPLICATIONS force-visible CC session: dropping back to
    // the app should not silently re-show the CC over it (the override is per-summon, not sticky).
    mCcForceVisible = false;

    // We deliberately do NOT mlockall() here on raise. The overlay's pages were released
    // by munlockall() while it was parked behind a running app (see threadLoop) so the game
    // could use that ~122MB. Re-locking here called mlockall(MCL_CURRENT), which faulted all
    // ~122MB back from zram SYNCHRONOUSLY on the render thread; over a live app (which must
    // be swapped out to make room) that stalled the render loop past the render watchdog ->
    // SIGABRT -> the overlay crashed and respawned, so it could never be shown or dismissed
    // (the reported "overlay won't dismiss" bug). Instead let the pages demand-fault from
    // zram (fast, lz4) as the overlay renders, spread across frames so the heartbeat keeps
    // ticking. The original mlockall (still unconditional at threadLoop, and active for the
    // DRM cold-boot home) guarded glyph faults off the SLOW lz4 EROFS system image; the
    // overlay's pages live in fast zram after parking, so demand-faulting them carries none
    // of that EROFS thrash/OOM risk.

    // The offscreen wave is frozen (rendered once, reused as the glass-icon
    // refraction source). We deliberately do NOT invalidate it on each show: the
    // overlay frees its 21MB of wave keyframes after the first bake (to give a
    // running game that RAM), so forcing a rebuild here would reload them off disk
    // and stall the power-hold raise. Reusing the baked frame keeps the raise
    // instant; the refraction is blurred and scrim-hidden so a slightly stale
    // day/night tint is imperceptible, and a real theme/day-night change still
    // bumps the wave epoch (setThemeColor/setDayNightBlend) to force one rebuild.

    // The FIXED_PERFORMANCE perf hint is owned by overlayApplyPresentMode (it is
    // mode-dependent: scrim only). See the comment there.

    // Resolve the foreground package so quit/launch know what to act on. The
    // dumpsys resolve intermittently returns empty for a live game from the
    // overlay's process context; fall back to the tracked launch_app so
    // quit/launch/ESC still target the right package.
    mOverlayPausedPkg = overlayResolveForegroundPkg();
    if (mOverlayPausedPkg.empty() &&
        property_get_bool("sys.gammaos.nano.app_launched", false)) {
        char la[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.launch_app", la, "");
        if (strchr(la, '.') != nullptr && nanolaunch::isValidPackageName(la)) mOverlayPausedPkg = la;
    }

    // Decide scrim-over-app vs full-wallpaper. A RWC hint (set when the overlay is
    // raised as the launcher after an app exits) forces wallpaper even if dumpsys
    // transiently still reports the dying app; otherwise infer from "no fg app".
    {
        char wp[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.overlay_wallpaper", wp, "0");
        // Wallpaper (launcher) vs scrim-over-app: decide from app_launched (a
        // reliable prop set when a nano app is running), NOT the dumpsys pkg resolve
        // above - that intermittently returns empty for a live game (RetroArch) and
        // wrongly flipped the in-game overlay to the full wave wallpaper. An app is
        // behind us iff app_launched==1 -> scrim; otherwise (launcher) -> wallpaper.
        // The RWC hint still forces wallpaper for the post-exit launcher raise even
        // in the moment before app_launched clears.
        // app_launched is the SOLE signal: scrim over the live app whenever a nano
        // app is launched, full wallpaper only in the launcher (no app). A stale
        // overlay_wallpaper hint must NOT force the wave over a running game (that
        // was the "submenus show the wave" bug). The launcher raise sets
        // app_launched=0 before show, so it correctly gets the wallpaper.
        bool appBehind = property_get_bool("sys.gammaos.nano.app_launched", false);
        mOverlayWallpaper = !appBehind;
        property_set("sys.gammaos.nano.overlay_wallpaper", "0");   // consume any hint
        ALOGI("overlay: show wallpaper=%d (app_launched=%d wp=%s paused=%s)",
              mOverlayWallpaper ? 1 : 0, appBehind ? 1 : 0, wp, mOverlayPausedPkg.c_str());
    }

    // PSP clock STANDALONE summon: the framework raised us (show_overlay=1) with
    // pspclock_summon=1 to invoke the clock over a running app via the slide, without the
    // user opening the overlay first. Force scrim-over-app (never the wave wallpaper) and
    // open the clock directly - there is no XMB behind it, so the entrance goes straight to
    // the exploding glyphs + disc drop (the blow-away producers no-op on mPspClockStandalone),
    // and nano lowers the overlay again once the clock retracts (drawPspClock teardown).
    if (property_get_bool("sys.gammaos.nano.pspclock_summon", false)) {
        mOverlayWallpaper = false;             // scrim over the live app, not the wave
        mPspClockStandalone = true;
        mPspClockRaisedOverlay = true;
        if (!mPspClockOn) mPspIconSeed += 17;  // fresh per-summon entrance avalanche
        esdeOverlayCloseAll();                 // it would hide the clock
        mPspClockOn = true;
        mPspGlyphBurst = 1.0f;
    }

    // Layer opacity + swap pacing for the chosen mode (shared helper: the
    // quit-to-launcher path flips wallpaper mode WITHOUT a hide+show cycle and
    // must apply the exact same state).
    overlayApplyPresentMode();

    // The map alone over a game needs none of the XMB: it draws on its first frame.
    const bool mapOnly = overlayraise::mapOnly(summon, mOverlayWallpaper);
    // Bind the hotkey keys in the emulators' settings for the next launch; the running app may write
    // its own back when it exits, so it is left until nano returns from it.
    std::thread([pkg = mOverlayPausedPkg]() { nanohotkeys::provision(pkg); }).detach();
    // Isolate the running app's input via the FRAMEWORK drop_input path: while it
    // is set, InputDispatcher drops keys + motion to the app (POWER and BACK are
    // exempt) so the app cannot act on XMB navigation - the same principle as
    // Global Actions taking input for its own menu. nano reads evdev directly for
    // the XMB. No EVIOCGRAB (which fought PhoneWindowManager's power gesture).
    property_set("sys.gammaos.nano.drop_input", "1");

    // Prioritise this render thread (SCHED_FIFO) so the XMB stays smooth while it
    // GPU-composites over the LIVE app. The service has CAP_SYS_NICE + rtprio 99.
    // Match SurfaceFlinger's RT priority (its main + RenderEngine run at FIFO 2)
    // rather than sit ABOVE it: at FIFO 4 we preempted SF mid-CTM-composite on the
    // A53, pushing its present past the vsync deadline and causing the wave
    // wallpaper's ~50fps vsync-beat. Equal priority round-robins on contention so
    // SF keeps its composite slice, while we stay above all SCHED_OTHER threads.
    {
        struct sched_param sp = {};
        sp.sched_priority = 2;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            ALOGW("overlay: SCHED_FIFO boost failed: %s", strerror(errno));
    }

    // DEFER the SF show: do NOT t.show() here. render() shows the layer only AFTER
    // it has composited the first faded-out (reveal~0) frame, so the entrance
    // animates IN instead of flashing the full XMB (the stale layer buffer). See
    // mOverlayPendingShow handling after the overlay eglSwapBuffers in render().
    mOverlayPendingShow = true;

    // Drain stale evdev events buffered while the overlay was HIDDEN (nano does
    // not read its input fds while idle, so they accumulate - including the power
    // gesture's own queued events, or a Back the user pressed in the app just
    // before summoning). Without this the very first pollInput flushes an old
    // Back/button press into the XMB and instantly dismisses the overlay ("shows
    // then disappears"). The fds are O_NONBLOCK so this returns at once.
    {
        struct input_event ev;
        for (int fd : mInputFds) {
            if (fd < 0) continue;
            while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) { /* discard */ }
            nanoabxy::forget(fd);
        }
        touchResync(-1, true);
    }
    // Raised by Anbernic + Start: the button can still be down, for a following chord.
    mAnbernicHeld = anbernicPhysicallyDown(&mAnbernicFd);

    mAppsSummoned = false;
    mOverlayXmbDeferred = mapOnly;
    if (!mapOnly) overlayPrepareXmb(summon);

    mOverlayShown = true;
    // Fade + float-in of the XMB chrome. -2 = pending; the first
    // rendered frame begins it and it then advances by clamped per-frame dt, so the
    // one-time lazy work on the first-ever raise cannot skip the animation.
    mOverlayEnterStart = -2.0f;
    mOverlayEnterElapsed = 0.0f;
    mPs3ChromeIconReveal = 0.0f;
    mPs3ChromeLabelReveal = 0.0f;
    mLastFrameNs = 0;
    // Publish nano's orientation immediately so WM clamps the display to landscape
    // (or the chosen nano orientation) the instant the overlay raises, instead of
    // waiting for the next periodic tick - otherwise the overlay flashes truncated
    // over a portrait app.
    orientationTick();
    ALOGI("overlay: shown (translucent live-app + scrim, drop_input=1)");
    // The button map over the app; with nothing to map (an Android game, Settings) the layer goes
    // down again before its first frame shows.
    if (mapOnly) {
        bmapOpenOverApp();
        if (!bmapActive()) overlayResume();
        else ALOGI("overlay: button map ready %lld ms after the raise began", (long long)(uptimeMillis() - raiseStartMs));
    }
}

// The XMB side of a raise: the lists the home may have changed since the last one, the GPU working
// set parked behind the app, and where the menu opens (summon as in overlayShow).
void NanoMenu::overlayPrepareXmb(int summon) {
    mOverlayXmbDeferred = false;
    esdeGamelistFlush();   // before the lists are read back from the files
    esdeStoreTick();
    // Re-apply the user's saved Theme Settings (wave colour, day/night, particles)
    // every time the overlay is raised, so the overlay wallpaper matches whatever
    // the user picked in the home XMB's Theme Settings - including changes made
    // AFTER this resident process started (persist props are only read once at
    // startup otherwise, leaving the overlay on the default wave).
    loadPs3ThemeSettings();

    // Reload Recently Played from disk on every raise. This resident overlay loaded
    // mXmbRecent once at startup; each game launch (this process AND the home DRM
    // nano's first launch) rewrites /data/system/nano_xmb_recent.list, so the
    // in-memory list drifts and the submenu showed a stale top entry / wrong index.
    // Re-reading here makes the list always reflect the most recently launched game.
    loadXmbRecent();
    loadCollections();
    {   // starred or un-starred from the home since the last raise: re-order those games' systems
        const auto favs = mXmbFavorites, unfavs = mEsdeUnfavorites;
        loadFavorites();
        std::unordered_set<std::string> changed;
        auto diff = [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
            for (const auto& p : a) if (std::find(b.begin(), b.end(), p) == b.end()) changed.insert(hiddenKey(p));
        };
        diff(favs, mXmbFavorites); diff(mXmbFavorites, favs);
        diff(unfavs, mEsdeUnfavorites); diff(mEsdeUnfavorites, unfavs);
        for (size_t s = 0; s < mXmbSystems.size() && !changed.empty(); s++)
            for (const auto& r : mXmbSystems[s].roms)
                if (changed.count(hiddenKey(r))) { favoritesResort((int)s, std::string()); break; }
        if (!changed.empty()) mPs3CatsStale = true;   // the Favorites entry comes or goes
    }
    // Games hidden or shown again from the home since the last raise: filter the lists now, and
    // rescan when one came back (the lists no longer hold it).
    {
        const size_t before = mHiddenGames.size();
        if (loadHiddenGames()) {
            refreshListsAfterHide();
            if (mHiddenGames.size() <= before) forceRescanAllSystems();
        }
    }

    // Power-button raise over a running app: land on the Quick Menu (the GammaOS
    // legacy global actions) as the default category, mirroring the PS3 in-game XMB
    // opening on its system row. Only in scrim-over-app mode; when re-raised as the
    // post-exit launcher (wallpaper mode) keep the normal home category. Snap with no
    // rail animation and clear any leftover submenu / modal from a prior raise. The
    // Quick Menu is always the first category buildPs3Cats pushes (index 0).
    overlayGpuUnpark();   // ensure the categories + their GPU assets exist before the snap
    int quickIdx = (mPs3QuickCatIdx >= 0) ? mPs3QuickCatIdx : 0;
    if (!mOverlayWallpaper && quickIdx < (int)mPs3Cats.size()) {
        mPs3Stack.clear();
        mPs3DlgActive = false; mPs3BrightSlider = false;
        mMenuState = MENU_MAIN;   // start each scrim raise at a clean top-level menu state;
                                  // a stale MENU_APPS/RECENT here used to block Back-to-dismiss
        mPs3CatIdx  = quickIdx;
        mPs3ItemIdx = 0;
        mPs3CatItemSel[mPs3CatIdx] = 0;
        mPs3AnimItem = 0.0f; mPs3ItemAnimStart = -1.0f; mPs3SubAnimStart = -1.0f;
        mPs3CatAnimActive = false; mPs3CatT = 1.0f; mPs3CatFromOffset = 0.0f;
    }
    if (mOverlayWallpaper) esdeReturnHome();   // raised as the launcher after an app exited
    if (summon == 1 && !mOverlayWallpaper && !quickMenuOpen()) toggleQuickMenu();
    if (!mOverlayWallpaper) mAppsReturn = QsReturn();   // the menu was reset above
    if (summon == 2 && !mOverlayWallpaper) {
        openApplicationsView();
        mAppsSummoned = applicationsViewOpen();
    }

    // DSi theme, app-EXIT return only (wallpaper/launcher raise, app_launched=0): replay the
    // card-drop entrance cascade like a fresh home (user request), and reflect the Recently
    // Played reorder. A scrim raise OVER a running app (app_launched=1) does neither - the
    // menu just appears over the paused game. loadXmbRecent() above already reloaded the list
    // (the just-played game moved to the front); rebuild any open Recently Played level from it
    // so it shows the new order and lands on that game, and re-arm the intro so the cards drop.
    if (mOverlayWallpaper && mNdsTheme) {
        mNdsIntroStart = 0;   // renderNdsCarousel re-fires the DSi card-drop on the next frame
        bool rebuiltRecent = false;
        for (auto& lvl : mPs3Stack) {
            if (!lvl.items.empty() && lvl.items[0].kind == PS3_RECENT) {
                buildRecentSubmenu(lvl);   // rebuild from the freshly reloaded mXmbRecent
                lvl.sel = 0;               // the just-played game is now the front card
                rebuiltRecent = true;
            }
        }
        if (rebuiltRecent) {
            mNdsCamera = (float)ndsFocusSel();
            mNdsScrubbing = false; mNdsFlingVel = 0.0f; mNdsFastScroll = false;
            mPs3AnimItem = (float)ndsFocusSel(); mPs3ItemAnimStart = -1.0f;
        }
    }
}

// Drop the XMB GPU working set while the overlay is parked behind a foreground app.
// Runs on the render thread with the GL context current. Everything freed here comes
// back on demand: the maps are lazy caches that refill on a miss, the wave keyframes
// reload on the next live wave frame, the blur scratch on the next capture, and the
// category icons (whose handles buildPs3Cats copied into the Ps3Cat entries) are
// reloaded plus the categories rebuilt by overlayGpuUnpark before the next raise.
// Kept alive on purpose: the glyph atlas, the 21 base console icons, the glass shader
// program with its two tiny support textures, the Control Center static cache and the
// wave scene FBOs, so the bottom-panel Control Center keeps rendering identically.
void NanoMenu::overlayGpuPark() {
    if (mOverlayGpuParked) return;
    mOverlayGpuParked = true;
    esdeOverlayCloseAll();   // frees the media viewer's texture
    esdeBgVideoStop();       // and a theme or game video (and its sound) behind the app
    // The open submenu levels (mPs3Stack) are NOT rebuilt on the raise: their items keep the
    // icon and normal-map handles they were built with, by value. Freeing those left every icon
    // of the open level black after returning from an app, and once GL handed the freed ids to
    // new textures an icon could even show an unrelated image (the XMB background), until the
    // user left and re-entered the level. Keep every texture an open level references (with its
    // cache entry, so the caches stay coherent); it is only the open level's icons.
    std::set<GLuint> live;
    for (const auto* st : {&mPs3Stack, &mAppsReturn.stack, &mQsReturn.stack})   // and the stacks restored later
        for (const auto& lvl : *st)
            for (const auto& it : lvl.items) {
                if (it.iconTex) live.insert(it.iconTex);
                if (it.nmapTex) live.insert(it.nmapTex);
            }
    auto keep = [&live](GLuint t) { return t == 0 || live.count(t) != 0; };
    ps3bg::freeWaveSeq();
    if (mOverlayBgTex) { glDeleteTextures(1, &mOverlayBgTex); mOverlayBgTex = 0; }
    freeGlassScratch();
    glassScratchFree();
    scraperFreeBoxart();
    iconGridResetCache();
    auto dropMap = [&keep](auto& m) {
        for (auto it = m.begin(); it != m.end();) {
            if (keep(it->second)) { ++it; continue; }
            glDeleteTextures(1, &it->second);
            it = m.erase(it);
        }
    };
    dropMap(mPs3NmapByIcon);
    dropMap(mPs3IconTexByIndex);
    dropMap(mPs3BevelByIconIdx);
    dropMap(mGpGlassNmaps);
    for (auto it = mPs3IconRefCache.begin(); it != mPs3IconRefCache.end();) {
        // An entry an open level still draws stays whole (its icon and its normal map).
        if (keep(it->second.first) && keep(it->second.second)) { ++it; continue; }
        // The failure entries alias a base console icon + its bevel (not owned here);
        // the bevel map above already dropped the bevel, the base icon stays.
        if (it->second.first && it->second.first != mIconTextures[16] && !keep(it->second.first))
            glDeleteTextures(1, &it->second.first);
        if (it->second.second && !keep(it->second.second))
            glDeleteTextures(1, &it->second.second);
        it = mPs3IconRefCache.erase(it);
    }
    dropMap(mPs3AppIcons);
    // The category icons are reloaded by overlayGpuUnpark (ps3LoadCatIcons), which also
    // replaces any handle kept here; remember them so it can repoint the open levels.
    for (int i = 0; i < 7; i++) {
        if (mPs3CatTex[i] && !keep(mPs3CatTex[i])) {
            glDeleteTextures(1, &mPs3CatTex[i]);
            mPs3CatTex[i] = 0;
        }
        mParkedCatTex[i] = mPs3CatTex[i];   // non-zero only when an open level holds it
        mPs3CatNmap[i] = 0;   // owned by mPs3NmapByIcon, handled above
    }
    esdeArtCancelAll();
    for (auto& kv : mEsdeTexCache) if (kv.second) glDeleteTextures(1, &kv.second);
    mEsdeTexCache.clear();
    mEsdeMediaUse.clear();
    for (auto& kv : mEsdeSvgCache) if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
    mEsdeSvgCache.clear();
    for (auto& kv : mEsdeAnimCache) for (GLuint t : kv.second.frames) if (t) glDeleteTextures(1, &t);
    mEsdeAnimCache.clear();
    for (int p = 0; p < 2; p++) {
        if (mNdsFxFbo[p]) { glDeleteFramebuffers(1, &mNdsFxFbo[p]); mNdsFxFbo[p] = 0; }
        if (mNdsFxTex[p]) { glDeleteTextures(1, &mNdsFxTex[p]); mNdsFxTex[p] = 0; }
        mNdsFxW[p] = mNdsFxH[p] = 0;
    }
    if (mPs3TzHeaderTex) { glDeleteTextures(1, &mPs3TzHeaderTex); mPs3TzHeaderTex = 0; }
    // DSi theme sprites + the 36 launch ring frames: ensureNdsAssets / ensureNdsRing are
    // one-shot loaders on the render paths, so clearing their guards reloads on demand.
    for (GLuint* t : {&mNdsFrameTex, &mNdsTileTex, &mNdsPhotoTex, &mNdsBattTex})
        if (*t) { glDeleteTextures(1, t); *t = 0; }
    mNdsTexLoaded = false;
    for (int i = 0; i < 36; i++)
        if (mNdsRingTex[i]) { glDeleteTextures(1, &mNdsRingTex[i]); mNdsRingTex[i] = 0; }
    mNdsRingLoaded = false;
    // The wave scene (shaders, gradient/work/half FBOs, geometry): a freshly started
    // overlay behind an app never initialises it either, so the parked Control Center
    // already renders without it in the common case; overlayGpuUnpark re-inits it.
    ps3bg::shutdown();
    // The 21 console icons stay: every built item (the root columns AND any open list
    // level) holds their texture handles by value, so freeing and reloading them here
    // left the open level's system and box art tiles blank until the user re-entered
    // the category (2026-09-26). Not worth the 7 MB.
    glFlush();
    ALOGI("overlay: parked, XMB GPU working set dropped");
    overlayPageOutSelf();
}

// Rebuild what overlayGpuPark dropped, before the overlay draws again. The XMB
// hierarchy itself (initPs3Menu) is only built here if it never was (the boot warm-up
// is skipped while an app is in front); otherwise reload the category icons and
// rebuild the categories so the Ps3Cat/Ps3Item handle copies are fresh. Everything
// else refills lazily as it is drawn.
void NanoMenu::overlayGpuUnpark() {
    if (!mPs3MenuBuilt) {
        initPs3Menu();
    } else if (mOverlayGpuParked) {
        initGlassIcons();
        ps3LoadCatIcons();
        // Point the open levels that copied a category icon (Home Categories, the Quick Menu
        // power row) at the reloaded one; their other icons were kept alive by overlayGpuPark.
        for (int i = 0; i < 7; i++) {
            GLuint was = mParkedCatTex[i];
            mParkedCatTex[i] = 0;
            if (!was || was == mPs3CatTex[i]) continue;
            for (auto& lvl : mPs3Stack)
                for (auto& it : lvl.items)
                    if (it.iconTex == was) it.iconTex = mPs3CatTex[i];
        }
        // Rebuild by LABEL, not by index. The launch that parked us added the game to
        // Recently Played (and may have starred, pinned or collected it), which inserts
        // "Recently Played" / Favorites / Pinned Apps / Collections entries at the front of
        // the Game column; the plain rebuild kept the old item index, so after the first
        // ever game the home came back one tile early (a NDS launch landed on N64).
        rebuildPs3CatsPreserveSel();
    }
    if (mOverlayMode && !ps3bg::ready()) {
        ps3bg::init();
        ps3bg::setScrimWaveFreeze(true);   // as the boot warm-up does (see overlayInitLayer)
    }
    if (mOverlayGpuParked) ALOGI("overlay: raised, XMB GPU working set rebuilt");
    mOverlayGpuParked = false;
}

// App launch hand-off: free the kernel's reclaimable dentry/inode caches. The ROM
// library scans (nano's own on every home return, plus the media scanner walking the
// same files through FUSE) leave 100 MB and more of them behind, and on the 1 GB RG
// DS the kernel then evicts and re-reads system_server's and the app's code pages for
// seconds rather than shrink that cache: measured 9 to 12 s to the RetroArch first
// frame with the cache in place, 5 to 7 s with it dropped just before the launch.
// Dropping it costs only the next scan's metadata re-reads. Best effort (root only).
void NanoMenu::nanoDropReclaimableCaches() {
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { ALOGW("NanoMenu: drop_caches open failed: %s", strerror(errno)); return; }
    if (write(fd, "2", 1) != 1) ALOGW("NanoMenu: drop_caches write failed: %s", strerror(errno));
    close(fd);
    ALOGI("NanoMenu: dropped the reclaimable kernel caches for the app launch");
}

// Parked overlay: push our own anonymous pages to zram NOW (MADV_PAGEOUT over every
// private writable anonymous mapping) instead of leaving them for the kernel to swap
// out one reclaim pass at a time while the launching app is already short of memory.
// Everything faults back on demand (lz4 zram) when the overlay next draws. Best effort.
void NanoMenu::overlayPageOutSelf() {
    mallopt(M_PURGE_ALL, 0);   // return freed heap to the kernel first (bionic has no malloc_trim)
    FILE* f = fopen("/proc/self/maps", "re");
    if (!f) return;
    char line[512];
    size_t total = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0; char perms[8] = {0}; unsigned long off = 0; char dev[16] = {0}; unsigned long ino = 0;
        int n = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %15s %lu %n", &lo, &hi, perms, &off, dev, &ino, &n) < 6) continue;
        if (perms[0] != 'r' || perms[1] != 'w' || perms[3] != 'p') continue;   // private writable only
        const char* path = line + n;
        if (ino != 0) continue;                                              // anonymous only (no file backing)
        if (strstr(path, "[stack") || strstr(path, "[vvar]") || strstr(path, "[vdso]")) continue;
        if (hi - lo < 64 * 1024) continue;                                    // skip tiny mappings
        if (madvise((void*)lo, hi - lo, MADV_PAGEOUT) == 0) total += hi - lo;
    }
    fclose(f);
    ALOGI("overlay: paged out %zu MB of own anonymous memory", total >> 20);
}

void NanoMenu::overlayHide() {
    if (!mOverlayShown) return;
    esdeGamelistFlush();   // parked, it does not tick: write the edits now
    // Hidden from outside (a power-hold, a kill) while an app's keyboard was up: answer it now, or
    // the bridge would wait for a fade that never renders and take no more requests.
    if (mOskOverApp) {
        const std::string id = mOskAppReqId;
        const std::string dir = mOskAppDir;
        if (mOsk.im) { mOsk.im->reset(); mOsk.candidates.clear(); }
        mOsk.miniOpen = false;
        mOskActive = false; mOsk.closing = false; mOsk.anim = 0.0f;
        mOskPasswordMode = false; mOskPlaintext = false;
        mOskPasswordPrompt.clear(); mOskPasswordCallback = nullptr;
        mOskQuery.clear(); mOsk.caret = 0;
        mOskOverApp = false;
        mOskAppReqId.clear();
        property_set("sys.gammaos.nano.osk_gen", "");
        if (!dir.empty()) unlink((dir + "/nano_osk_live.txt").c_str());
        property_set("sys.gammaos.nano.osk_done", ("cancel:" + id).c_str());
    }
    esdeBgVideoStop();   // no theme or game video (or its sound) over the app
    mAnbernicArmed = mAnbernicHeld = false;
    mAppsSummoned = false;
    bmapClose(false);
    mOverlayXmbDeferred = false;
    mSsHeld[0] = mSsHeld[1] = false;
    mBackHeld = false;
    mKeyReplay.clear();
    navRelease(NavDir::None);   // its release may be among the events dropped while hidden

    // Drop any input up to NOW before restoring the app's input - notably the BACK
    // press that dismissed the overlay. The overlay reads BACK via evdev and clears
    // drop_input here, so without this the just-pressed BACK could still be sitting
    // in InputDispatcher's queue and get delivered to the app once drop_input=0
    // (the "BACK bleeds into the app after dismiss" bug). The timestamp fence drops
    // any event with eventTime <= now (checked independently of drop_input); newer
    // events still reach the app. Same mechanism the home launch handoff uses.
    {
        int64_t fenceNs = uptimeMillis() * 1000000LL;
        char fb[32];
        snprintf(fb, sizeof(fb), "%lld", (long long)fenceNs);
        property_set("sys.gammaos.nano.drop_fence_ns", fb);
    }
    // Restore the app's input (the framework re-dispatches keys+motion to it).
    property_set("sys.gammaos.nano.drop_input", "0");
    // Release FIXED_PERFORMANCE so a launched app gets normal vendor DVFS.
    property_set("sys.gammaos.nano.fixedperf", "0");
    mOverlayPausedPkg.clear();

    // Drop back to normal scheduling so the resident-hidden overlay does not hold
    // a real-time priority while the app runs unobstructed.
    {
        struct sched_param sp = {};
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
    }

    if (mFlingerSurfaceControl != nullptr) {
        SurfaceComposerClient::Transaction t;
        t.hide(mFlingerSurfaceControl);
        mMonitorLayerShown = false;
        // Also hide the secondary (bottom) surface, atomically with the primary, so the DSi
        // in-game overlay's opaque RGBX bottom panel never covers the resumed game's bottom
        // screen. The render-loop toggle also hides it (show_overlay=0), this is the belt.
        for (const auto& sc : mSecondaryWallpaperControls) t.hide(sc);
        t.apply();
        mNdsSecondaryShown = false;   // re-show on the next summon
        mTopRingShown = false;        // this hide also covers any focus-ring layer state
    }
    mOverlayShown = false;
    mOverlayPendingShow = false;   // cancel any deferred show (hidden before 1st frame)
    mOverlayWallpaper = false;     // next in-game summon starts in scrim mode
    // Clear any PSP-clock standalone summon so a stale prop cannot re-trigger the
    // standalone path on an unrelated later raise (e.g. a power-hold quick-menu).
    property_set("sys.gammaos.nano.pspclock_summon", "0");
    mPspClockStandalone = false;
    mPspClockRaisedOverlay = false;
    // Clear the overlay-foreground flag here so the tail orientationTick() below always
    // resolves the foreground APP's token (its per-app override or its own request),
    // instead of relying on every caller to have cleared it first. All current dismiss
    // callers already clear it, so this is a no-op today but prevents a future caller
    // from leaving the app clamped to nano's orientation after dismiss.
    property_set("sys.gammaos.nano.show_overlay", "0");
    // Hand orientation back to the foreground app (its per-app override or its own
    // request) the instant the overlay is dismissed.
    orientationTick();
    ALOGI("overlay: hidden (drop_input=0)");
}

bool NanoMenu::overlayAtTopLevel() const {
    // Back dismisses the overlay (resumes the running app) when it is at a bare top-level
    // XMB category with no submenu/modal open. The sub-screen states (Settings/WiFi/BT) are
    // already diverted by handleBack() before ps3XmbBack(), and the setup wizard is guarded
    // by mPs3WizActive, so the only menu states that reach here are the top-level category
    // states MAIN / RECENT / APPS - all of which must dismiss on Back (categories are moved
    // through with Left/Right, not Back). Requiring exactly MENU_MAIN was the dismiss bug: a
    // stale/navigated MENU_APPS left Back doing nothing over a live game
    // (overlay-back-diag showed menuState=2 stack=0 atTop=0).
    return mOverlayMode && mPs3Stack.empty() && !mPs3DlgActive &&
           !mPs3WizActive && !mPs3TzActive &&
           (mMenuState == MENU_MAIN || mMenuState == MENU_RECENT || mMenuState == MENU_APPS);
}

void NanoMenu::overlayResume() {
    stopEqPreview();   // never leave the GammaEQ preview clip playing into a resumed app
    // In overlay-home LAUNCHER mode (full wallpaper, no app behind us) there is
    // nothing to return to - the overlay IS the home surface - so Back/Resume at
    // the top level must be a no-op. Dismissing would orphan the screen and the
    // framework would immediately re-raise it, a hide/show flicker loop. Only the
    // in-game overlay (scrim over a running app) dismisses on Back.
    if (mOverlayWallpaper &&
        property_get_bool("persist.gammaos.nano.overlay_home", false)) {
        ALOGI("overlay: resume ignored (launcher mode, nothing to return to)");
        return;
    }
    // Back / power at the XMB top level: dismiss the overlay and let the running
    // app take input again.
    property_set("sys.gammaos.nano.show_overlay", "0");
    overlayHide();
    ALOGI("overlay: resume -> dismissed, app resumed");
}

// Layer opacity + swap pacing for the current wallpaper/scrim mode.
// In WALLPAPER mode (no app behind us) the overlay fully covers the screen with
// an opaque wave, so mark the SF layer OPAQUE (eLayerOpaque): SurfaceFlinger can
// then occlusion-cull everything beneath and scan out on a hardware plane where
// possible, instead of GPU-compositing a translucent full-screen layer every
// frame. In SCRIM mode the layer MUST stay translucent so the live app shows
// through the 90% scrim. Prop-gated (default on) so it can be A/B'd live.
//
// Swap pacing: WALLPAPER mode uses vsync-blocked swaps (interval 1), exactly
// like the cold-boot home. Measured on the Brick: the home presents 59.8fps
// (SF timestats, 0 dropped) while a free-running interval-0 + wall-clock-sleep
// overlay presented only 51.8fps at the same ~59fps render-loop rate - the
// submission phase drifts across SurfaceFlinger's latch point and ~8 frames/sec
// get replaced in the queue, which the eye reads as constant judder even though
// the loop counter says 60. SCRIM mode keeps interval 0 + the threadLoop top-up
// sleep; blocking on vsync there would contend with the game's own pipeline and
// that mode is verified smooth as-is.
// Must run on the render thread: eglSwapInterval applies to the surface current
// on the calling thread.
void NanoMenu::overlayApplyPresentMode() {
    if (mFlingerSurfaceControl != nullptr) {
        bool opaque = mOverlayWallpaper &&
            property_get_bool("persist.gammaos.nano.overlay.opaque_wallpaper", true);
        SurfaceComposerClient::Transaction()
            .setFlags(mFlingerSurfaceControl,
                      opaque ? layer_state_t::eLayerOpaque : 0u,
                      layer_state_t::eLayerOpaque)
            // Belt-and-braces: make sure no background blur is requested on
            // this layer (nano never sets one, but blur with nothing behind
            // the launcher wallpaper would be pure SF GPU waste).
            .setBackgroundBlurRadius(mFlingerSurfaceControl, 0)
            .apply();
        ALOGI("overlay: layer opaque=%d (wallpaper=%d)",
              opaque ? 1 : 0, mOverlayWallpaper ? 1 : 0);
    }
    if (mDisplay != EGL_NO_DISPLAY)
        eglSwapInterval(mDisplay, mOverlayWallpaper ? 1 : 0);

    // Universal perf hint: request IPower FIXED_PERFORMANCE while the overlay
    // is up, in BOTH modes (init turns the prop into `cmd power
    // set-fixed-performance-mode-enabled`). A wallpaper-only-unpinned variant
    // was A/B'd on the Brick and the apparent win did not reproduce across
    // runs (the governor can sag under the steady wave load); the pinned
    // sustained level matches the build that measured 60.1fps presented in the
    // post-game launcher. Released in overlayHide.
    if (property_get_bool("persist.gammaos.nano.perf.fixedperf", true))
        property_set("sys.gammaos.nano.fixedperf", "1");
}

void NanoMenu::overlayQuitToHome(bool keepApp) {
    // Quit the running app. The app is being killed so we must NOT thaw-then-resume
    // it: clear the paused state + marker first so overlayHide's thaw is a no-op.
    std::string pkg = mOverlayPausedPkg;
    mOverlayPausedPkg.clear();
    unlink(kOverlayFrozenMarker);
    bool isGame = !pkg.empty() &&
        (pkg.find("retroarch") != std::string::npos ||
         pkg.find("drastic") != std::string::npos);
    // Per-app "Keep Running in Background": leave the app alive when exiting to the menu so it can
    // resume warm. We still return to the launcher (raise the overlay wallpaper / hide to the DRM
    // home), we just skip the ESC/force-stop below. Explicit Kill All / Kill Background still stop it.
    bool keepAlive = !pkg.empty() && (keepApp || backgroundHas(pkg));

    if (property_get_bool("persist.gammaos.nano.overlay_home", false)) {
        // Overlay-home: quit == return to the overlay launcher. Keep the overlay
        // shown and switch it to the opaque full-wallpaper XMB; clean-exit the app
        // behind it (ESC save-state for RetroArch/DraStic, force-stop otherwise).
        // app_launched=0 + show_overlay=1 keeps the RWC overlay-launcher
        // short-circuit active so the real launcher never appears.
        mOverlayWallpaper = true;
        // The mode flips WITHOUT a hide+show cycle, so re-apply the wallpaper
        // presentation state here (opaque layer + vsync-locked swaps). Without
        // this the launcher kept the scrim's translucent layer + free-running
        // swap interval: SF could not occlusion-cull the dead app's layer and
        // the wave juddered (~34fps presented at a 59fps render loop).
        overlayApplyPresentMode();
        property_set("sys.gammaos.nano.app_launched", "0");
        esdeReturnHome();
        if (keepAlive) {
            ALOGI("overlay: quit %s -> overlay launcher (kept alive in background)", pkg.c_str());
        } else if (!pkg.empty()) {
            std::string p = pkg;
            std::thread([p, isGame]() {
                if (isGame) {
                    property_set("sys.gammaos.nano.qr_send_esc", "1");
                    for (int i = 0; i < 30 && overlaySignalPackage(p.c_str(), 0) > 0; i++)
                        usleep(100000);
                }
                // Force-stop if still alive: either a non-game app, or a game
                // that ignored the injected ESC (observed: RetroArch keeps
                // rendering at ~33fps behind the opaque launcher forever,
                // burning GPU/CPU the cold-boot menu never pays). The 3s ESC
                // grace above still gives RetroArch/DraStic their clean
                // save-state exit when they do honor it.
                if (!isGame && overlayHasForegroundService(p)) {
                    ALOGI("overlay: quit %s -> overlay launcher (foreground service running, kept alive)", p.c_str());
                    return;
                }
                if (overlaySignalPackage(p.c_str(), 0) > 0) {
                    char c[320];
                    snprintf(c, sizeof(c), "am force-stop %s 2>/dev/null",
                             overlayShq(p).c_str());
                    system(c);
                    ALOGI("overlay: quit %s ignored ESC, force-stopped", p.c_str());
                }
                ALOGI("overlay: quit %s -> overlay launcher", p.c_str());
            }).detach();
        }
        return;   // stay shown as the launcher (drop_input stays 1)
    }

    // Non-overlay-home: force-stop and let the DRM home XMB take the display back. An app running a
    // foreground service (a server, a download, music) is kept alive like a background app.
    if (!keepAlive && !isGame && overlayHasForegroundService(pkg)) {
        keepAlive = true;
        ALOGI("overlay: quit %s with a foreground service running, keeping it alive", pkg.c_str());
    }
    if (!pkg.empty() && !keepAlive) {
        char cmd[320];
        snprintf(cmd, sizeof(cmd), "am force-stop %s 2>/dev/null",
                 overlayShq(pkg).c_str());
        system(cmd);
        ALOGI("overlay: quit -> force-stopped %s, returning to home XMB", pkg.c_str());
    }
    property_set("sys.gammaos.nano.show_overlay", "0");
    if (keepAlive) {
        // The app is left alive, so the framework's app-death-triggered home restart never fires.
        // Trigger the DRM home restart ourselves and clear the launch state (mirroring the
        // RootWindowContainer cleanup) so nano retakes the display over the still-alive app instead
        // of overlayHide simply revealing it again. app_launched=0 stops the restart from relaunching
        // it via the LAUNCHER fallback.
        property_set("sys.gammaos.nano.app_launched", "0");
        commitLaunch(LaunchTuple()
                .app("")
                .intent("")
                .core(""));
        property_set("sys.gammaos.nano.restart", "1");
        ALOGI("overlay: quit -> %s kept alive in background, restarting home XMB", pkg.c_str());
    }
    overlayHide();
}

// Single-quote a string for safe interpolation into a /bin/sh command line.
// Handles embedded apostrophes (ROM names like "Marvel's ...") via '\'' splicing.
static std::string overlayShq(const std::string& s) {
    return nanolaunch::shellQuote(s);
}

// True while the package runs a foreground service: an FTP or SMB server (MiXplorer), a
// download, music playback. Such an app is working in the background on purpose, so quitting
// it to the menu leaves it running like a "Keep Running in Background" app instead of
// force-stopping it (which killed MiXplorer's FTP server the moment the user went back home, so
// every connection from the PC was refused). Games keep their save-and-quit path. Takes a few
// hundred milliseconds (dumpsys), so callers use it on quit only.
static bool overlayHasForegroundService(const std::string& pkg) {
    if (pkg.empty()) return false;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "dumpsys activity services %s 2>/dev/null", overlayShq(pkg).c_str());
    FILE* f = popen(cmd, "r");
    if (!f) return false;
    bool fg = false;
    char line[512];
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "isForeground=true")) fg = true;   // keep reading so dumpsys never sees a broken pipe
    pclose(f);
    return fg;
}

// Run a shell command and return its trimmed stdout (first use: pm path / settings).
static std::string overlayShellCapture(const char* cmd) {
    std::string out;
    FILE* f = popen(cmd, "r");
    if (!f) return out;
    char b[512];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
    pclose(f);
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// Quick Menu: replicate the GammaOS legacy "Kill Background Apps" / "Kill All
// Apps" global actions. `pm list packages -3` enumerates only third-party
// (non-system) packages, mirroring the legacy action's FLAG_SYSTEM skip;
// force-stopping a package that is not running is a harmless no-op, so the
// observable result (every third-party app stopped) matches the legacy
// ActivityManager.forceStopPackage sweep over running processes. The Kill
// Background variant skips the current foreground package so the running game
// keeps going. Runs on a detached thread because the sweep shells out per
// package and must not stall the render loop.
void NanoMenu::quickKillApps(bool includeForeground) {
    std::string fg = includeForeground ? std::string() : overlayResolveForegroundPkg();
    std::thread([includeForeground, fg]() {
        std::string list = overlayShellCapture("pm list packages -3 2>/dev/null");
        size_t pos = 0;
        int killed = 0;
        while (pos < list.size()) {
            size_t nl = list.find('\n', pos);
            std::string line = list.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? list.size() : nl + 1;
            const char* pfx = "package:";
            size_t p = line.find(pfx);
            std::string pkg = (p == std::string::npos) ? line : line.substr(p + strlen(pfx));
            while (!pkg.empty() && (pkg.back() == '\r' || pkg.back() == '\n' || pkg.back() == ' '))
                pkg.pop_back();
            if (pkg.empty() || pkg.find('.') == std::string::npos) continue;
            if (pkg.find("gammaos") != std::string::npos) continue;          // never ourselves
            if (!includeForeground && !fg.empty() && pkg == fg) continue;    // keep the running game
            char c[320];
            snprintf(c, sizeof(c), "am force-stop %s 2>/dev/null", overlayShq(pkg).c_str());
            system(c);
            killed++;
        }
        ALOGI("nano: quickKillApps(includeForeground=%d) force-stopped %d third-party packages",
              includeForeground, killed);
    }).detach();
}

// Quick Menu -> Kill All Apps (in-game overlay). Hard-stop EVERY third-party app, the
// foreground game included, and drop back to the home launcher. Simply force-stopping
// the running game makes RootWindowContainer relaunch it (its launch state is still
// live -- the user "can still hear the game after Kill All"), so clear the relaunch
// triggers and become the wallpaper launcher FIRST -- exactly as overlayQuitToHome
// does for a clean quit -- then force-stop everything. NOTE: no pending_exit here; in
// overlay-home mode that can spawn a DRM-home nano that fights the SF overlay launcher.
void NanoMenu::overlayKillAll() {
    mOverlayWallpaper = true;
    overlayApplyPresentMode();   // mode flip without hide+show: opaque + vsync lock
    property_set("sys.gammaos.nano.app_launched", "0");
    commitLaunch(LaunchTuple().app(""));
    property_set("sys.gammaos.nano.return_apps", "0");
    property_set("persist.gammaos.nano.qr_prepared", "0");
    property_set("persist.gammaos.nano.qr_core", "");
    mOverlayPausedPkg.clear();
    unlink(kOverlayFrozenMarker);
    esdeReturnHome();
    quickKillApps(true);   // force-stop ALL incl the game; app_launched=0 blocks the relaunch
}

// Close the app the overlay is sitting over before another launch takes the panel.
// A game (RetroArch / DraStic) is asked to quit itself through ESC and waited for,
// so it saves its state; force-stop is the last resort for a hung one. A plain app is
// force-stopped unless it is a "Keep Running in Background" app. A game is too (as DraStic
// can be without the DS player), unless NEXT, the package launched after it, is the same
// one. Blocks up to ~10 s, so callers run it off the render thread. The caller has already
// set killing=1.
void NanoMenu::overlayCloseRunningApp(const std::string& old, const std::string& next) {
    bool oldIsGame = !old.empty() && (old == next || !backgroundHas(old)) &&
        (old.find("retroarch") != std::string::npos ||
         old.find("drastic") != std::string::npos);
    if (oldIsGame) {
        // CLEAN SELF-CLOSE (no force-stop): capture the game's pids, send ESC
        // (RetroArch / DraStic save state then quit themselves), and WAIT for
        // those pids to ACTUALLY die via kill(pid,0). dumpsys drops the
        // ProcessRecord seconds before the process exits, so polling dumpsys was
        // premature and needed a force-stop to stop the relaunch racing the husk
        // - but a force-stop is a hard kill the user does not want. kill(pid,0)
        // on the captured pids is reliable, so the old game closes itself
        // cleanly (saving state) and the new ROM launches only once it is truly
        // gone. The death lands under killing=1 so the AMS overlay hook ignores it.
        std::set<int> oldPids = overlayGetPids(old.c_str());
        // Restore the game's input, then ask PhoneWindowManager to send ESCAPE
        // (overlay_esc -> triggerVirtualKeypress, the SAME path the back-long-
        // press uses; a shell-injected ESC is ignored by RetroArch). The game
        // must be the focused foreground window for this to land, which it is
        // (the overlay is an SF layer, not a focusable window).
        property_set("sys.gammaos.nano.drop_input", "0");
        property_set("sys.gammaos.nano.overlay_esc", "1");
        if (!oldPids.empty()) {
            bool alive = true;
            for (int i = 0; i < 100 && alive; i++) {   // up to ~10s for save+quit
                usleep(100000);
                // Re-send ESC a couple more times early, in case the first was
                // dropped while focus settled after drop_input cleared.
                if (i == 15 || i == 35)
                    property_set("sys.gammaos.nano.overlay_esc", "1");
                alive = overlayAnyAlive(oldPids);
            }
            if (alive) {
                // Last resort ONLY (it never exited - a hung save): force-stop so
                // the relaunch does not race a stuck instance.
                char c[320];
                snprintf(c, sizeof(c), "am force-stop %s 2>/dev/null",
                         overlayShq(old).c_str());
                system(c);
                usleep(400000);
                ALOGW("overlay: %s did not self-exit ~10s after ESC, force-stopped",
                      old.c_str());
            } else {
                ALOGI("overlay: %s self-exited cleanly (saved state) before launch",
                      old.c_str());
            }
        } else {
            usleep(2500000);   // pids unknown: give the ESC time to save and quit
            ALOGI("overlay: ESC-exited %s (pids unknown, fixed wait)", old.c_str());
        }
    } else if (!old.empty() && !backgroundHas(old) && !overlayHasForegroundService(old)) {
        // Do NOT force-stop a "Keep Running in Background" app when switching away from it -
        // it must stay alive so re-selecting it later resumes warm. Same for an app running a
        // foreground service (see overlayHasForegroundService).
        char c[320];
        snprintf(c, sizeof(c), "am force-stop %s 2>/dev/null",
                 overlayShq(old).c_str());
        system(c);
    }
}

void NanoMenu::overlayLaunchCommand(const std::string& pkg, const std::string& amCmd) {
    // A BIOS install runs, or a card is adopted: emulators are being written.
    if (biosWorkerBusy() || emuSdAdopting() || emuMoveRunning()) { showLaunchBusyToast(); return; }
    // NOTE: no "pkg == mOverlayPausedPkg -> resume" shortcut here. A single
    // emulator package (com.retroarch.aarch64) hosts MANY games, so selecting a
    // different ROM of the running emulator MUST relaunch with the new ROM, not
    // resume the old game. The same-app resume shortcut lives in
    // overlayLaunchPackage (plain apps only); selecting a game always (re)launches.

    std::string old = mOverlayPausedPkg;
    mOverlayPausedPkg.clear();
    unlink(kOverlayFrozenMarker);

    // Hold the overlay up through the transition; overlayPoll() dismisses onto the
    // new app once it resumes (so the user never sees the dying app or a black
    // frame). Armed synchronously before the worker thread starts.
    mOverlayLaunchPending = true;
    mLaunchHandOffMs = elapsedRealtime();
    mOverlayLaunchTarget = pkg;
    mOverlayLaunchStartMs = uptimeMillis();
    mOverlayLaunchLastCheckMs = 0;

    // Dual screen: launches go to the monitor (display 0), whichever display has the controller; an
    // app already on the panel moves there.
    const bool dualMonitor = mDualPanel;

    // Do the (possibly slow) clean exit + launch off the render thread so the XMB
    // keeps animating during the handoff.
    std::thread([this, old, pkg, amCmd, dualMonitor]() {
        // Guard the whole exit+launch transition: RootWindowContainer skips ALL of
        // its startHome handling while killing=1, so force-stopping the old app
        // cannot make it falsely detect the (not-yet-registered) new app as
        // "exited" and tear it down. Same guard the home-mode nanoKillAppAndRestart
        // uses. Cleared only once the new app is actually the resumed activity.
        property_set("sys.gammaos.nano.killing", "1");

        overlayCloseRunningApp(old, pkg);
        emuSettingsSyncLaunch(pkg);   // its settings as the card has them
        // Track for the framework: RootWindowContainer raises the overlay launcher
        // when this app exits, and PhoneWindowManager's back-long-press exit fires
        // (both gated on app_launched=1).
        if (!pkg.empty())
            commitLaunch(LaunchTuple().app(pkg.c_str()));
        property_set("sys.gammaos.nano.app_launched", "1");
        // GammaOS Nano: arm the boot-anim hand-off signal that the DRM-home launch path also sets
        // (NanoMenuInput.cpp launchXmbGame / MENU_RECENT). On minimal_boot, WindowManagerService.
        // performEnableScreen ENABLES framework input dispatch only on its "full" branch, which is
        // gated on service.bootanim.nano_retroarch==1; the preload branch (nano home) leaves dispatch
        // OFF (nano reads evdev directly). This resident-overlay launch path never set nano_retroarch,
        // so a framework app launched through the overlay could stay in the preload branch with input
        // dispatch globally disabled - dead controls. This bit RetroArch launched right after a native
        // drastic-nano session (drastic is not a framework activity, so it never opened the gate, and
        // the overlay launch that followed did not either). Setting it here makes the overlay launch
        // match the DRM-home launch. Quick-Resume / background preloads use a separate path (not this
        // worker), so they are unaffected and the nano home stays in the preload branch.
        commitLaunch(LaunchTuple().retroarch("1"));
        // Free the kernel's reclaimable caches for the launching app while the launch
        // effect is still playing (this is the worker thread, off the render loop).
        nanoDropReclaimableCaches();
        // GammaOS dual-screen: once the resident overlay home is up (app_launched=1),
        // app/game relaunches funnel through here instead of the framework home path,
        // which has no display target - so without this they default to display 0 (the
        // bottom, where the overlay home + Control Center live) and the TOP panel stays
        // blank. Pin the launch to the top panel like the first launch does. The Control
        // Center path sets its own --display (bottom for a secondary app), which we
        // respect; commands that are not `am start` (rare) are left untouched.
        {
            int primaryPort = property_get_int32("persist.gammaos.nano.primary_display", 0);
            int td = property_get_int32("persist.gammaos.nano.cc.topdisplay", 2);
            std::string finalCmd = amCmd;
            size_t sp = finalCmd.find("am start");
            // Do NOT pin a dual-stack app to the top panel: it must launch on the
            // default display (0), where DualStackController forces the tall 640x960
            // canvas and mirrors it across both panels. Only ordinary apps get pinned
            // to the top. (A caller that already set --display is respected either way.)
            // Only inject --display on multi-screen devices (primary_display > 0, i.e.
            // a secondary port holds the XMB). Single-screen devices have only display 0
            // and would black-screen if directed to a non-existent display 2.
            // Also skip the top-pin for a dual-SCREEN "run on primary" app (e.g. cocoonshell): it
            // must launch on the default display (0/bottom) so its main lands there and it can open
            // its own second activity on the top panel, matching the cold-launch routing.
            if (sp != std::string::npos && finalCmd.find("--display") == std::string::npos
                    && !dualstackHas(pkg) && !primaryScreenHas(pkg) && primaryPort > 0)
                finalCmd.insert(sp + 8, " --display " + std::to_string(td));
            else if (sp != std::string::npos && finalCmd.find("--display") == std::string::npos && dualMonitor)
                finalCmd.insert(sp + 8, " --display 0");
            system(finalCmd.c_str());
        }
        ALOGI("overlay: launched %s", pkg.c_str());

        // Release the guard only once the new app is the resumed activity (or a
        // timeout), so RootWindowContainer never sees the gap between the old app
        // dying and the new one registering.
        for (int i = 0; i < 40; i++) {
            usleep(100000);
            if (pkg.empty() || overlayResolveForegroundPkg() == pkg) break;
        }
        property_set("sys.gammaos.nano.killing", "0");
    }).detach();
}

bool NanoMenu::overlayLaunchPackage(const std::string& pkg) {
    if (pkg.empty()) return false;
    // A BIOS install runs: no launch (bringing back the app under the scrim is not one).
    if (biosWorkerBusy() && !(pkg == mOverlayPausedPkg && !mOverlayWallpaper)) { showLaunchBusyToast(); return true; }
    // Selecting the app that is ALREADY running = just resume it (one app per
    // package, unlike emulators). Games never take this path (see overlayLaunchGame).
    if (pkg == mOverlayPausedPkg) {
        // Scrim-over-app: dismiss the scrim to return to the running app.
        if (!mOverlayWallpaper) { overlayResume(); return true; }
        // Launcher mode (the overlay IS the home): overlayResume() is a deliberate no-op, so a
        // "Keep Running in Background" app that was exited to the menu would get stuck here. Its
        // process is still alive, so fall through and am-start its launcher activity - that brings the
        // existing task to the front WARM. Clear mOverlayPausedPkg first so overlayLaunchCommand does
        // NOT force-stop the very process we kept alive (which would cold-restart it).
        mOverlayPausedPkg.clear();
    } else {
        // Another app replaces the game: the button map must not take the game's system and ROM
        // for it (an emulator opened on its own menu), as the home's launchInstalledApp does.
        commitLaunch(LaunchTuple()
                .romPath("")
                .core(""));
    }
    // Plain app (Applications submenu): start its LAUNCHER activity. Resolve the
    // component and start it with `am start` rather than `monkey` - monkey cannot take
    // a --display flag, which overlayLaunchCommand needs to pin the app to the top panel
    // (otherwise a relaunch from the overlay home lands on the bottom display and the top
    // stays blank). Mirrors the Control Center's resolve-then-am-start idiom.
    std::string cmd =
        "ACT=$(cmd package resolve-activity --brief -a android.intent.action.MAIN "
        "-c android.intent.category.LAUNCHER " + overlayShq(pkg)
        + " 2>/dev/null | tail -1); "
        "case \"$ACT\" in */*) am start -n \"$ACT\" 2>/dev/null;; esac";
    overlayLaunchCommand(pkg, cmd);
    return true;
}

// In-game (overlay) URL launch: start the chosen browser directly ON the URL with an
// explicit ACTION_VIEW intent, instead of the plain LAUNCHER start overlayLaunchPackage
// does (which would drop the URL and open the browser on its home page). Works for any
// browser.
bool NanoMenu::overlayLaunchUrl(const std::string& pkg, const std::string& comp,
                                const std::string& url) {
    if (pkg.empty() || url.empty()) return false;
    if (biosWorkerBusy()) { showLaunchBusyToast(); return true; }   // a BIOS install runs
    std::string cmd = "am start -a android.intent.action.VIEW -d " + overlayShq(url);
    if (!comp.empty()) cmd += " -n " + overlayShq(comp);
    else               cmd += " " + overlayShq(pkg);
    cmd += " 2>/dev/null";
    overlayLaunchCommand(pkg, cmd);
    return true;
}

// OSK-over-app bridge. An app that needs text entry (e.g. WebView web fields)
// cannot rely on the framework leanback IME here: on this 1GB low-ram device it is
// OOM-killed the instant it cold-starts under a heavy WebView. Instead the app asks
// nano to host its own lightweight OSK over the live app:
//   app  writes the current field text to <filesDir>/nano_osk_in.txt
//   app  setprop sys.gammaos.nano.osk_dir  <filesDir>
//   app  setprop sys.gammaos.nano.osk_type text|password
//   app  setprop sys.gammaos.nano.osk_hint <label>  (optional: shown above the field)
//   app  setprop sys.gammaos.nano.osk_req  <id>     (triggers us)
//   app  setprop sys.gammaos.nano.osk_abort <id>    (its field went away: close, answer cancel)
//   we   raise the overlay (drop_input=1) in OSK-only mode, prefilled
//   commit -> write <filesDir>/nano_osk_out.txt, setprop osk_done ok:<id>, dismiss
//   cancel -> setprop osk_done cancel:<id>, dismiss
// Text travels through a file (props cap at 91 bytes; URLs/fields exceed that).
// Live typing: stream the current OSK buffer to the requesting app so the field
// fills as the user types, instead of only on commit. Writes the plain buffer to
// <dir>/nano_osk_live.txt (atomic temp+rename so the reader never sees a partial
// file) and bumps sys.gammaos.nano.osk_gen=<id>:<n>, which the app polls. Only runs
// during an app-hosted OSK session.
void NanoMenu::oskPublishLive() {
    if (!mOskOverApp || mOskAppDir.empty()) return;
    std::string p = mOskAppDir + "/nano_osk_live.txt";
    std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    if (!mOskQuery.empty()) fwrite(mOskQuery.data(), 1, mOskQuery.size(), f);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);          // buffer must be on disk before the gen bump
    fclose(f);
    chmod(tmp.c_str(), 0644);
    if (rename(tmp.c_str(), p.c_str()) != 0) { unlink(tmp.c_str()); return; }
    char v[80];
    snprintf(v, sizeof v, "%s:%u", mOskAppReqId.c_str(), ++mOskAppGen);
    property_set("sys.gammaos.nano.osk_gen", v);
}

// Runs "ime enable <id>; ime set <id>" off the calling thread and logs the result.
// Android can be a while listing a keyboard on the first boot (setup finished before it knew the
// Nano keyboard), so one it calls unknown is tried again for half a minute.
static void selectIme(const std::string& id) {
    std::thread([id] {
        const std::string cmd = "ime enable " + overlayShq(id) + " 2>&1; ime set " + overlayShq(id) + " 2>&1";
        for (int attempt = 0; attempt < 15; attempt++) {
            if (attempt) sleep(2);
            std::string out;
            if (FILE* f = popen(cmd.c_str(), "r")) {
                char buf[256];
                while (fgets(buf, sizeof(buf), f) && out.size() < 1024) out += buf;
                pclose(f);
            }
            for (char& c : out) if (c == '\n') c = ' ';
            ALOGI("keyboard %s: %s", id.c_str(), out.c_str());
            if (out.find("Unknown input method") == std::string::npos) return;
        }
    }).detach();
}

// With the Nano keyboard off (persist.gammaos.nano.ime), the build's own keyboard
// (persist.gammaos.default_ime, an input method id); with neither, Android's default.
void NanoMenu::selectDefaultIme() {
    if (property_get_bool("persist.gammaos.nano.ime", true)) { selectNanoIme(); return; }
    char id[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.default_ime", id, "");
    if (id[0]) selectIme(id);
}

void NanoMenu::selectNanoIme() {
    if (!property_get_bool("persist.gammaos.nano.ime", true)) return;
    selectIme(NANO_IME);
}

void NanoMenu::overlayOskPoll() {
    if (mOskOverApp) {
        char abort[PROPERTY_VALUE_MAX] = {};
        if (mOskActive && !mOsk.closing && property_get("sys.gammaos.nano.osk_abort", abort, "") > 0 &&
            mOskAppReqId == abort) {
            property_set("sys.gammaos.nano.osk_abort", "");
            if (mOsk.miniOpen) oskClosePopup();
            closeOsk();   // answered as a cancel once it has faded out, below
        }
        // A commit clears mOskOverApp inside its callback below; reaching here with
        // it still set once the OSK has fully closed means the user backed out.
        if (!mOskActive && !mOsk.closing) {
            std::string id = mOskAppReqId;
            std::string dir = mOskAppDir;
            mOskOverApp = false;
            mOskAppReqId.clear();
            property_set("sys.gammaos.nano.osk_gen", "");
            if (!dir.empty()) unlink((dir + "/nano_osk_live.txt").c_str());
            ALOGI("osk bridge: %s answered cancel", id.c_str());
            property_set("sys.gammaos.nano.osk_done", ("cancel:" + id).c_str());
            property_set("sys.gammaos.nano.show_overlay", "0");
            overlayHide();
        } else if (mOskQuery != mOskAppLastBuf) {
            mOskAppLastBuf = mOskQuery;   // stream each edit to the app for live typing
            oskPublishLive();
        }
        return;   // one OSK session at a time
    }
    char req[PROPERTY_VALUE_MAX] = {};
    if (property_get("sys.gammaos.nano.osk_req", req, "") <= 0 || !req[0]) return;
    // Never mid app-launch, nor over a menu the user opened (it holds the app's input, and may have
    // its own keyboard up): the request waits until the overlay is down.
    if (mOverlayLaunchPending || mOverlayShown) {
        static std::string sLoggedWait;
        if (sLoggedWait != req) {
            sLoggedWait = req;
            ALOGI("osk bridge: %s waits (%s)", req, mOverlayLaunchPending ? "app launching" : "overlay shown");
        }
        return;
    }
    property_set("sys.gammaos.nano.osk_req", "");   // consume the edge

    char dir[PROPERTY_VALUE_MAX] = {}, type[PROPERTY_VALUE_MAX] = {}, hint[PROPERTY_VALUE_MAX] = {};
    property_get("sys.gammaos.nano.osk_dir", dir, "");
    property_get("sys.gammaos.nano.osk_type", type, "text");
    property_get("sys.gammaos.nano.osk_hint", hint, "");
    property_set("sys.gammaos.nano.osk_hint", "");   // a client that sets none gets no stale label

    // A request its client already withdrew is not opened, nor one from Nano Keyboard ("ime-" and
    // its uptime in ms) older than 2.5 s, which it stops waiting for after 3 s. Both are answered.
    {
        char abort[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.osk_abort", abort, "");
        bool drop = strcmp(abort, req) == 0;
        if (!drop && strncmp(req, "ime-", 4) == 0) {
            const long long at = strtoll(req + 4, nullptr, 10);
            const long long now = (long long)uptimeMillis();
            drop = at > 0 && now - at > 2500;
            if (drop) ALOGW("osk bridge: request %s is %lld ms old; not opened", req, now - at);
        }
        if (drop) {
            property_set("sys.gammaos.nano.osk_done", (std::string("cancel:") + req).c_str());
            return;
        }
    }
    if (!dir[0]) { ALOGW("osk bridge: %s has no osk_dir", req); return; }
    mOskAppDir = dir;
    mOskAppReqId = req;

    std::string pre;
    {
        std::string p = mOskAppDir + "/nano_osk_in.txt";
        FILE* f = fopen(p.c_str(), "rb");
        if (f) {
            char buf[8192]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) pre.append(buf, n);
            fclose(f);
        } else {
            ALOGW("osk bridge: can't read %s: %s", p.c_str(), strerror(errno));
        }
    }
    bool masked = (strcmp(type, "password") == 0);
    ALOGI("osk bridge: opening %s (%s, %zu bytes)", req, type, pre.size());

    mOskOverApp = true;
    if (!mOverlayShown) {
        property_set("sys.gammaos.nano.show_overlay", "1");
        overlayShow();
    }
    std::string id = mOskAppReqId;
    std::string outDir = mOskAppDir;
    openOskForPassword(hint, [this, id, outDir](const std::string& val) {
        std::string p = outDir + "/nano_osk_out.txt";
        FILE* f = fopen(p.c_str(), "wb");
        if (f) { fwrite(val.data(), 1, val.size(), f); fclose(f); chmod(p.c_str(), 0644); }
        else ALOGW("osk bridge: can't write %s: %s", p.c_str(), strerror(errno));
        ALOGI("osk bridge: %s answered ok", id.c_str());
        mOskOverApp = false;
        mOskAppReqId.clear();
        property_set("sys.gammaos.nano.osk_gen", "");
        unlink((outDir + "/nano_osk_live.txt").c_str());
        property_set("sys.gammaos.nano.osk_done", ("ok:" + id).c_str());
        property_set("sys.gammaos.nano.show_overlay", "0");
        overlayHide();
    });
    mOskPlaintext = !masked;            // show typed text for normal fields, mask passwords
    mOskQuery = pre;                    // prefill AFTER openOskForPassword (it clears the query)
    mOsk.caret = (int)mOskQuery.size();
    // Live-typing baseline: the prefill equals the field's current value, so seed the
    // change-gate with it (first publish fires on the first real edit) and reset the gen.
    mOskAppGen = 0;
    mOskAppLastBuf = mOskQuery;
    property_set("sys.gammaos.nano.osk_gen", "");
}

void NanoMenu::overlayPoll() {
    overlayOskPoll();
    // Deferred dismiss after launching another app from the overlay: hold the
    // overlay layer up (it occludes the dying old app / black) until the new app
    // is the resumed activity, or a safety timeout. Throttle the ActivityManager
    // query so it does not run every frame.
    if (mOverlayLaunchPending) {
        int64_t now = uptimeMillis();
        int64_t el = now - mOverlayLaunchStartMs;
        // The launch worker holds sys.gammaos.nano.killing=1 for the WHOLE
        // exit+launch transition and clears it ONLY after the new app is the resumed
        // activity. Do NOT test foreground while killing=1: when switching to a
        // different ROM of the SAME emulator package (RetroArch A -> RetroArch B),
        // the OLD game is still that package and foreground, so foreground==target
        // would fire instantly and dismiss onto the dying old game before the new
        // one loads (the "switching games does nothing" bug). Wait for killing=0.
        bool switchInProgress = property_get_bool("sys.gammaos.nano.killing", false);
        // Launch abandoned backstop: the worker has finished (killing cleared)
        // but the launched app is not running (app_launched=0). The worker sets
        // app_launched=1 unconditionally before the am-start and only the AMS
        // death hook clears it, so app_launched=0 here means the app crashed/
        // exited during or right after the handoff and the foreground will never
        // become the target. Without this, mOverlayLaunchPending - and the
        // input-drain it triggers in pollInput() - would ride all the way to the
        // 12s ceiling, freezing the overlay home. Release the hold and fall
        // through to normal show/hide reconciliation (show_overlay is still 1, so
        // the overlay stays up as the home). A running app keeps app_launched=1,
        // so this can never dismiss a slow-but-valid launch; the el>1500 guard
        // skips the brief startup window before the worker sets killing/app_launched.
        if (!switchInProgress && el > 1500 &&
            !property_get_bool("sys.gammaos.nano.app_launched", false)) {
            mOverlayLaunchPending = false;
            mOverlayLaunchTarget.clear();
            ALOGI("overlay: launch abandoned (app not running, %lldms) - "
                  "releasing input hold", (long long)el);
            // fall through to show/hide reconciliation below (do NOT return)
        } else {
            bool ready = false;
            if (!switchInProgress && el > 250 && (now - mOverlayLaunchLastCheckMs) > 300) {
                mOverlayLaunchLastCheckMs = now;
                ready = (overlayResolveForegroundPkg() == mOverlayLaunchTarget);
            }
            // Ceiling is generous (12s): a RetroArch/DraStic clean-exit can take ~3s to
            // save state and die, plus a settle, plus the new app's own resume, plus the
            // worker's post-launch confirm poll. The `ready` check (after killing clears)
            // dismisses the instant the new app is up; the ceiling is only a backstop.
            if (ready || el > 12000) {
                mOverlayLaunchPending = false;
                mOverlayLaunchTarget.clear();
                property_set("sys.gammaos.nano.show_overlay", "0");
                overlayHide();
                ALOGI("overlay: launch dismiss (resumed=%d, %lldms)",
                      ready ? 1 : 0, (long long)el);
            }
            return;   // keep the overlay shown during the launch transition
        }
    }

    // drastic-nano relaunch guard: during a Restart Game / settings relaunch the
    // old drastic sets sys.gammaos.nano.killing=1 for the session=0 gap before the
    // new own-layer session re-takes the panel (it clears killing + show_overlay
    // when it asserts session=1). In that window a freshly (re)started home or a
    // framework/AMS path can briefly set show_overlay=1; raising the XMB then just
    // flashes it over the reload for ~2s. Since this is the SOLE consumer that
    // actually shows the overlay, gate it here so NO setter can flash the overlay
    // mid-relaunch, whoever sets the prop. Not applied to a normal exit (killing is
    // only set on a relaunch), so the overlay still returns after a real quit.
    {
        char kb[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.killing", kb, "0");
        if (kb[0] == '1' && kb[1] == '\0') {
            if (mOverlayShown) overlayHide();
            return;
        }
    }

    char v[PROPERTY_VALUE_MAX] = {};
    property_get("sys.gammaos.nano.show_overlay", v, "0");
    bool want = (v[0] == '1' && v[1] == '\0');
    if (want && !mOverlayShown) {
        overlayShow();   // snaps to the Quick Menu by default in scrim-over-app mode
    } else if (want && property_get_int32("sys.gammaos.nano.qs_summon", 0) != 0) {
        // The Anbernic button pressed while the layer was still up (a retract holding it): open
        // the Quick Menu (or, for Anbernic + Start, the Applications view) on it rather than leave
        // the request for the next raise.
        const int summon = property_get_int32("sys.gammaos.nano.qs_summon", 0);
        property_set("sys.gammaos.nano.qs_summon", "0");
        // Drop the press itself, still queued on the input fds, so pollInput does not toggle
        // the Quick Menu shut again (overlayShow drains the same way).
        struct input_event ev;
        for (int fd : mInputFds) {
            if (fd < 0) continue;
            while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) ssReleaseTaken(ev);
            nanoabxy::forget(fd);
        }
        touchResync(-1, true);
        // The layer was only up for a standalone PSP clock retract: keep it up for the Quick Menu
        // instead of letting the retract's teardown lower it. A button map with nothing to map
        // leaves the retract to lower it.
        if (summon != 3 && mOverlayXmbDeferred) overlayPrepareXmb(0);
        if (summon == 3) {
            if (!mOverlayWallpaper && !bmapActive()) bmapOpenOverApp();
            if (bmapActive()) mPspClockRaisedOverlay = false;
        } else if (summon == 2) {
            mPspClockRaisedOverlay = false;
            if (!mOverlayWallpaper && !applicationsViewOpen()) {
                openApplicationsView();
                mAppsSummoned = applicationsViewOpen();
            }
        } else {
            mPspClockRaisedOverlay = false;
            if (!mOverlayWallpaper && !quickMenuOpen()) toggleQuickMenu();
        }
    } else if (!want && mOverlayShown) {
        // Do NOT lower the overlay while a standalone clock summon is still retracting:
        // let drawPspClock's teardown clear show_overlay once the clock reaches reveal 0,
        // so the retract (glyphs implode + disc rise) plays fully instead of the layer
        // snapping away mid-animation. (Belt-and-braces: on the normal path the framework
        // leaves show_overlay=1 through the retract, so want stays true and we never reach here.)
        if (!(mPspClockStandalone && mPspClockReveal > 0.0f)) {
            overlayHide();
        }
    }
    // No grab to reconcile: input isolation is the framework drop_input prop,
    // which overlayShow/overlayHide set/clear, and InputDispatcher self-clears a
    // stuck drop_input (plus the 10s BACK emergency) if this process ever dies
    // mid-show. The overlay never owns an evdev grab now.
}

void NanoMenu::overlayLaunchGame() {
    // A BIOS install runs, or a card is adopted or emulator files move: emulators are being written.
    if (biosWorkerBusy() || emuSdAdopting() || emuMoveRunning()) { showLaunchBusyToast(); return; }
    // Launch the currently selected XMB game/ROM from the overlay by building the
    // SAME activity intent the home-mode launchXmbGame() hands the framework, but
    // starting it directly with `am start` (no DRM-exit handshake, which must never
    // run in the resident overlay). Standalone emulators get the SAF content:// URI;
    // RetroArch gets the libretro extras (direct FUSE ROM path, no DE-cache shuffle
    // since the system is fully up). overlayLaunchCommand() handles the clean exit
    // of any running game first.
    std::string romPath, romDir, coreSo, launchPkg, launchIntent, launchArgs, romName;
    bool standalone;
    bool fromRecent = false;
    if (mXmbSystemIndex == -1) {                  // Recently Played
        if (mXmbGameIndex < 0 || mXmbGameIndex >= (int)mXmbRecent.size()) return;
        const XmbRecentEntry& re = mXmbRecent[mXmbGameIndex];
        romPath = re.romPath; romDir = re.romDir; coreSo = re.coreSo;
        launchPkg = re.launchPkg; launchIntent = re.launchIntent; standalone = re.standalone;
        romName = re.displayName; fromRecent = true;
        // The system's extra launch args, while the entry still uses that system's emulator.
        const int si = recentSystemIndex(re);
        if (si >= 0 && mXmbSystems[si].launchIntent == re.launchIntent)
            launchArgs = mXmbSystems[si].launchArgs;
    } else {                                      // per-system ROM list
        if (mXmbSystemIndex < 0 || mXmbSystemIndex >= (int)mXmbSystems.size()) return;
        const XmbSystem& sys = mXmbSystems[mXmbSystemIndex];
        if (mXmbGameIndex < 0 || mXmbGameIndex >= (int)sys.roms.size()) return;
        romPath = sys.roms[mXmbGameIndex]; romDir = sys.romDir; coreSo = sys.coreSo;
        launchPkg = sys.launchPkg; launchIntent = sys.launchIntent; launchArgs = sys.launchArgs;
        standalone = sys.isStandalone();
        if (mXmbGameIndex < (int)sys.displayNames.size())
            romName = sys.displayNames[mXmbGameIndex];
    }
    if (romPath.empty()) return;
    if (launchSelectedAppShortcut()) return;
    // The game's own emulator (Game Options > Emulator), over its system's or the recorded one.
    { XmbSystem g;
      if (applyGameEmulator(romPath, g)) {
          coreSo = g.coreSo; launchPkg = g.launchPkg; launchIntent = g.launchIntent;
          launchArgs = g.launchArgs;
          standalone = g.isStandalone();
      } }
    // Same check as the home launcher: a game whose file is gone (deleted, or its card is not
    // mounted) must say so instead of replacing the running app with an emulator that then dies.
    if (!romLaunchExists(romPath)) {
        showRomMissingMsg(romName);
        // Prune the row only when its folder is reachable (genuinely deleted); an offline share/card
        // keeps the row so a briefly-down NAS never wipes valid Recently Played history.
        if (fromRecent && romParentDirReachable(romPath)) recentRemoveAt(mXmbGameIndex);
        return;
    }

    // OA-38: an emulator this platform cannot run starts with its default for this launch only.
    { XmbSystem g;
      const int si = fromRecent ? recentSystemIndex(mXmbRecent[mXmbGameIndex]) : mXmbSystemIndex;
      g.id = si >= 0 && si < (int)mXmbSystems.size() ? mXmbSystems[si].id : romDir;
      g.romDir = romDir;
      g.coreSo = coreSo; g.launchIntent = launchIntent;
      if (standalone) g.launchPkg = launchPkg;
      if (!emuCanRun(g) && emuApplyDefault(g)) {
          coreSo = g.coreSo; launchPkg = g.launchPkg; launchIntent = g.launchIntent;
          launchArgs = g.launchArgs;
          standalone = g.isStandalone();
      } }

    // Same emulator guard as the home launcher: if the core .so or standalone app is not on the
    // device, warn instead of replacing the running app with an emulator that immediately dies.
    if (standalone) {
        if (!packageInstalled(launchPkg)) { showEmuMissingMsg(romName, true); return; }
    } else if (!coreSoExists(coreSo)) {
        showEmuMissingMsg(romName, false); return;
    }

    // GammaOS: drastic-nano intercept for the resident overlay launcher. The
    // home XMB reroutes a DS ROM to the drastic-nano binary when
    // persist.gammaos.nano.drastic_nano=1; do the same here so launching a DS
    // ROM while the overlay is the launcher (overlay-home mode) does not fall
    // back to the stock DraStic APK. backend=drm/auto takes the DRM handshake:
    // drastic-nano grabs the panel, and the start trigger stops both the home
    // and this overlay. The dual-display RG DS uses exactly this DRM path,
    // unchanged. (SF mode is launched through its own host-activity path.)
    char dnGate[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.drastic_nano", dnGate, "0");
    char dnBackend[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.drastic_nano.backend", dnBackend, "auto");
    if (standalone && launchPkg == "com.dsemu.drastic" && dnGate[0] == '1' && drasticNanoAvailable()) {
        setDrasticNanoRomPath(romPath);
        // Keep Quick Resume's boot preview in sync with the game launched here. The
        // preview shows qr_game_name and, when storage is slow to mount, falls back
        // to the single .nds staged in the drastic cache. QR is armed later (at
        // power-off in prepareShutdown), not here, so if we launch a DS game from
        // the overlay without refreshing these the resume would preview the PREVIOUS
        // game. Refresh the display name and re-stage the cache for THIS ROM now (an
        // async copy that runs during gameplay), regardless of the QR toggle so a
        // mid-game toggle-on is covered too. Mirrors the XMB launch path.
        {
            std::string gameName = romPath;
            size_t ls = gameName.rfind('/');
            if (ls != std::string::npos) gameName = gameName.substr(ls + 1);
            size_t dot = gameName.rfind('.');
            if (dot != std::string::npos) gameName.erase(dot);
            property_set("persist.gammaos.nano.qr_game_name", gameName.c_str());
            // Point qr_core at drastic now (the resume is armed at power-off, but
            // the flag must not stay at a prior RetroArch core, or an abrupt reboot
            // during this DS session would resume the wrong game on the wrong
            // runtime). The boot dispatch reads qr_core to choose the DS preview vs
            // the RetroArch relaunch.
            property_set("persist.gammaos.nano.qr_core", "drastic");
            property_set("sys.gammaos.nano.cache_ready", "0");
            property_set("sys.gammaos.nano.cache_op", "populate_drastic");
        }
        // The DRM backend drives the panel directly; a monitor (the screen, or mirroring the panel)
        // is reached only through SurfaceFlinger.
        if (strcmp(dnBackend, "sf") == 0 || mTargetMonitor || nanodisplay::monitorListed()) {
            // SurfaceFlinger mode: launch the DrasticSf host activity through the
            // SAME overlay launch path as any other app (overlayLaunchCommand).
            // That is what dismisses the XMB onto the new app (overlayPoll), sets
            // app_launched, and runs the clean handoff -- the XMB must not be
            // treated specially here. The overlay stays running as the SF panel
            // keeper (the activity composites on top of it) and the activity starts
            // the drastic-nano binary itself (start_sf from onResume); we never
            // touch DRM master and never _exit.
            ALOGW("drastic nano: overlay launch -> drastic-nano (SF host)");
            overlayLaunchCommand("com.gammaos.drasticsf",
                "am start -W -n com.gammaos.drasticsf/.DrasticSfActivity "
                "-a android.intent.action.MAIN -c android.intent.category.LAUNCHER "
                "--activity-clear-task 2>/dev/null");
            return;
        }
        // DRM mode: do not let the overlay respawn after we exit -- drastic-nano
        // owns the panel now. Clear the QR prime so the home comes back plain.
        // The dual-display RG DS uses this DRM path, unchanged.
        //
        mOverlayLaunchStartMs = uptimeMillis();
        mOverlayLaunchPending = true;   // input stays frozen through the hand-off
        // DSi theme: the process exits below, so persist the carousel nav path for the fresh
        // home after the session (the same save the DRM home makes before its hand-off).
        if (mNdsTheme && mPs3Xmb) ndsSaveReturnPath();
        if (mEsdeTheme || mEsdeSettingsMinima) esdeSaveReturnPath();
        // The app the overlay is sitting over (a RetroArch game, say) must be gone
        // before drastic-nano takes DRM master: left running it keeps rendering and
        // holding the GPU and CPU under the DS session, and SurfaceFlinger keeps
        // compositing it against the panel drastic owns. Same clean close the
        // overlay's normal app launch performs (ESC + wait, force-stop only for a
        // hung one), run off the render thread so the overlay keeps drawing the
        // fade hold; the hand-off fires from that thread once the app is gone.
        {
            std::string old = mOverlayPausedPkg;
            if (old.empty() && property_get_bool("sys.gammaos.nano.app_launched", false)) {
                // The app was launched from the home, not through this overlay, so the
                // overlay never recorded it; the framework's launch_app is the running app.
                char la[PROPERTY_VALUE_MAX] = {};
                property_get("sys.gammaos.nano.launch_app", la, "");
                if (strchr(la, '.') && nanolaunch::isValidPackageName(la) &&
                    strcmp(la, "com.gammaos.drasticsf") != 0) old = la;
            }
            mOverlayPausedPkg.clear();
            unlink(kOverlayFrozenMarker);
            std::thread([this, old]() {
                property_set("sys.gammaos.nano.killing", "1");
                overlayCloseRunningApp(old);
                // No framework app resumes after this hand-off (drastic-nano is not an
                // activity), so nothing else would clear the guard; left at 1 it makes
                // RootWindowContainer skip every later home and app launch.
                property_set("sys.gammaos.nano.killing", "0");
                property_set("sys.gammaos.nano.overlay_ran", "0");
                property_set("sys.gammaos.nano.app_launched", "0");
                property_set("sys.gammaos.nano.show_overlay", "0");
                property_set("persist.gammaos.nano.qr_prepared", "0");
                ALOGW("drastic nano: overlay launch -> drastic-nano (DRM), %s closed",
                      old.empty() ? "no app" : old.c_str());
                property_set("sys.gammaos.drastic_nano.start", "1");
                _exit(0);
            }).detach();
        }
        // Keep the fade hold on screen until the worker exits the process.
        for (;;) {
            const int64_t f0 = (int64_t)android::elapsedRealtimeNano();
            render();
            mRenderHeartbeat.fetch_add(1, std::memory_order_relaxed);
            const int64_t spentUs = ((int64_t)android::elapsedRealtimeNano() - f0) / 1000;
            if (spentUs < 16666) usleep((useconds_t)(16666 - spentUs));
        }
    }

    std::string pkg, cmd;
    if (standalone) {
        pkg = launchPkg;
        nanolaunch::LaunchVars lv;
        lv.fileUri = nanolaunch::safContentUri(romPath, romDir);
        lv.filePath = nanolaunch::appVisiblePath(romPath);
        if (!launchFileIdFor(launchIntent, launchArgs, romPath, romName, lv.fileContent)) return;
        std::string direct;
        std::vector<std::string> toks;
        if (mupenDirectIntent(pkg, romPath, lv.fileUri, false, direct)) {
            cmd = "am start " + direct + " 2>/dev/null";
        } else if (nanolaunch::buildLaunchIntent(launchIntent, launchArgs, lv, toks)) {
            cmd = "am start " + nanolaunch::joinShell(toks) + " --grant-read-uri-permission 2>/dev/null";
        } else {
            showUnlaunchableNameMsg(romName);
            return;
        }
    } else {
        pkg = "com.retroarch.aarch64";
        std::string rom = romPath;                   // direct FUSE path for RetroArch
        if (rom.find("/data/media/0/") == 0) rom = "/sdcard/" + rom.substr(14);
        else if (rom.find("/mnt/media_rw/") == 0) rom = "/storage/" + rom.substr(14);
        std::string apk = overlayShellCapture("pm path com.retroarch.aarch64 2>/dev/null");
        {   size_t pp = apk.find("package:");
            if (pp != std::string::npos) {
                apk = apk.substr(pp + 8);
                size_t nl = apk.find_first_of("\r\n");
                if (nl != std::string::npos) apk = apk.substr(0, nl);
            } else {
                apk.clear();
            }
        }
        std::string ime = overlayShellCapture(
                "settings get secure default_input_method 2>/dev/null");
        if (ime == "null") ime.clear();
        const std::string dataDir = "/data/user/0/com.retroarch.aarch64";
        const std::string ext = "/storage/emulated/0/Android/data/com.retroarch.aarch64/files";
        cmd = "am start -n com.retroarch.aarch64/com.retroarch.browser.retroactivity.RetroActivityFuture"
              " -a android.intent.action.MAIN -c android.intent.category.LAUNCHER"
              " --activity-clear-task --activity-clear-top"
              " --es ROM " + overlayShq(rom) +
              " --es CONFIGFILE " + overlayShq(retroarchConfigFile()) +
              " --es DATADIR " + overlayShq(dataDir) +
              " --es SDCARD " + overlayShq(std::string("/storage/emulated/0")) +
              " --es EXTERNAL " + overlayShq(ext);
        if (!coreSo.empty())
            cmd += " --es LIBRETRO " + overlayShq("/data/data/com.retroarch.aarch64/cores/" + coreSo);
        if (!apk.empty()) cmd += " --es APK " + overlayShq(apk);
        if (!ime.empty()) cmd += " --es IME " + overlayShq(ime);
        cmd += " 2>/dev/null";
    }
    // Record the launch in Recently Played (mirror the home-mode launchXmbGame),
    // so games launched from the overlay show up in the overlay's Recently Played
    // list the next time it is opened. mXmbSystemIndex/mXmbGameIndex still hold the
    // selected entry. For a re-launch from Recently Played itself, move it to front.
    if (mXmbSystemIndex == -1) {
        if (mXmbGameIndex >= 0 && mXmbGameIndex < (int)mXmbRecent.size()) esdeGamelistPlayed(mXmbRecent[mXmbGameIndex].romPath);
        if (mXmbGameIndex > 0 && mXmbGameIndex < (int)mXmbRecent.size()) {
            XmbRecentEntry moved = mXmbRecent[mXmbGameIndex];
            mXmbRecent.erase(mXmbRecent.begin() + mXmbGameIndex);
            mXmbRecent.insert(mXmbRecent.begin(), moved);
            saveXmbRecent();
        }
    } else {
        addXmbRecent(mXmbSystemIndex, mXmbGameIndex);
    }
    // What runs now, for the button map over it (RetroArch's own content_history.lpl names its core).
    android::base::SetProperty("sys.gammaos.nano.launch_time", std::to_string((long long)time(nullptr)));
    commitLaunch(LaunchTuple().game(romDir, romPath,
                  standalone || coreSo.empty() ? std::string() : "/data/data/com.retroarch.aarch64/cores/" + coreSo));
    // Quick Resume prime for the overlay-launched game, mirroring the home XMB
    // launch (launchXmbGame). On the Brick the resident overlay IS the home after
    // the first game, so every launch after that comes through here -- and without
    // the same prime a RetroArch launch leaves qr_core stale (for example "drastic"
    // from a prior DS game). The boot resume dispatches on qr_core (== "drastic"
    // takes the in-process DS preview, otherwise the framework relaunches RetroArch
    // with that core), so a stale qr_core makes the next Restart resume the WRONG
    // game on the WRONG runtime. The standalone drastic ROM already returned above
    // via the drastic-nano intercept, so this only covers RetroArch cores and other
    // standalone apps (PPSSPP, etc., which have no Quick Resume).
    if (!standalone && mQuickResumeEnabled) {
        std::string corePath =
                "/data/data/com.retroarch.aarch64/cores/" + coreSo;
        setQrRomPath(romPath);
        property_set("persist.gammaos.nano.qr_core", corePath.c_str());
        property_set("persist.gammaos.nano.qr_prepared", "1");
        std::string gameName = romPath;
        size_t ls = gameName.rfind('/');
        if (ls != std::string::npos) gameName = gameName.substr(ls + 1);
        size_t dot = gameName.rfind('.');
        if (dot != std::string::npos) gameName.erase(dot);
        property_set("persist.gammaos.nano.qr_game_name", gameName.c_str());
        // Stage the DE cache for the QR preview, mirroring the home-XMB launch
        // (launchXmbGame, NanoMenuXmb.cpp:~1893). On a force-SF / overlay-home
        // device (e.g. Unisoc, where nano composites via a SurfaceFlinger layer)
        // the resident overlay IS the home after the first game, so every launch
        // after that comes through here; without this the DE cache is never warmed
        // and the next Quick Resume has no live preview (cache miss). do_populate
        // resolves the ROM from nano_qr_rom.txt (setQrRomPath above) and the core
        // from qr_core; the edge-trigger reset in nano_cache.sh makes the populate
        // re-fire even if cache_op already held "populate".
        property_set("sys.gammaos.nano.cache_ready", "0");
        property_set("sys.gammaos.nano.cache_op", "populate");
        ALOGI("overlay: primed Quick Resume (qr_core=%s, rom=%s)",
              corePath.c_str(), romPath.c_str());
    } else {
        // RetroArch with QR off, or a non-drastic standalone app: clear any stale
        // prime so the next boot does not resume an unrelated game.
        property_set("persist.gammaos.nano.qr_prepared", "0");
        property_set("persist.gammaos.nano.qr_core", "");
    }

    ALOGI("overlay: launch game pkg=%s standalone=%d rom=%s",
          pkg.c_str(), standalone ? 1 : 0, romPath.c_str());
    overlayLaunchCommand(pkg, cmd);
}

// ===================== Orientation control =====================
//
// Nano publishes a single foreground-aware orientation token to
// sys.gammaos.nano.force_orientation, which WindowManagerService reads in
// mapOrientationRequest (only while auto-rotation is off, which is nano's
// default). The token is: nano's own Screen Orientation setting when the nano
// menu or its overlay is foreground (default "landscape", so an app like
// Firefox cannot rotate the XMB to portrait), a per-app override when a normal
// app with one is foreground, or "none" (honor the app's own request) otherwise.

static int64_t nanoAppOrientMtime() {
    struct stat st;
    if (stat("/data/system/nano_app_orient.json", &st) != 0) return -1;
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

void NanoMenu::appOrientLoad() {
    mAppOrientLoaded = true;
    mAppOrient.clear();
    mAppOrientStamp = nanoAppOrientMtime();
    int fd = open("/data/system/nano_app_orient.json", O_RDONLY);
    if (fd < 0) return;
    std::string content;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 64 * 1024 * 1024) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) content.append(buf, (size_t)n);
    }
    close(fd);
    njson::Value root;
    if (!njson::parse(content, &root) || !root.isObject()) return;
    if (const njson::Value* apps = root.find("apps"); apps && apps->isObject())
        for (const auto& kv : apps->obj)
            if (kv.second.isString() && !kv.second.str.empty())
                mAppOrient[kv.first] = kv.second.str;
}

void NanoMenu::appOrientSave() {
    njson::Value root = njson::Value::makeObject();
    root.set("version") = njson::Value::makeNumber(1);
    njson::Value apps = njson::Value::makeObject();
    for (const auto& kv : mAppOrient) apps.set(kv.first) = njson::Value::makeString(kv.second);
    root.set("apps") = std::move(apps);
    std::string text = njson::serialize(root, true);

    const char* path = "/data/system/nano_app_orient.json";
    const char* tmp = "/data/system/nano_app_orient.json.tmp";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ALOGW("nano: cannot write %s (errno %d)", tmp, errno); return; }
    size_t off = 0; bool ok = true;
    while (off < text.size()) {
        ssize_t w = write(fd, text.c_str() + off, text.size() - off);
        if (w <= 0) { ok = false; break; }
        off += (size_t)w;
    }
    fsync(fd); close(fd);
    if (!ok) { unlink(tmp); return; }
    if (rename(tmp, path) != 0) { unlink(tmp); return; }
    (void)chown(path, 0, 0);
    (void)chmod(path, 0644);
    mAppOrientStamp = nanoAppOrientMtime();
}

std::string NanoMenu::appOrientGet(const std::string& pkg) {
    if (pkg.empty()) return std::string();
    // Reload if another nano process (the home menu sets the override, the
    // overlay enforces it) rewrote the file since we last read it.
    if (!mAppOrientLoaded || nanoAppOrientMtime() != mAppOrientStamp) appOrientLoad();
    auto it = mAppOrient.find(pkg);
    return it == mAppOrient.end() ? std::string() : it->second;
}

void NanoMenu::appOrientSet(const std::string& pkg, const std::string& value) {
    if (pkg.empty()) return;
    if (!mAppOrientLoaded) appOrientLoad();
    if (value.empty() || value == "default") mAppOrient.erase(pkg);
    else                                     mAppOrient[pkg] = value;
    appOrientSave();
    mLastOrientToken.clear();   // re-publish on the next orientationTick
}

void NanoMenu::overlayUpdateSurfaceSize() {
    if (!mOverlayMode || mDisplayToken == nullptr || mFlingerSurface == nullptr) return;
    ui::DisplayState state;
    if (SurfaceComposerClient::getDisplayState(mDisplayToken, &state) != NO_ERROR) return;
    // Cache the rotation BEFORE the size-unchanged early-return below: touchMapRaw
    // reads mOverlayRotation to follow a forced-portrait rotation over a landscape
    // panel. (A 90<->270 flip keeps the logical size but still rotates the axes.)
    mOverlayRotation = (int)state.orientation;
    // GammaOS hardware rotation key: on a swivel device the physical panel rotation is authoritative.
    // SurfaceFlinger's reported display orientation is NOT reliable here: on a square panel it reads
    // 0 on a clean boot, but after an app has driven the WMS logical rotation to 90 and exited it can
    // report 1 - and composing the rotate prop ON TOP of that double-rotated the overlay (the "works
    // on clean boot, breaks after app launch/exit" bug). So when the rotate feature is enabled we set
    // the overlay rotation SOLELY from sys.gammaos.rotate.state (the same signal
    // PhoneWindowManager/DisplayRotation use), ignoring state.orientation entirely. Both the render
    // matrix and touchMapRaw key off mOverlayRotation, so render and touch stay consistent.
    int renderSelfRot = mOverlayRotation;   // default: match SF orientation (non-rotate devices)
    if (property_get_bool("persist.gammaos.rotate.enabled", false)) {
        // Physical panel rotation from the authoritative rotate state prop.
        int physical = 0;
        if (property_get_int32("sys.gammaos.rotate.state", 0) == 1) {
            const int deg = property_get_int32("persist.gammaos.rotate.degrees", 90);
            physical = (deg == 270) ? 3 : (deg == 180) ? 2 : 1;
        }
        // Touch un-rotation follows the FULL physical rotation: nano reads raw evdev touch in panel
        // coordinates and its content appears at the physical rotation on screen.
        mOverlayRotation = physical;
        // Render self-rotation must COMPENSATE for what SurfaceFlinger already applies. On the nano
        // home (no app) SF keeps the display at orientation 0, so nano self-rotates the full amount.
        // Over a rotated app SF applies its own display transform (state.orientation != 0) to every
        // layer including nano's, so nano must self-rotate only the DIFFERENCE, or it double-rotates
        // and the overlay goes blank/upside-down (the "overlay over the app is blank at 90" bug).
        renderSelfRot = (physical - (int)state.orientation) & 3;
    }
    // Dual screen: the UI on the panel beside the game is upright, whatever the monitor does.
    if (mUiOnPanel) {
        mOverlayRotation = 0;
        renderSelfRot = 0;
    }
    // Turn nano's own rendering to match, compensating for any SurfaceFlinger display transform.
    nanoSetOverlayRenderRotation(renderSelfRot);
    // Instrumentation for the seamless-rotation work: log only on a change so the sequence during a
    // rotate is readable (physical from the rotate prop, SF orientation from getDisplayState, and the
    // resulting self-rotation nano applies). With SF pinned at 0 while the overlay is up this should
    // read sf=0 and renderSelfRot==physical, matching the seamless cold-boot path.
    {
        static int sLastLoggedRot = -100;
        const int packed = (mOverlayRotation << 8) | ((int)state.orientation << 4) | (renderSelfRot & 0xf);
        if (packed != sLastLoggedRot) {
            sLastLoggedRot = packed;
            ALOGI("nano rotate: physical=%d sf_orient=%d renderSelfRot=%d (lw=%d lh=%d)",
                  mOverlayRotation, (int)state.orientation, renderSelfRot,
                  (int)state.layerStackSpaceRect.getWidth(),
                  (int)state.layerStackSpaceRect.getHeight());
        }
    }
    const int lw = (int)state.layerStackSpaceRect.getWidth();
    const int lh = (int)state.layerStackSpaceRect.getHeight();
    if (lw <= 0 || lh <= 0) return;
    resizeMainSurface(lw, lh);
}

// Nano's surface at a new size: on a rotation (overlayUpdateSurfaceSize), or on another display
// (followDisplay0).
bool NanoMenu::resizeFlingerSurface(int lw, int lh) {
    if (mFlingerSurface == nullptr || lw <= 0 || lh <= 0) return false;
    // The display's logical size changed (a rotation, e.g. nano forced portrait, or another display).
    // We must land a producer buffer of the NEW size so SurfaceFlinger composites
    // us over the whole rotated display. setBuffersDimensions alone is not enough:
    // the GLES driver caches the window geometry at eglCreateWindowSurface time and
    // re-asserts it on every dequeue, so it clobbers a bare setBuffersDimensions and
    // keeps handing out landscape (1920x1080) buffers. SF then crops that square and
    // parks it in a corner of the portrait panel (observed on RK3576: source_crop
    // 840,0,1920,1080 -> display_frame 0,840,1080,1920). Recreate the EGL window
    // surface after resizing the ANativeWindow so the driver re-reads the new
    // dimensions and allocates portrait (1080x1920) buffers that fill the display.
    // A native window can host only ONE EGLSurface at a time, so the old surface
    // must be torn down before the new one is created: creating first fails with
    // EGL_BAD_ALLOC (the window is still connected to the old producer). Unbind the
    // context, destroy the old surface, resize the ANativeWindow, then recreate.
    const int prevW = mWidth, prevH = mHeight;
    EGLConfig config = getEglConfig(mDisplay, mOverlayMode);
    eglMakeCurrent(mDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mSurface != EGL_NO_SURFACE) {
        eglDestroySurface(mDisplay, mSurface);
        mSurface = EGL_NO_SURFACE;
    }
    // Resize the SurfaceControl's BLASTBufferQueue to the new size. This is the
    // consumer (SurfaceFlinger) side: the layer's queue was created at the surface
    // creation size (1920x1080), and SF only latches buffers that match the queue
    // size. Without this, a portrait (1080x1920) buffer is produced but never
    // latched, so SF keeps compositing the last landscape buffer - the menu renders
    // and swaps but the panel is frozen on a stale frame (landscape-native sizes
    // happen to match, so only portrait wedged). updateDefaultBufferSize forwards to
    // BLASTBufferQueue::update so producer and consumer agree on the new geometry.
    if (mFlingerSurfaceControl != nullptr)
        mFlingerSurfaceControl->updateDefaultBufferSize((uint32_t)lw, (uint32_t)lh);
    // Use USER dimensions, not buffer (request) dimensions: eglCreateWindowSurface
    // sizes the surface from NATIVE_WINDOW_DEFAULT_WIDTH/HEIGHT, which reads the
    // user dimensions and otherwise falls back to the SurfaceControl's creation
    // size (1920x1080). setBuffersDimensions only sets the request size, so the
    // driver kept making landscape surfaces. User dimensions also drive dequeue,
    // so the whole pipeline (EGL surface + buffers) matches the portrait display.
    native_window_set_buffers_user_dimensions(mFlingerSurface.get(), lw, lh);
    EGLSurface ns = eglCreateWindowSurface(mDisplay, config, mFlingerSurface.get(), nullptr);
    if (ns == EGL_NO_SURFACE) {
        // Fall back to a surface at the previous size so rendering keeps going.
        ALOGE("nano overlay: eglCreateWindowSurface failed at %dx%d (0x%x); "
              "restoring %dx%d", lw, lh, eglGetError(), prevW, prevH);
        if (mFlingerSurfaceControl != nullptr)
            mFlingerSurfaceControl->updateDefaultBufferSize((uint32_t)prevW, (uint32_t)prevH);
        native_window_set_buffers_user_dimensions(mFlingerSurface.get(), prevW, prevH);
        ns = eglCreateWindowSurface(mDisplay, config, mFlingerSurface.get(), nullptr);
        if (ns == EGL_NO_SURFACE) {
            ALOGE("nano overlay: fallback surface create failed (0x%x)", eglGetError());
            return false;
        }
        eglMakeCurrent(mDisplay, ns, ns, mContext);
        if (mOverlayMode) eglSwapInterval(mDisplay, 0);
        mSurface = ns;
        return false;
    }
    eglMakeCurrent(mDisplay, ns, ns, mContext);
    // Overlay pacing is swap-interval 0 (the threadLoop top-up sleep drives frames),
    // matching the creation path in NanoMenu.cpp.
    if (mOverlayMode) eglSwapInterval(mDisplay, 0);
    mSurface = ns;

    // Trust the actual surface geometry the driver gave us.
    EGLint qw = lw, qh = lh;
    eglQuerySurface(mDisplay, mSurface, EGL_WIDTH, &qw);
    eglQuerySurface(mDisplay, mSurface, EGL_HEIGHT, &qh);
    ALOGI("nano overlay: display logical size %dx%d -> recreated surface %dx%d (was %dx%d)",
          lw, lh, qw, qh, prevW, prevH);
    mWidth = qw; mHeight = qh;
    mDisplayDirty = true;
    mMonitorFresh = false;   // dual screen: no frame at the new size yet
    return true;
}

// NanoDisplayTarget.h: keep Nano on the display showing layer stack 0. A monitor plugged in becomes
// that display, with the panel off (docked) or beside it (dual screen mode, setDualPanel); unplugged,
// the panel is it again.
// The home re-creates its surface at the display's mode (a monitor's can still change after it
// connects); the overlay follows the display's logical size through overlayUpdateSurfaceSize.
// Also re-applies the layer stack when DisplayManager reassigns it (port 1 can start at layer stack
// 1 and become 2).
void NanoMenu::followDisplay0() {
    if (mDisplayToken == nullptr || mFlingerSurfaceControl == nullptr) return;
    // Shorter than the home's ~480 ms check, so each check sees the display as it is now.
    const int64_t now = uptimeMillis();
    if (now - mDisplayFollowMs < 250) return;
    mDisplayFollowMs = now;
    const std::vector<PhysicalDisplayId> ids = SurfaceComposerClient::getPhysicalDisplayIds();
    const nanodisplay::Choice c = nanodisplay::sfChoose(ids);
    mDisplayShown = c.target >= 0 && c.shown;
    if (c.target < 0 || !c.shown) return;   // asleep: no display shows layer stack 0, nothing changed
    const sp<IBinder> token = SurfaceComposerClient::getPhysicalDisplayToken(ids[c.target]);
    ui::DisplayState state;
    if (token == nullptr || SurfaceComposerClient::getDisplayState(token, &state) != NO_ERROR) return;
    // A panel blanked beside the monitor turns dual screen on only once it shows something, but does
    // not turn it off: around sleep the screens blank one after the other.
    const bool panelBlankedMidDual = mDualPanel && c.monitor && c.second < 0 && c.blankSecond >= 0 &&
                                     ids[c.blankSecond].value == mDualPanelId;
    if (!panelBlankedMidDual)
        setDualPanel(c.monitor && c.second >= 0, c.second >= 0 ? ids[c.second].value : 0);
    if (c.monitor != mTargetMonitor) {
        mTargetMonitor = c.monitor;
        // A second panel's layers (wallpaper, Control Center) hide while a monitor is the screen:
        // one of them can be on the layer stack the monitor now shows.
        if (!mSecondaryWallpaperControls.empty()) {
            SurfaceComposerClient::Transaction t;
            for (const auto& sc : mSecondaryWallpaperControls) {
                if (sc == nullptr) continue;
                if (panelsOff()) t.hide(sc);
                else t.show(sc);
            }
            t.apply();
        }
        // A touch across the change: let the panel's contacts go while the monitor is the screen
        // (its events are dropped, the lift included), and read them afresh once it is back.
        if (panelsOff()) {
            std::vector<int> fds;
            for (const auto& [fd, p] : mTouchPanels) fds.push_back(fd);
            for (int fd : fds) touchPanelGone(fd);
        } else {
            touchResync(-1);
        }
    }
    const bool moved = token != mDisplayToken;
    if (moved) {
        ALOGI("NanoMenu: drawing on display %" PRIu64 "%s", ids[c.target].value,
              c.monitor ? " (monitor)" : "");
        mDisplayToken = token;
    }
    if (state.layerStack.id != mAppliedLayerStack) {
        SurfaceComposerClient::Transaction t;
        t.setLayerStack(mFlingerSurfaceControl, state.layerStack);
        t.apply();
        ALOGI("NanoMenu: layerStack changed %u -> %u, reapplied", mAppliedLayerStack, state.layerStack.id);
        mAppliedLayerStack = state.layerStack.id;
    }
    if (mOverlayMode || !(moved || c.monitor)) return;
    ui::DisplayMode mode;
    if (SurfaceComposerClient::getActiveDisplayMode(token, &mode) != NO_ERROR) return;
    const int w = mode.resolution.width, h = mode.resolution.height;
    if (moved && !c.monitor) {   // the panel: Nano sets its projection, as at start
        SurfaceComposerClient::Transaction t;
        t.setDisplayProjection(token, ui::ROTATION_0, Rect(0, 0, w, h), Rect(0, 0, w, h));
        t.apply();
    }
    resizeMainSurface(w, h);
}

// resizeFlingerSurface for the target's own surface, wherever the UI is (uiToPanel).
void NanoMenu::resizeMainSurface(int w, int h) {
    const bool onPanel = mUiOnPanel;
    const int cw = onPanel ? mMonitorUiSize.first : mWidth;
    const int ch = onPanel ? mMonitorUiSize.second : mHeight;
    if (w == cw && h == ch) return;
    if (onPanel) swapUiSurface(false);
    resizeFlingerSurface(w, h);
    if (onPanel) swapUiSurface(true);
}

// Dual screen on or off (followDisplay0, the surface setup). Off, the UI comes back to the target and
// the panel's surfaces go: the panel is switched off, or it is the target itself now.
void NanoMenu::setDualPanel(bool on, uint64_t id) {
    if (on == mDualPanel && (!on || id == mDualPanelId)) return;
    if (mDualPanel) {
        uiToPanel(false);
        releaseSecondarySurfaces();
        // Over a game its layer was hidden for the panel: the menu is back on it.
        if (mOverlayMode && mOverlayShown && !mMonitorLayerShown) mOverlayPendingShow = true;
    }
    mDualPanel = on;
    mDualPanelId = on ? id : 0;
    if (on) mMonitorFresh = false;   // until a frame of the home is drawn on the monitor
    ALOGI("NanoMenu: dual screen %s", on ? "on" : "off");
    panelTouchSync();
    mDisplayDirty = true;
}

// Where the UI draws in dual screen mode (nanodisplay::uiOnPanel). A menu stays on the monitor until
// the monitor's surface holds a frame of the home without one (mMonitorFresh): after a game (its
// layer was hidden for it), a plug-in or a resize there is none to leave there.
bool NanoMenu::panelUiWanted() const {
    bool overGame = false;
    return panelUiRaw(&overGame) && (overGame || mMonitorFresh);
}

// nanodisplay::uiOnPanel for what Nano shows now.
bool NanoMenu::panelUiRaw(bool* overGame) const {
    nanodisplay::UiState s;
    s.overGame = mOverlayMode && mOverlayShown && !mOverlayWallpaper;
    s.media = mVidActive || mPvActive || mPdfActive || mMpActive || mEsdeViewerActive;
    s.menu = quickMenuOpen() || applicationsViewOpen() || bmapActive() || esdeMenuInContext() ||
             mEsdeMenuClosing;
    s.settingsOpen = mEsdeSettingsMinima ||
                     (mPs3SettingsCatIdx >= 0 && mPs3CatIdx == mPs3SettingsCatIdx &&
                      ((mNdsTheme || mMinimaTheme) ? !mNdsAtRoot : !mPs3Stack.empty()));
    if (overGame) *overGame = s.overGame;
    return nanodisplay::uiOnPanel(s);
}

// The swap interval for the UI's surface: the home and the overlay acting as it are paced by it,
// the overlay over a game is not (overlayApplyPresentMode).
int NanoMenu::uiSwapInterval() const {
    return (mOverlayMode && !mOverlayWallpaper) ? 0 : 1;
}

// mSurface, mWidth and mHeight to the panel's surface (the first secondary) or back; the panel's own
// size stays in mSecondaryCreatedSize, the monitor's is kept in mMonitorUiSize meanwhile.
void NanoMenu::swapUiSurface(bool on) {
    std::swap(mSurface, mSecondaryEglSurfaces[0]);
    if (on) {
        mMonitorUiSize = {mWidth, mHeight};
        mWidth = mSecondaryCreatedSize[0].first;
        mHeight = mSecondaryCreatedSize[0].second;
    } else {
        mWidth = mMonitorUiSize.first;
        mHeight = mMonitorUiSize.second;
    }
    mUiOnPanel = on;
    eglMakeCurrent(mDisplay, mSecondaryEglSurfaces[0], mSecondaryEglSurfaces[0], mContext);
    eglSwapInterval(mDisplay, 0);   // the surface not showing the UI never blocks a frame
    eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);
    mUiInterval = uiSwapInterval();
    eglSwapInterval(mDisplay, mUiInterval);
}

// Moves the UI onto the panel, or back to the monitor, whose surface keeps its last frame meanwhile.
// The layers follow in dualPanelTick.
void NanoMenu::uiToPanel(bool on) {
    if (on == mUiOnPanel) return;
    if (on && (mSecondaryEglSurfaces.empty() || mSecondaryCreatedSize.empty())) return;
    swapUiSurface(on);
    if (on && mOverlayMode && !mOverlayWallpaper) mMonitorFresh = false;   // hidden beside the game
    if (on && mOverlayMode) {   // upright on the panel from its first frame (overlayUpdateSurfaceSize)
        mOverlayRotation = 0;
        nanoSetOverlayRenderRotation(0);
    }
    // A touch across the move: the panel's contacts go with the UI, and are read afresh on its return.
    if (on) {
        touchResync(-1);
    } else {
        std::vector<int> fds;
        for (const auto& [fd, p] : mTouchPanels) fds.push_back(fd);
        for (int fd : fds) touchPanelGone(fd);
    }
    panelTouchSync();
    mDisplayDirty = true;
    ALOGI("NanoMenu: UI on the %s (%dx%d)", on ? "panel" : "monitor", mWidth, mHeight);
}

// Once a frame, before anything draws: in dual screen mode the panel's surface exists on the panel's
// current layer stack, the UI is where it belongs, and the layers show what they should. A layer
// coming back is shown after the frame drawn into it (mOverlayPendingShow, mPanelRevealPending).
void NanoMenu::dualPanelTick() {
    panelAppTick();
    if (!mDualPanel) return;
    if (mSecondaryEglSurfaces.empty()) setupSecondaryEglSurfaces();
    uiToPanel(panelUiWanted());
    if (mUiOnPanel && mUiInterval != uiSwapInterval()) {   // the overlay became the home, or stopped being it
        mUiInterval = uiSwapInterval();
        eglSwapInterval(mDisplay, mUiInterval);
    }
    // DisplayManager can give the panel another layer stack after a connect.
    const int64_t now = uptimeMillis();
    if (now - mPanelStackCheckMs >= 500 && !mSecondaryDisplayTokens.empty() &&
        mSecondaryDisplayTokens[0] != nullptr && !mSecondaryWallpaperControls.empty() &&
        !mSecondaryAppliedLayerStacks.empty()) {
        mPanelStackCheckMs = now;
        ui::DisplayState st;
        if (SurfaceComposerClient::getDisplayState(mSecondaryDisplayTokens[0], &st) == NO_ERROR &&
            st.layerStack != ui::INVALID_LAYER_STACK &&   // blanked or not enabled yet
            st.layerStack.id != mSecondaryAppliedLayerStacks[0]) {
            SurfaceComposerClient::Transaction t;
            t.setLayerStack(mSecondaryWallpaperControls[0], st.layerStack);
            t.apply();
            ALOGI("NanoMenu: panel layerStack %u -> %u", mSecondaryAppliedLayerStacks[0], st.layerStack.id);
            mSecondaryAppliedLayerStacks[0] = st.layerStack.id;
        }
    }
    if (mOverlayMode) {
        // The monitor layer: up with the overlay, but not while the UI is on the panel beside a game.
        if (mOverlayShown && !mOverlayPendingShow && mFlingerSurfaceControl != nullptr) {
            const bool want = !(mUiOnPanel && !mOverlayWallpaper);
            if (want && !mMonitorLayerShown) {
                if (!mUiOnPanel) {
                    mOverlayPendingShow = true;   // this frame draws it
                } else {
                    SurfaceComposerClient::Transaction t;   // it holds a frame of the home (mMonitorFresh)
                    t.show(mFlingerSurfaceControl);
                    t.apply();
                    mMonitorLayerShown = true;
                }
            } else if (!want && mMonitorLayerShown) {
                SurfaceComposerClient::Transaction t;
                t.hide(mFlingerSurfaceControl);
                t.apply();
                mMonitorLayerShown = false;
            }
        }
        return;   // the overlay's panel layer: render()'s wantShown
    }
    // The home's panel layer: off the panel's own app while the UI is elsewhere.
    const bool hide = !mPanelApp.empty() && !mUiOnPanel;
    if (hide && !mPanelLayerHidden && !mSecondaryWallpaperControls.empty()) {
        SurfaceComposerClient::Transaction t;
        for (const auto& sc : mSecondaryWallpaperControls) if (sc != nullptr) t.hide(sc);
        t.apply();
        mPanelLayerHidden = true;
        mNdsSecondaryShown = false;
        mPanelRevealPending = false;
    } else if (!hide && mPanelLayerHidden) {
        mPanelRevealPending = true;
    }
}

// End of render(), after both passes: the home's panel layer comes back over a fresh frame.
void NanoMenu::dualPanelReveal() {
    if (!mPanelRevealPending) return;
    mPanelRevealPending = false;
    SurfaceComposerClient::Transaction t;
    for (const auto& sc : mSecondaryWallpaperControls) if (sc != nullptr) t.show(sc);
    t.apply();
    mPanelLayerHidden = false;
    mNdsSecondaryShown = true;
}

// Dual screen: start an app (or Android Settings) on the panel, leaving the home or the game on the
// monitor as it is; it takes the controller where Controller Screen says so. False when the app
// should open the usual way, on the monitor (the screen chosen for it, or the default:
// panelOpenWanted). An app moves between the screens rather than starting again.
bool NanoMenu::panelLaunch(const std::string& pkg, bool settings, bool always) {
    if (!mDualPanel || mSecondaryAppliedLayerStacks.empty() || !nanolaunch::isValidPackageName(pkg)) return false;
    if (mSecondaryAppliedLayerStacks[0] == ui::INVALID_LAYER_STACK.id) return false;   // no display id yet
    // A home grabbing the pads (persist.gammaos.nano.grab_input) would keep the keys from the app.
    if (!mOverlayMode && android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) return false;
    // The game on the monitor, chosen again: back to it, not moved.
    if (!settings && pkg == mOverlayPausedPkg && gameOnMonitor()) return false;
    if (!settings && !always && !panelOpenWanted(pkg)) {
        if (pkg != mPanelApp) return false;
        panelAppToMonitor();
        return true;
    }
    if (!settings && mOverlayMode && mOverlayShown && !mOverlayWallpaper && pkg == mOverlayPausedPkg &&
        !gameOnMonitor())
        return monitorAppToPanel();
    const int disp = (int)mSecondaryAppliedLayerStacks[0];   // a display's layer stack is its id
    std::string c;
    if (!mPanelApp.empty() && mPanelApp != pkg)
        c += "am force-stop " + nanolaunch::shellQuote(mPanelApp) + " 2>/dev/null; ";
    char amc[192];
    if (settings) {
        snprintf(amc, sizeof(amc), "am start --display %d -a android.settings.SETTINGS 2>/dev/null", disp);
        c += amc;
    } else {
        c += "ACT=$(cmd package resolve-activity --brief -a android.intent.action.MAIN "
             "-c android.intent.category.LAUNCHER " + nanolaunch::shellQuote(pkg) + " 2>/dev/null | tail -1); ";
        snprintf(amc, sizeof(amc), "case \"$ACT\" in */*) am start --display %d -n \"$ACT\" 2>/dev/null;; esac", disp);
        c += amc;
    }
    ALOGI("NanoMenu: %s on the panel (display %d)", pkg.c_str(), disp);
    std::thread([c]() { (void)system(c.c_str()); }).detach();
    mPanelApp = pkg;
    property_set("sys.gammaos.nano.panel_app", pkg.c_str());
    mPanelAppGen.fetch_add(1, std::memory_order_acq_rel);
    mPanelAppGoneStreak.store(0, std::memory_order_release);
    mPanelAppWatchMs = uptimeMillis() + 2500;   // the first check after the app is up
    mPs3CatsStale = true;                       // the Quick Menu's panel app rows
    if (mOverlayMode && mOverlayShown && !mOverlayWallpaper) {
        overlayHide();   // back to the game, which keeps running beside the app
    } else {
        if (applicationsViewOpen()) closeApplicationsView();
        if (quickMenuOpen()) toggleQuickMenu();
    }
    const bool take = panelPinAllowed() && nanoscreen::focusOnLaunch(nanoscreen::ctrlMode(
            android::base::GetProperty("persist.gammaos.nano.ctrl_screen", "monitor")));
    panelAppFocus(take, take);
    return true;
}

// The controller to the panel's app, or back to Nano (and the game under the overlay). Keys reach
// an app only while Nano doesn't read them itself (drop_input).
void NanoMenu::panelAppFocus(bool focused, bool notice) {
    mPanelAppFocused = focused && !mPanelApp.empty() && !mSecondaryAppliedLayerStacks.empty() &&
                       mSecondaryAppliedLayerStacks[0] != ui::INVALID_LAYER_STACK.id;
    char v[16];
    snprintf(v, sizeof(v), "%d", mPanelAppFocused ? (int)mSecondaryAppliedLayerStacks[0] : -1);
    property_set("sys.gammaos.nano.focus.display", v);
    const bool nanoReads = !mPanelAppFocused && (!mOverlayMode || mOverlayShown);
    property_set("sys.gammaos.nano.drop_input", nanoReads ? "1" : "0");
    mPanelFnDown = false;
    // Presses whose release the other side may take.
    mAnbernicArmed = false;
    mAnbernicHeld = anbernicPhysicallyDown(&mAnbernicFd);
    mSsHeld[0] = mSsHeld[1] = false;
    mBackHeld = false;
    navRelease(NavDir::None);
    if (notice && mDualPanel) {   // the monitor is display 0
        if (mFocusNoticeCount == 0) mFocusNoticeCount = (uint32_t)uptimeMillis();   // new in each process
        const std::string n = nanoscreen::noticeValue(
                ++mFocusNoticeCount, mPanelAppFocused ? (int)mSecondaryAppliedLayerStacks[0] : 0);
        property_set("sys.gammaos.nano.focus.notice", n.c_str());
    }
    panelTouchSync();
}

// While Nano has the controller with its UI on the monitor, touches on the panel still reach the
// panel's app (InputDispatcher lets them through drop_input).
void NanoMenu::panelTouchSync() {
    int want = -1;
    // A parked overlay is off the panel whatever mUiOnPanel says (it follows only as Nano draws).
    const bool uiOnPanel = mUiOnPanel && (!mOverlayMode || mOverlayShown);
    if (mDualPanel && !mPanelApp.empty() && !uiOnPanel && !mSecondaryAppliedLayerStacks.empty() &&
        mSecondaryAppliedLayerStacks[0] != ui::INVALID_LAYER_STACK.id)
        want = (int)mSecondaryAppliedLayerStacks[0];
    if (want == mPanelTouchDisp) return;
    mPanelTouchDisp = want;
    property_set("sys.gammaos.nano.touch_display", std::to_string(want).c_str());
}

// A game runs on the monitor: an emulator Nano started, or the DS player.
bool NanoMenu::gameOnMonitor() const {
    if (!mOverlayMode) return false;
    if (property_get_bool("sys.gammaos.drastic_nano.session", false)) return true;
    if (!property_get_bool("sys.gammaos.nano.app_launched", false)) return false;
    if (!android::base::GetProperty("sys.gammaos.nano.launch_game", "").empty()) return true;
    // A Quick Resume boot records only launch_rom (an app's launch clears it: setLaunchRomPath).
    if (!android::base::GetProperty("sys.gammaos.nano.launch_rom", "").empty()) return true;
    // The file holds "<launch_time>\n<path>": only this launch's counts.
    const std::string f = readPathFile(kLaunchGameFile);
    const std::string t = android::base::GetProperty("sys.gammaos.nano.launch_time", "");
    return !t.empty() && f.size() > t.size() && f.compare(0, t.size() + 1, t + "\n") == 0;
}

// Dual screen: the screen chosen for the app (persist.gammaos.nano.screen_panel_pkgs and
// screen_monitor_pkgs), else the monitor unless a game runs there.
bool NanoMenu::panelOpenWanted(const std::string& pkg) {
    return nanoscreen::openOnPanel(screenPanelHas(pkg), screenMonitorHas(pkg), gameOnMonitor());
}

// Open on Monitor / Open on Built-in Screen: remembered for the app, then it opens (or moves) there.
void NanoMenu::openAppOnScreen(const std::string& pkg, bool panel) {
    screenChoiceSet(pkg, panel ? 1 : 2);
    if (panelLaunch(pkg)) return;
    launchInstalledApp(pkg, true);
}

// The panel's app to the monitor, still running: a start on display 0 moves its task there (the
// overlay's launches name the monitor in dual screen mode). The cold-boot home only hands launches
// to the framework, which starts apps as a home, so there it starts again on the monitor.
void NanoMenu::panelAppToMonitor() {
    const std::string pkg = mPanelApp;
    if (pkg.empty()) return;
    ALOGI("NanoMenu: %s from the panel to the monitor", pkg.c_str());
    panelAppForget();
    if (quickMenuOpen()) toggleQuickMenu();
    if (applicationsViewOpen()) closeApplicationsView();
    if (mOverlayMode) {
        overlayLaunchPackage(pkg);
        return;
    }
    (void)system(("am force-stop " + nanolaunch::shellQuote(pkg) + " 2>/dev/null").c_str());
    launchInstalledApp(pkg, true);
}

// The app on the monitor (under the overlay) to the panel, still running: the overlay becomes the
// home there, and a start on the panel moves the app's task. Not a game.
bool NanoMenu::monitorAppToPanel() {
    const std::string pkg = mOverlayPausedPkg;
    if (!mOverlayMode || pkg.empty() || gameOnMonitor() || !nanolaunch::isValidPackageName(pkg)) return false;
    if (!mDualPanel || mSecondaryAppliedLayerStacks.empty() ||
        mSecondaryAppliedLayerStacks[0] == ui::INVALID_LAYER_STACK.id)
        return false;   // panelLaunch would refuse it, after the app was sent to the background
    ALOGI("NanoMenu: %s from the monitor to the panel", pkg.c_str());
    overlayQuitToHome(true);
    return panelLaunch(pkg, false, true);
}

// The Switch Screen hotkey: the controller to the panel's app, or back. With no app there it stays,
// and the monitor shows that it has it.
void NanoMenu::screenSwap() {
    if (!mDualPanel) return;
    if (mOverlayMode && mOverlayShown && !mOverlayWallpaper) return;   // the Quick Menu over an app has it
    const bool toPanel = !mPanelAppFocused && !mPanelApp.empty() && panelPinAllowed();
    if (toPanel) {   // the menus covering the panel's app close; Settings or a viewer there keeps it
        if (quickMenuOpen()) toggleQuickMenu();
        if (applicationsViewOpen()) closeApplicationsView();
        if (panelUiRaw(nullptr) && !quickMenuOpen() && !applicationsViewOpen()) return;
    }
    panelAppFocus(toPanel, true);
}

// The controller may go to the panel's app: not in a DS player session, which holds its input back
// only by drop_input and would act on every press too.
bool NanoMenu::panelPinAllowed() const {
    return !property_get_bool("sys.gammaos.drastic_nano.session", false);
}

// The panel's app is gone from it (moved to the monitor, or started there by another route).
void NanoMenu::panelAppForget() {
    mPanelApp.clear();
    mPanelAppGen.fetch_add(1, std::memory_order_acq_rel);
    property_set("sys.gammaos.nano.panel_app", "");
    panelAppFocus(false);
    mPs3CatsStale = true;
}

bool NanoMenu::propEdge(PropEdge& e) {
    if (!e.pi) e.pi = __system_property_find(e.name);
    const uint32_t s = e.pi ? __system_property_serial(e.pi) : 0;
    if (e.inited && s == e.serial) return false;
    const bool fresh = e.inited;
    e.inited = true;
    e.serial = s;
    return fresh;
}

// PhoneWindowManager's Anbernic + Switch Screen (sys.gammaos.nano.screen_swap, the time of the press),
// over an app or while the panel's app has the controller; and a finger down on the panel's app
// (sys.gammaos.nano.panel_touch, from InputDispatcher), which brings the controller where Controller
// Screen says so. Values set before Nano started are old.
void NanoMenu::screenSwapPoll() {
    // A parked overlay beside a home Nano started (persist.gammaos.nano.overlay off) leaves them to it.
    const bool parkedBesideHome = mOverlayMode && !mOverlayShown &&
                                  !property_get_bool("sys.gammaos.nano.app_launched", false);
    const bool swap = propEdge(mScreenSwapEdge), touch = propEdge(mPanelTouchEdge);
    if (parkedBesideHome) return;
    if (swap) screenSwap();
    if (touch && !mPanelApp.empty() && !mPanelAppFocused && (!mUiOnPanel || (mOverlayMode && !mOverlayShown)) &&
        panelPinAllowed() &&
        nanoscreen::focusOnTouch(nanoscreen::ctrlMode(
                android::base::GetProperty("persist.gammaos.nano.ctrl_screen", "monitor")))) {
        mLastInputMs = android::uptimeMillis();
        panelAppFocus(true, true);
    }
}

void NanoMenu::panelAppClose() {
    if (mPanelApp.empty()) return;
    const std::string c = "am force-stop " + nanolaunch::shellQuote(mPanelApp) + " 2>/dev/null";
    std::thread([c]() { (void)system(c.c_str()); }).detach();
    mPanelAppGoneGen.store(mPanelAppGen.load(std::memory_order_acquire), std::memory_order_release);
}

// Once a frame, and while the overlay is parked: adopt the panel's app from the other Nano process,
// notice that it has exited (three checks in a row, as for the Control Center's bottom app), and
// keep Nano's panel layer off it while the UI isn't there.
void NanoMenu::panelAppTick() {
    static bool sAdopted = false;
    if (!sAdopted && mDualPanel) {
        sAdopted = true;
        char pa[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.panel_app", pa, "");
        if (nanolaunch::isValidPackageName(pa)) {
            mPanelApp = pa;
            mPanelAppWatchMs = uptimeMillis();
            mPs3CatsStale = true;
            panelAppFocus(false);   // a restarted Nano takes the controller back from it
        }
    }
    if (mDualPanel) screenSwapPoll();
    panelTouchSync();
    if (mPanelApp.empty()) return;
    const bool gone = mPanelAppGoneGen.load(std::memory_order_acquire) ==
                      mPanelAppGen.load(std::memory_order_acquire);
    if (!mDualPanel || gone) {
        ALOGI("NanoMenu: the panel's app %s has gone", mPanelApp.c_str());
        char mode[PROPERTY_VALUE_MAX] = {};
        property_get("persist.gammaos.display.mode", mode, "docked");
        if (!mDualPanel && (strcmp(mode, "dual") != 0 || !nanodisplay::monitorConnected())) {
            // Back to docked or unplugged: it closes with the panel (DESTROY_CONTENT does too).
            const std::string c = "am force-stop " + nanolaunch::shellQuote(mPanelApp) + " 2>/dev/null";
            std::thread([c]() { (void)system(c.c_str()); }).detach();
        }
        mPanelApp.clear();
        mPanelAppGen.fetch_add(1, std::memory_order_acq_rel);
        property_set("sys.gammaos.nano.panel_app", "");
        panelAppFocus(false);
        mPs3CatsStale = true;
        if (quickMenuOpen()) quickMenuRefresh();
        mDisplayDirty = true;
        return;
    }
    const int64_t t = uptimeMillis();
    if (t - mPanelAppWatchMs < (mOverlayMode && !mOverlayShown ? 3000 : 1000)) return;   // lighter beside a game
    // InputDispatcher clears drop_input when a window gains focus (a dialog in the panel's app): keep
    // the app off the controller while Nano has it.
    if (!mPanelAppFocused && (!mOverlayMode || mOverlayShown) &&
        !android::base::GetBoolProperty("sys.gammaos.nano.drop_input", false))
        property_set("sys.gammaos.nano.drop_input", "1");
    bool expected = false;
    if (!mPanelPollBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    mPanelAppWatchMs = t;
    char ss[PROPERTY_VALUE_MAX] = {};
    property_get("sys.screen.state", ss, "on");
    if (strcmp(ss, "on") != 0) {   // asleep its activities are paused: not seen as resumed
        mPanelPollBusy.store(false, std::memory_order_release);
        return;
    }
    const std::string pkg = mPanelApp;
    const uint32_t gen = mPanelAppGen.load(std::memory_order_acquire);
    NanoMenu* self = this;
    std::thread([self, pkg, gen]() {
        const std::string cmd =
            "D=$(dumpsys activity activities 2>/dev/null); "
            "echo \"$D\" | grep -F " + nanolaunch::shellQuote(":" + pkg + " ") + " | grep -q 'visible=true' && exit 0; "
            "echo \"$D\" | grep -E 'ResumedActivity|topResumedActivity|Resumed:' | grep -qF " +
            nanolaunch::shellQuote(" " + pkg + "/") + " && exit 0; "
            "exit 1";
        const int rc = system(cmd.c_str());
        if (self->mPanelAppGen.load(std::memory_order_acquire) == gen) {
            if (rc == 0) {
                self->mPanelAppGoneStreak.store(0, std::memory_order_release);
            } else if (self->mPanelAppGoneStreak.fetch_add(1, std::memory_order_acq_rel) + 1 >= 3) {
                self->mPanelAppGoneGen.store(gen, std::memory_order_release);   // that launch's app only
            }
        }
        self->mPanelPollBusy.store(false, std::memory_order_release);
    }).detach();
}

// pollInput while the panel's app has the controller: Nano drops every event but the Anbernic button,
// whose release brings the controller back and opens the Quick Menu (Anbernic + the Switch Screen
// Button brings it back alone: PhoneWindowManager, screenSwapPoll). True while it has it.
bool NanoMenu::panelAppInputGate() {
    if (!mPanelAppFocused) return false;
    bool fnReleased = false;
    struct input_event ev;
    for (int fd : mInputFds) {
        if (fd < 0) continue;
        nanoabxy::forget(fd);
        while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type != EV_SYN) mLastInputMs = android::uptimeMillis();   // in use: no idle sleep
            if (ev.type != EV_KEY) continue;
            if (!(devkeys::isFunctionCode(ev.code) || ev.code == KEY_HOMEPAGE)) {
                if (ev.value == 1) mPanelFnDown = false;   // a chord (Anbernic + Volume ...), not the button alone
                continue;
            }
            if (ev.value == 1) mPanelFnDown = true;
            else if (ev.value == 0 && mPanelFnDown) fnReleased = true;
        }
    }
    touchResync(-1, true);
    if (!fnReleased) return true;
    panelAppFocus(false);
    if (mPs3Xmb && !quickMenuOpen()) toggleQuickMenu();
    return false;
}

void NanoMenu::releaseSecondarySurfaces() {
    if (mSecondaryEglSurfaces.empty() && mSecondaryWallpaperControls.empty()) return;
    eglMakeCurrent(mDisplay, mSurface, mSurface, mContext);
    for (EGLSurface es : mSecondaryEglSurfaces) eglDestroySurface(mDisplay, es);
    mSecondaryEglSurfaces.clear();
    mSecondarySurfaces.clear();
    if (!mSecondaryWallpaperControls.empty()) {
        SurfaceComposerClient::Transaction t;
        for (const auto& sc : mSecondaryWallpaperControls) if (sc != nullptr) t.reparent(sc, nullptr);
        t.apply();
        mSecondaryWallpaperControls.clear();
    }
    mSecondaryDisplayTokens.clear();
    mSecondaryAppliedLayerStacks.clear();
    mSecondaryCreatedSize.clear();
    mSecondaryAppliedLssH.clear();
    mNdsSecondaryShown = false;
    mPanelLayerHidden = false;
    mPanelRevealPending = false;
}

void NanoMenu::orientationTick() {
    // One-shot: seed auto-rotation from the persisted Screen Orientation - "auto" ->
    // sensor ON, any fixed orientation -> OFF (nano forces it via mapOrientationRequest,
    // which only engages when accelerometer_rotation is 0). The device may ship with a
    // different value. Home process only, guarded by a persist prop so it runs once and
    // never fights a later change. Detached thread that retries until the write takes
    // (the settings provider may not be ready on the first tick).
    if (!mOverlayMode &&
        !property_get_bool("persist.gammaos.nano.accel_seeded", false)) {
        property_set("persist.gammaos.nano.accel_seeded", "1");
        std::thread([]() {
            nanoThreadNormalPriority();
            char ob[PROPERTY_VALUE_MAX] = {};
            property_get("persist.gammaos.nano.orientation", ob, "landscape");
            const char want = (std::string(ob) == "auto") ? '1' : '0';
            for (int i = 0; i < 20; i++) {
                char cmd[96];
                snprintf(cmd, sizeof(cmd),
                         "settings put system accelerometer_rotation %c 2>/dev/null", want);
                (void)system(cmd);
                char b[32] = {};
                FILE* f = popen("settings get system accelerometer_rotation 2>/dev/null", "r");
                if (f) { if (fgets(b, sizeof(b), f)) {} pclose(f); }
                if (b[0] == want) break;
                usleep(500000);
            }
        }).detach();
    }

    // One-shot: clear a stale user_rotation left by the previous freeze-based control.
    // Mechanism A never writes user_rotation, but a device upgrading from the old build
    // can carry mUserRotation=270, which would leak into any "none"/UNSPECIFIED path (an
    // app that itself requests no orientation would inherit portrait). Reset to 0 once so
    // natural landscape is the baseline. Own guard prop so it runs once even on devices
    // that already seeded accel, and never fights a legitimate later rotation.
    if (!mOverlayMode &&
        !property_get_bool("persist.gammaos.nano.urot_reset", false)) {
        property_set("persist.gammaos.nano.urot_reset", "1");
        std::thread([]() {
            for (int i = 0; i < 20; i++) {
                (void)system("settings put system user_rotation 0 2>/dev/null");
                char b[32] = {};
                FILE* f = popen("settings get system user_rotation 2>/dev/null", "r");
                if (f) { if (fgets(b, sizeof(b), f)) {} pclose(f); }
                if (b[0] == '0') break;
                usleep(500000);
            }
        }).detach();
    }

    char nb[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.orientation", nb, "landscape");
    std::string nanoSetting = nb[0] ? std::string(nb) : std::string("landscape");

    // Foreground context. nano is what the user sees when it holds the panel as
    // the DRM-direct home (drm_active) or when the overlay is raised over an app
    // (show_overlay). Otherwise an app owns the SurfaceFlinger display: resolve
    // the top resumed package (empty = nano SF home with no app). app_launched is
    // NOT used here - it is unreliable (stays 0/stale across the overlay handoff).
    const bool overlayShown = property_get_bool("sys.gammaos.nano.show_overlay", false);
    const bool drmActive    = property_get_bool("sys.gammaos.nano.drm_active", false);

    // Global force-orientation mode. When persist.gammaos.nano.orient_mode is a force_*
    // value, EVERY foreground app is forced to that orientation and cannot override it -
    // this sits ABOVE the per-app override and the honor-the-app "none" default. The nano
    // menu / overlay itself keeps following its own Screen Orientation (nanoSetting), an
    // independent axis (forcing the XMB home to portrait is the deferred DRM AHB item).
    // Note: an app that cannot render the forced orientation (e.g. a landscape-locked game
    // asked for portrait) will still black-screen - that is inherent to the app, not this.
    char mb[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.orient_mode", mb, "normal");
    std::string mode = mb[0] ? std::string(mb) : std::string("normal");
    std::string forced;
    if      (mode == "force_landscape")     forced = "landscape";
    else if (mode == "force_portrait")      forced = "portrait";
    else if (mode == "force_rev_landscape") forced = "rev_landscape";
    else if (mode == "force_rev_portrait")  forced = "rev_portrait";

    std::string token;
    if (overlayShown || drmActive) {
        token = nanoSetting;                     // nano menu / overlay is foreground
    } else if (!forced.empty()) {
        token = forced;                          // force mode overrides the app entirely
    } else {
        // An app owns SurfaceFlinger. Identify it by the package nano launched -
        // reliable and cheap, unlike the dumpsys foreground-resolver popen which
        // returns empty from the parked overlay's render thread. Empty = fall back
        // to nano's own setting.
        char pb[PROPERTY_VALUE_MAX] = {};
        property_get("sys.gammaos.nano.launched_pkg", pb, "");
        std::string fg = pb;
        if (fg.empty()) {
            token = nanoSetting;
        } else {
            std::string ov = appOrientGet(fg);   // per-app override, else honor the app
            token = ov.empty() ? std::string("none") : ov;
        }
    }

    if (token != mLastOrientToken) {
        mLastOrientToken = token;
        property_set("sys.gammaos.nano.force_orientation", token.c_str());
        ALOGI("nano: force_orientation=%s (overlay=%d drm_active=%d)",
              token.c_str(), overlayShown ? 1 : 0, drmActive ? 1 : 0);
        // Screen Orientation is the single owner of accelerometer_rotation. Resolve it
        // from the RESOLVED token (not just the nano setting) so a per-app "auto"
        // override - which is applied via appOrientSet and bypasses the settings write
        // path - also enables the sensor, and any fixed/none token disables it so the
        // nano force engages. Only shell out when the bit actually changes ("settings
        // put" forks the settings CLI); token changes are infrequent (fg transitions).
        static int sLastAccel = -1;
        const int wantAccel = (token == "auto") ? 1 : 0;
        if (wantAccel != sLastAccel) {
            sLastAccel = wantAccel;
            std::thread([wantAccel]() {
                nanoThreadNormalPriority();
                char cmd[96];
                snprintf(cmd, sizeof(cmd),
                         "settings put system accelerometer_rotation %d 2>/dev/null", wantAccel);
                (void)system(cmd);
            }).detach();
        }
    }
}

} // namespace android
