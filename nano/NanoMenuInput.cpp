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

// Input device discovery + hotplug, event polling, and the D-pad/face-button
// dispatch table used by the non-XMB menu. XMB-specific input
// (handleLeft/handleRight, OSK handling) lives in NanoMenuXmb.cpp.
//
// Extracted from NanoMenu.cpp — behavior unchanged.

#define LOG_TAG "GammaOSNano"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <inttypes.h>
#include <cinttypes>
#include <math.h>
#include <string.h>
#include <errno.h>

#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

#include <android-base/properties.h>
#include <cutils/properties.h>
#include <sys/system_properties.h>   // prop_info, __system_property_find
// Exported by libc but declared only in the internal <sys/_system_properties.h>;
// used to detect the test-nav prop changing via its serial (cheap pointer-deref)
// instead of a full name lookup every pollInput().
extern "C" uint32_t __system_property_serial(const prop_info* __pi);
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include "NanoBacklight.h"
#include "NanoDeviceKeys.h"
#include "NanoMenu.h"
#include "NanoScreenCal.h"
#include "NanoMenuSettingsTree.h"
#include "NanoMenuDrm.h"
#include "NanoMenuShaders.h"
#include "NanoMenuUtils.h"
#include "NanoMenuStrings.h"   // LOCALE_COUNT (DSi language picker touch)
#include "NanoAbxy.h"
#include "NanoI18n.h"

namespace android {

// ---------------------------------------------------------------------------
// Input device discovery + hotplug
// ---------------------------------------------------------------------------

void NanoMenu::openInputDevices() {
    DIR* dir = opendir("/dev/input");
    if (!dir) { ALOGE("Cannot open /dev/input"); return; }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (mOpenedDevices.count(entry->d_name)) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        nanoabxy::forget(fd);
        if (fd >= 0) {
            // Exclusive grab: prevent Android InputReader from stealing events.
            // Gated by property — disable when preload is off to avoid input
            // ownership issues during app transitions.
            if (android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                    ALOGW("EVIOCGRAB failed for %s: %s", path, strerror(errno));
                }
                ALOGD("Opened + grabbed input device: %s", path);
            } else {
                ALOGD("Opened input device (no grab): %s", path);
            }
            mInputFds.push_back(fd);
            mOpenedDevices.insert(entry->d_name);
            // Cache the device name per-fd so the Control Center can route the BOTTOM touch
            // digitizer (RG DS: gt9xx-0 = secondary/bottom panel) distinctly from the top.
            char nm[128] = {0};
            if (ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm) >= 0) mInputFdNames[fd] = nm;
        }
    }
    closedir(dir);

    // Set up inotify to detect hotplugged and replaced input devices.
    // IN_DELETE is needed because gammapad may destroy+recreate device nodes
    // to seize them; we must detect the deletion, drop our stale fd, and
    // re-open+grab when the replacement IN_CREATE arrives.
    mInotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (mInotifyFd >= 0) {
        inotify_add_watch(mInotifyFd, "/dev/input", IN_CREATE | IN_DELETE);
        ALOGD("Watching /dev/input for hotplug");
    }
}

void NanoMenu::checkInputHotplug() {
    if (mInotifyFd < 0) return;
    char buf[512] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(mInotifyFd, buf, sizeof(buf));
    if (len > 0) {
        for (char* ptr = buf; ptr < buf + len; ) {
            auto* ev = reinterpret_cast<struct inotify_event*>(ptr);
            if (ev->len > 0 && strncmp(ev->name, "event", 5) == 0) {
                if (ev->mask & IN_DELETE) {
                    // Device node was removed (gammapad hides+recreates devices).
                    // Close our stale fd and forget it so we re-grab on IN_CREATE.
                    //
                    // BUG fix: only match the fd whose path corresponds to the
                    // deleted device. The previous code OR'd against any
                    // "(deleted)" fd, which closed the wrong fd when multiple
                    // devices were being torn down nearly simultaneously
                    // (gammapad's swap of event12 races with EventHub removing
                    // event10, etc). The result was that nano permanently lost
                    // the Xbox controller because event12's fd got closed by
                    // event10's IN_DELETE event, leaving stale state in
                    // mOpenedDevices that suppressed IN_CREATE re-opening.
                    if (mOpenedDevices.count(ev->name)) {
                        char path[PATH_MAX];
                        snprintf(path, sizeof(path), "/dev/input/%s", ev->name);
                        for (auto it = mInputFds.begin(); it != mInputFds.end(); ++it) {
                            char fdPath[PATH_MAX];
                            char procLink[64];
                            snprintf(procLink, sizeof(procLink), "/proc/self/fd/%d", *it);
                            ssize_t rl = readlink(procLink, fdPath, sizeof(fdPath) - 1);
                            if (rl > 0) {
                                fdPath[rl] = '\0';
                                // Match only by exact device name. The path may
                                // also have " (deleted)" appended once the kernel
                                // marks it removed; tolerate that suffix.
                                char wantPath[PATH_MAX];
                                snprintf(wantPath, sizeof(wantPath), "/dev/input/%s", ev->name);
                                size_t wantLen = strlen(wantPath);
                                if (strncmp(fdPath, wantPath, wantLen) == 0 &&
                                        (fdPath[wantLen] == '\0' ||
                                         fdPath[wantLen] == ' ')) {
                                    inputEventsLost(*it);
                                    ioctl(*it, EVIOCGRAB, 0);
                                    mInputFdNames.erase(*it);   // drop the fd->name entry so CC pollers
                                                                // (key / top / bottom digitizer) never match
                                                                // a stale closed fd after a device re-seize
                                    touchPanelGone(*it);   // its fd number may come back
                                    mPollSkipFd.erase(*it);
                                    close(*it);
                                    mInputFds.erase(it);
                                    break;
                                }
                            }
                        }
                        mOpenedDevices.erase(ev->name);
                        ALOGI("Device removed, dropped stale fd: %s", path);
                    }
                } else if ((ev->mask & IN_CREATE) && !mOpenedDevices.count(ev->name)) {
                    // Small delay for the device node to be fully ready
                    usleep(100000); // 100ms
                    char path[PATH_MAX];
                    snprintf(path, sizeof(path), "/dev/input/%s", ev->name);
                    int fd = open(path, O_RDONLY | O_NONBLOCK);
                    nanoabxy::forget(fd);
                    if (fd >= 0) {
                        // Home-mode grab only (persist.gammaos.nano.grab_input).
                        // The overlay does NOT grab: it isolates the app's input
                        // via the framework drop_input prop, not EVIOCGRAB.
                        if (android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                            if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                                ALOGW("EVIOCGRAB failed for hotplugged %s: %s", path, strerror(errno));
                            }
                        }
                        mInputFds.push_back(fd);
                        mOpenedDevices.insert(ev->name);
                        char nm[128] = {0};   // re-cache the device name so CC pollers keep routing by name
                        if (ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm) >= 0) mInputFdNames[fd] = nm;
                        mTouchPanels.erase(fd);   // a new device: its panel is read on its first touch event
                        ALOGI("Hotplugged + grabbed input device: %s", path);
                    }
                }
            }
            ptr += sizeof(struct inotify_event) + ev->len;
        }
    }

    // GammaOS: defensive sweep for missed devices. The inotify handler above
    // can lose track of devices when multiple are torn down/recreated nearly
    // simultaneously (typical with gammapad swaps). Periodically rescan
    // /dev/input and reconcile against mOpenedDevices: open anything missing,
    // and drop any fd that has gone "(deleted)" without a matching IN_DELETE.
    // Ratelimited to once per second to keep cost negligible.
    static int64_t sLastSweepNs = 0;
    int64_t nowNs = systemTime(SYSTEM_TIME_MONOTONIC);
    if (nowNs - sLastSweepNs < 1000000000LL) return;
    sLastSweepNs = nowNs;

    // (1) Drop any of our fds that point to a deleted inode. This handles
    // the case where gammapad recreated a device under the same name without
    // us seeing the IN_DELETE.
    for (auto it = mInputFds.begin(); it != mInputFds.end(); ) {
        char fdPath[PATH_MAX];
        char procLink[64];
        snprintf(procLink, sizeof(procLink), "/proc/self/fd/%d", *it);
        ssize_t rl = readlink(procLink, fdPath, sizeof(fdPath) - 1);
        if (rl > 0) {
            fdPath[rl] = '\0';
            if (strstr(fdPath, "(deleted)")) {
                // Recover the device name from the path so we can also
                // erase it from mOpenedDevices and let the rescan re-open it.
                const char* base = strrchr(fdPath, '/');
                if (base) {
                    base++;
                    char nameOnly[64];
                    size_t i = 0;
                    while (base[i] && base[i] != ' ' && i < sizeof(nameOnly) - 1) {
                        nameOnly[i] = base[i];
                        i++;
                    }
                    nameOnly[i] = '\0';
                    mOpenedDevices.erase(nameOnly);
                    ALOGI("Sweep: dropped stale fd %s", fdPath);
                }
                inputEventsLost(*it);
                ioctl(*it, EVIOCGRAB, 0);
                mInputFdNames.erase(*it);   // keep the fd->name map free of stale closed fds (CC pollers)
                touchPanelGone(*it);   // its fd number may come back
                mPollSkipFd.erase(*it);
                close(*it);
                it = mInputFds.erase(it);
                continue;
            }
        }
        ++it;
    }

    // (2) Open any /dev/input/event* that we don't currently have.
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        if (mOpenedDevices.count(entry->d_name)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        nanoabxy::forget(fd);
        if (fd >= 0) {
            // Home-mode grab only; the overlay isolates via drop_input, not grab.
            if (android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                    ALOGW("EVIOCGRAB failed for swept %s: %s", path, strerror(errno));
                }
            }
            mInputFds.push_back(fd);
            mOpenedDevices.insert(entry->d_name);
            char nm[128] = {0};   // re-cache the device name so CC pollers keep routing by name
            if (ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm) >= 0) mInputFdNames[fd] = nm;
            mTouchPanels.erase(fd);   // a new device: its panel is read on its first touch event
            ALOGI("Sweep: opened previously-missed input device %s", path);
        }
    }
    closedir(dir);
}

// ---------------------------------------------------------------------------
// Menu navigation: Back / Select / Up / Down
// (Left/Right live in NanoMenuXmb.cpp because they drive the XMB system axis.)
// ---------------------------------------------------------------------------

void NanoMenu::handleBack() {
    if (esdeOverlayKey(BTN_EAST)) return;   // closes the ES-DE media viewer / game information
    if (mBiosDlgActive) { biosDlgBack(); return; }   // Game Settings BIOS dialog
    // Live controller Test / Calibration screen: back closes it and returns to
    // the Calibrate & Test list (does not pop the menu behind).
    if (mGpTestActive || mGpCalibActive || mCpActive) {
        mGpTestActive = false; mGpCalibActive = false;
        mCpActive = false; mCpBinding = nullptr; mGpSelectDownMs = 0;
        mDisplayDirty = true;
        return;
    }
    // GammaOS Nano: back cancels any queued launch the user armed
    // before the system was ready. Without this, mLaunchPending
    // would still re-fire handleSelect() once isLaunchReady() flips.
    cancelPendingLaunch();
    // Boxart scraper modal: a scrape carries on in the background (Square cancels it), a folder
    // copy or move is cancelled, and a finished summary closes.
    if (mScrapeProgActive) {
        if (mScrapeRunning && mBgScrapeRun.empty()) scraperCancel();
        else { mScrapeProgActive = false; mScrapeBusyNote = false; mDisplayDirty = true; }
        return;
    }
    if (mOskActive) {
        closeOsk();
        return;
    }
    if (mWifiManageActive) { wifiManageClose(); return; }
    if (mMenuState == MENU_WIFI) {
        closeWifiScreen();
        if (mSetupWizardActive) mMenuState = MENU_SETUP_WIZARD;
        return;
    }
    if (mMenuState == MENU_BT) {
        closeBtScreen();
        if (mSetupWizardActive) mMenuState = MENU_SETUP_WIZARD;
        return;
    }
    if (mMenuState == MENU_SETTINGS) { handleSettingsTreeBack(); return; }
    // Mirror handleSelect: route the setup-wizard WiFi/BT step's Back to
    // ps3XmbBack -> wizBack (mPs3Xmb is false during the setup wizard).
    if (mPs3Xmb || mPs3WizActive) {
        // Back out of the last level of a screen opened from the ES-DE menu: the menu again.
        const bool esdeReturn = mEsdeMenuReturn && !mPs3Stack.empty();
        // DSi carousel / Minima list: B walks up one level (pop submenu / leave the category).
        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && !ndsInModal()) ndsNavBack();
        else if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuBack();
        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) {
            // ES-DE Back walks up one level: gamelist -> system view inside esdeBack(). At the
            // system root esdeBack() returns false ("nothing left to go back to"); in overlay mode
            // that must dismiss the overlay and resume the running app, exactly like the XMB
            // (ps3XmbBack -> overlayResume) and DSi/Minima top-level Back. Without this, Back was a
            // dead key over a running app in the ES-DE theme and the overlay could not be closed.
            if (!esdeBack() && mOverlayMode) overlayResume();
        }
        else ps3XmbBack();   // native XMB submenu drilled in (mPs3Stack): pop it, e.g. the power-hold Quick Menu
        if (esdeReturn && mPs3Stack.empty()) esdeMenuReopen();
        return;
    }
    if (mXmbMode) {
        if (mSearchActive) {
            mSearchActive = false;
            mOskQuery.clear();
            mSearchResults.clear();
        } else {
            // Exit XMB mode back to text menu
            mXmbMode = false;
            property_set("persist.gammaos.nano.xmb_mode", "0");
            mMenuState = MENU_MAIN;
            mDisplayDirty = true;
        }
        return;
    }
    if (mMenuState == MENU_RECENT) {
        mMenuState = MENU_MAIN;
        mRecentSelectedIndex = 0;
        mMenuScrollTop = 0;
        mDisplayDirty = true;
    } else if (mMenuState == MENU_APPS) {
        mMenuState = MENU_MAIN;
        mAppSelectedIndex = 0;
        mMenuScrollTop = 0;
        mDisplayDirty = true;
    }
}

void NanoMenu::handleSelect() {
    ALOGI("NanoMenu: select (ps3=%d esde=%d esdeMenu=%d stack=%zu xs=%d gamelist=%d clock=%d scrape=%d osk=%d wiz=%d menu=%d)",
          mPs3Xmb, mEsdeTheme, mEsdeMenuActive, mPs3Stack.size(), mEsdeXsActive, mEsdeInGamelist,
          mPspClockOn, mScrapeProgActive, mOskActive, mPs3WizActive, (int)mMenuState);
    if (esdeOverlayKey(BTN_SOUTH)) return;   // closes the ES-DE media viewer / game information
    if (mBiosDlgActive) { biosDlgSelect(); return; }   // Game Settings BIOS dialog
    // Clock is up: block XMB/menu selection (launch/open) behind it - the clock owns the screen.
    if (mPspClockOn || mPspClockReveal > 0.0f) return;
    // Boxart scraper modal: X closes the summary once the scrape has finished.
    if (mScrapeProgActive) {
        if (!mScrapeRunning) { mScrapeProgActive = false; mScrapeBusyNote = false; mDisplayDirty = true; }
        return;
    }
    if (mOskActive) {
        if (!oskButtonLeavesTouch()) oskAPress();
        return;
    }
    if (mWifiManageActive)           { wifiManageActivate();       return; }
    if (mMenuState == MENU_WIFI)     { handleWifiScreenSelect();   return; }
    if (mMenuState == MENU_BT)       { handleBtScreenSelect();     return; }
    if (mMenuState == MENU_SETTINGS) { handleSettingsTreeSelect();  return; }
    // The PS3 net/BT wizard (mPs3WizActive) runs during the setup wizard's WiFi/
    // Bluetooth steps with mPs3Xmb=false (home XMB not up yet). Route its confirm
    // to ps3XmbSelect -> wizConfirm; otherwise it falls through to the legacy text
    // menu below and "selects" item 0 (RetroArch), wrongly launching it.
    if (mPs3Xmb || mPs3WizActive) {
        // DSi carousel: A enters/drills the focused card (buttons navigate only; launch is touch-only).
        // Minima list: A OPENS the row - enters a category / drills a submenu / LAUNCHES a leaf, matching
        // NextUI's "A OPEN" paradigm (a discrete button press, so stick drift is not a concern here).
        if (mNdsTheme && mPs3Xmb && !ndsInModal()) ndsNavSelect(false);
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal()) ndsNavSelect(true);
        else if (mEsdeTheme && mEsdeMenuActive) esdeMenuSelect();
        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeSelect();
        else ps3XmbSelect();   // native XMB submenu drilled in (mPs3Stack): confirm the focused row
        return;
    }
    if (mXmbMode) {
        if (isOnSettingsColumn()) {
            openSettingsTree();
            return;
        }
        launchXmbGame();
        return;
    }
    if (mMenuState == MENU_RECENT) {
        int numEntries = (int)mRecentEntries.size();
        // Last item is "< Back"
        if (mRecentSelectedIndex >= numEntries) {
            handleBack();
            return;
        }
        // GammaOS Nano: gate launches until the system can actually
        // accept them. See NanoMenuSystem.cpp / isLaunchReady().
        if (!isLaunchReady()) {
            ALOGI("NanoMenu: recent launch deferred -- boot not ready");
            showLaunchBusyToast();
            return;
        }
        // Launch the selected game directly into RetroArch
        const auto& entry = mRecentEntries[mRecentSelectedIndex];
        ALOGI("NanoMenu: launching game: %s core: %s",
              entry.romPath.c_str(), entry.corePath.c_str());
        setLaunchRomPath(entry.romPath);
        setLaunchGame("", entry.romPath, entry.corePath);
        android::base::SetProperty("sys.gammaos.nano.launch_core", entry.corePath);
        // Track launched package so NanoMenu can force-stop it on next restart
        {
            char launchApp[PROPERTY_VALUE_MAX] = {};
            property_get("sys.gammaos.nano.launch_app", launchApp, "com.retroarch.aarch64");
            android::base::SetProperty("sys.gammaos.nano.launched_pkg", launchApp);
        }
        // Trigger DE cache populate (ROM first, then delta sync everything)
        property_set("sys.gammaos.nano.cache_ready", "0");
        property_set("sys.gammaos.nano.cache_op", "populate");
        // Prime Quick Resume now — the persist write has time to flush to disk
        // while the game runs. ShutdownThread may update ROM/core from the
        // playlist if the user loaded a different game, but this ensures the
        // flag survives even if the reboot races the persist write.
        if (mQuickResumeEnabled) {
            setQrRomPath(entry.romPath);
            android::base::SetProperty("persist.gammaos.nano.qr_core", entry.corePath);
            property_set("persist.gammaos.nano.qr_prepared", "1");
        }
        // Flag so next nano menu restart returns to Recently Played
        property_set("sys.gammaos.nano.return_recent", "1");
        armAppLaunchTrigger();
        // Tell InputDispatcher to drop events immediately — prevents a fast
        // double-press A from queuing a second event before the transition.
        property_set("sys.gammaos.nano.drop_input", "1");
        // Set a timestamp fence — InputDispatcher drops any events with
        // eventTime <= this value, covering the race where the A-DOWN was
        // queued before drop_input was set.
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t fenceNs = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        char buf[32];
        snprintf(buf, sizeof(buf), "%" PRId64, fenceNs);
        property_set("sys.gammaos.nano.drop_fence_ns", buf);
        // Don't exit yet — wait for the select key to be released so the
        // key-up event passes through Android's InputReader before RetroArch
        // gets focus. Otherwise the A press leaks to RetroArch as a phantom input.
        mWaitForRelease = true;
        return;
    }

    if (mMenuState == MENU_APPS) {
        int numApps = (int)mAppEntries.size();
        // Last item is "< Back"
        if (mAppSelectedIndex >= numApps) {
            handleBack();
            return;
        }
        // GammaOS Nano: gate -- see MENU_RECENT branch above.
        if (!isLaunchReady()) {
            ALOGI("NanoMenu: app launch deferred -- boot not ready");
            showLaunchBusyToast();
            return;
        }
        // Launch the selected app
        const auto& app = mAppEntries[mAppSelectedIndex];
        ALOGI("NanoMenu: launching app: %s", app.packageName.c_str());
        android::base::SetProperty("sys.gammaos.nano.launch_app", app.packageName);
        // Track launched package so NanoMenu can force-stop it on next restart
        android::base::SetProperty("sys.gammaos.nano.launched_pkg", app.packageName);
        // Clear any ROM/core properties so RootWindowContainer uses generic launch
        setLaunchRomPath("");
        android::base::SetProperty("sys.gammaos.nano.launch_core", "");
        // Clear any stale Quick Resume priming: the user is launching a
        // non-drastic/non-retroarch app, so a leftover qr_prepared=1 from an
        // earlier game would otherwise auto-resume that game on the next
        // nano start instead of returning the user to Applications.
        property_set("persist.gammaos.nano.qr_prepared", "0");
        android::base::SetProperty("persist.gammaos.nano.qr_core", "");
        // Flag so next nano menu restart returns to Applications
        property_set("sys.gammaos.nano.return_apps", "1");
        armAppLaunchTrigger();
        property_set("sys.gammaos.nano.drop_input", "1");
        mWaitForRelease = true;
        return;
    }

    // Main menu - dispatch by index (order matches buildMenu)
    // 0=RetroArch 1=RecentlyPlayed 2=Applications 3=BootAndroid
    // 4=Recovery 5=SafeMode 6=Reboot 7=PowerOff
    ALOGD("Select item %d: %s", mSelectedIndex, mMenuItems[mSelectedIndex].label.c_str());
    if (mSelectedIndex == 0) { // RetroArch
        // GammaOS Nano: gate -- see MENU_RECENT branch above.
        if (!isLaunchReady()) {
            ALOGI("NanoMenu: RetroArch launch deferred -- boot not ready");
            showLaunchBusyToast();
            return;
        }
        armAppLaunchTrigger("com.retroarch.aarch64");
        property_set("sys.gammaos.nano.drop_input", "1");
        mWaitForRelease = true;
    } else if (mSelectedIndex == 1) { // Recently Played
        if (!mStorageReady) return; // greyed out, ignore
        // Load playlist from RetroArch's content_history.lpl (needs CE unlock)
        loadRecentPlaylist();
        mMenuState = MENU_RECENT;
        mRecentSelectedIndex = 0;
        mMenuScrollTop = 0;
        mDisplayDirty = true;
    } else if (mSelectedIndex == 2) { // Applications
        if (!mStorageReady) return; // greyed out, ignore
        loadInstalledApps();
        mMenuState = MENU_APPS;
        mAppSelectedIndex = 0;
        mMenuScrollTop = 0;
        mDisplayDirty = true;
    } else if (mSelectedIndex == 3) { // Boot Android
        // Full Android needs a clean boot.  Dispatch via nano_action so
        // init (which has powerctl_prop access) handles the reboot.
        // Clear QR priming: the user is leaving nano for full Android,
        // not resuming a game.
        property_set("persist.gammaos.nano.qr_prepared", "0");
        android::base::SetProperty("persist.gammaos.nano.qr_core", "");
        property_set("service.bootanim.nano_action", "android");
    } else if (mSelectedIndex == 4) { // Recovery Mode
        property_set("persist.gammaos.nano.qr_prepared", "0");
        android::base::SetProperty("persist.gammaos.nano.qr_core", "");
        property_set("service.bootanim.nano_action", "recovery");
    } else if (mSelectedIndex == 5) { // Safe Mode
        property_set("persist.gammaos.nano.qr_prepared", "0");
        android::base::SetProperty("persist.gammaos.nano.qr_core", "");
        property_set("service.bootanim.nano_action", "safemode");
    } else if (mSelectedIndex == 6) { // Reboot
        prepareShutdown("reboot");
    } else if (mSelectedIndex == 7) { // Power Off
        prepareShutdown("shutdown");
    }
}

void NanoMenu::handleUp() {
    if (biosDlgShown()) { if (!mSetupTransitioning || !mBiosDlgSetup) biosDlgNav(-1); return; }   // BIOS dialog
    if (esdeOverlayNav(NavDir::Up)) return;   // ES-DE media viewer / game information
    if (mScrapeProgActive) return;   // modal swallows navigation
    // GammaOS Nano: navigating cancels any queued launch.
    cancelPendingLaunch();
    if (mSetupWizardActive && mMenuState == MENU_SETUP_WIZARD && !mPs3WizActive) {
        handleSetupUp(); return;
    }
    if (mOskActive) {
        if (!oskButtonLeavesTouch()) oskMoveCursor(NavDir::Up);
        return;
    }
    if (mWifiManageActive)           { wifiManageMove(-1);       return; }
    if (mMenuState == MENU_WIFI)     { handleWifiScreenUp();     return; }
    if (mMenuState == MENU_BT)       { handleBtScreenUp();       return; }
    if (mMenuState == MENU_SETTINGS) { handleSettingsTreeUp();    return; }
    if (mPs3Xmb || mPs3WizActive) {
        // DSi stacked carousel: UP walks up to the parent level (the parent carousel is drawn
        // above the focused one). A modal keeps normal vertical list nav.
        // A settings LIST level moves the selection up a row; a carousel level walks up to the parent.
        if (mNdsTheme && mPs3Xmb && !ndsInModal()) { if (ndsCurLevelIsList()) ndsNavHoriz(-1); else ndsNavBack(); }
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal()) ndsNavHoriz(-1);   // Minima: flat list, UP moves the selection up a row
        else if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuMove(-1);  // ES-DE menu row up
        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeNav(0, -1);       // ES-DE: previous system / game
        else ps3XmbUp();   // native XMB submenu drilled in (mPs3Stack): move selection up
        return;
    }
    if (mXmbMode) {
        if (mSearchActive) {
            if (mSearchSelectedIndex > 0) mSearchSelectedIndex--;
        } else if (isOnSettingsColumn()) {
            if (mSettingsSelectedIndex > 0) mSettingsSelectedIndex--;
            mDisplayDirty = true;
        } else {
            if (mXmbGameIndex > 0) mXmbGameIndex--;
        }
        return;
    }
    if (mMenuState == MENU_RECENT) {
        if (mRecentSelectedIndex > 0) mRecentSelectedIndex--;
    } else if (mMenuState == MENU_APPS) {
        if (mAppSelectedIndex > 0) mAppSelectedIndex--;
    } else {
        if (mSelectedIndex > 0) {
            mSelectedIndex--;
            // Skip greyed-out items when storage isn't ready
            if (!mStorageReady && mSelectedIndex < (int)mMenuItems.size()) {
                const auto& lbl = mMenuItems[mSelectedIndex].label;
                if ((mSelectedIndex == 1 || mSelectedIndex == 2)
                    && mSelectedIndex > 0) {
                    mSelectedIndex--;
                    // Check again for the other greyed item
                    if (!mStorageReady && mSelectedIndex < (int)mMenuItems.size()) {
                        const auto& lbl2 = mMenuItems[mSelectedIndex].label;
                        if ((mSelectedIndex == 1 || mSelectedIndex == 2)
                            && mSelectedIndex > 0) {
                            mSelectedIndex--;
                        }
                    }
                }
            }
        }
    }
}

void NanoMenu::handleDown() {
    if (biosDlgShown()) { if (!mSetupTransitioning || !mBiosDlgSetup) biosDlgNav(1); return; }   // BIOS dialog
    if (esdeOverlayNav(NavDir::Down)) return;   // ES-DE media viewer / game information
    if (mScrapeProgActive) return;   // modal swallows navigation
    // GammaOS Nano: navigating cancels any queued launch.
    cancelPendingLaunch();
    if (mSetupWizardActive && mMenuState == MENU_SETUP_WIZARD && !mPs3WizActive) {
        handleSetupDown(); return;
    }
    if (mOskActive) {
        if (!oskButtonLeavesTouch()) oskMoveCursor(NavDir::Down);
        return;
    }
    if (mWifiManageActive)           { wifiManageMove(+1);         return; }
    if (mMenuState == MENU_WIFI)     { handleWifiScreenDown();     return; }
    if (mMenuState == MENU_BT)       { handleBtScreenDown();       return; }
    if (mMenuState == MENU_SETTINGS) { handleSettingsTreeDown();    return; }
    if (mPs3Xmb || mPs3WizActive) {
        // DSi: a settings LIST level moves the selection down a row. Otherwise DOWN enters
        // the focused card: at the categories root it opens the selected category; deeper, a
        // stacked carousel card is drilled ONLY when it opens a submenu (the same ">" chevron
        // rows). A leaf card is NOT confirmed/launched on DOWN (launch is touch-only anyway),
        // so a drifting stick / temperamental d-pad diagonal cannot activate a selection.
        if (mNdsTheme && mPs3Xmb && !ndsInModal()) {
            if (ndsCurLevelIsList()) ndsNavHoriz(+1);
            else if (mNdsAtRoot || ps3FocusOpensSubmenu()) ndsNavSelect(false);
        }
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal()) ndsNavHoriz(+1);   // Minima: flat list, DOWN moves the selection down a row
        else if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuMove(+1);  // ES-DE menu row down
        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeNav(0, +1);       // ES-DE: next system / game
        else ps3XmbDown();   // native XMB submenu drilled in (mPs3Stack): move selection down
        return;
    }
    if (mXmbMode) {
        if (mSearchActive) {
            int maxIdx = (int)mSearchResults.size() - 1;
            if (mSearchSelectedIndex < maxIdx) mSearchSelectedIndex++;
        } else if (isOnSettingsColumn()) {
            int maxIdx = (int)mSettingsItems.size() - 1;
            if (mSettingsSelectedIndex < maxIdx) mSettingsSelectedIndex++;
            mDisplayDirty = true;
        } else if (mXmbSystemIndex == -1) {
            // Recently Played
            int maxIdx = (int)mXmbRecent.size() - 1;
            if (mXmbGameIndex < maxIdx) mXmbGameIndex++;
        } else {
            int selSys = mXmbSystemIndex;
            if (selSys >= 0 && selSys < (int)mXmbSystems.size()) {
                int maxIdx = (int)mXmbSystems[selSys].roms.size() - 1;
                if (mXmbGameIndex < maxIdx) mXmbGameIndex++;
            }
        }
        return;
    }
    if (mMenuState == MENU_RECENT) {
        int maxIdx = (int)mRecentEntries.size(); // "< Back" is at this index
        if (mRecentSelectedIndex < maxIdx) mRecentSelectedIndex++;
    } else if (mMenuState == MENU_APPS) {
        int maxIdx = (int)mAppEntries.size(); // "< Back" is at this index
        if (mAppSelectedIndex < maxIdx) mAppSelectedIndex++;
    } else {
        int last = (int)mMenuItems.size() - 1;
        if (mSelectedIndex < last) {
            mSelectedIndex++;
            // Skip greyed-out items when storage isn't ready
            if (!mStorageReady && mSelectedIndex < (int)mMenuItems.size()) {
                const auto& lbl = mMenuItems[mSelectedIndex].label;
                if ((mSelectedIndex == 1 || mSelectedIndex == 2)
                    && mSelectedIndex < last) {
                    mSelectedIndex++;
                    // Check again for the other greyed item
                    if (!mStorageReady && mSelectedIndex < (int)mMenuItems.size()) {
                        const auto& lbl2 = mMenuItems[mSelectedIndex].label;
                        if ((mSelectedIndex == 1 || mSelectedIndex == 2)
                            && mSelectedIndex < last) {
                            mSelectedIndex++;
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Hold-to-repeat navigation.
//
// Users expect that holding a dpad direction (or pushing a stick past the
// deadzone and keeping it there) will scroll long lists continuously, not
// just move a single step. We model this as an edge-triggered "press"
// that records which direction is held, plus a per-frame tick that fires
// the matching handle* function on an accelerating cadence.
//
// Cadence:
//   - Initial delay before the first repeat fires.
//   - Interval starts slow and shortens with each repeat, floored at a
//     minimum so the scroll stays controllable on very long lists.
//
// navPress() always fires the first handle* call synchronously so a tap
// (press then immediate release) still moves exactly one step.
// ---------------------------------------------------------------------------

// Match the PS3 firmware/web key-repeat (index.html INPUT_* constants): an
// initial delay, then a base interval that accelerates GEOMETRICALLY (divide by
// the accel multiplier each repeat) down to a floor. The geometric ramp gives a
// smooth increase in speed while holding (the linear decrement juddered).
static constexpr int64_t kNavInitialDelayMs    = 300;   // INPUT_INITIAL_DELAY_MS
static constexpr int64_t kNavSlowIntervalMs    = 200;   // INPUT_REPEAT_MS (base)
static constexpr int64_t kNavMinIntervalMs     = 50;    // INPUT_MIN_REPEAT_MS
static constexpr float   kNavAccelMult         = 1.4f;  // INPUT_ACCEL_MULT
// The DSi System Menu holds a FIXED auto-repeat cadence with NO acceleration
// (launcher.js holdNav/update: one step now, navRepeatDelay 15f = 250ms before the
// first repeat, then navRepeatCadence 9f = 150ms per step). Distinct from the PS3
// XMB firmware's accelerating schedule above; used while the DSi carousel is front.
static constexpr int64_t kNdsNavInitialDelayMs = 250;   // navRepeatDelay 15f @60fps
static constexpr int64_t kNdsNavCadenceMs      = 150;   // navRepeatCadence 9f @60fps

void NanoMenu::navPress(NavDir dir) {
    if (dir == NavDir::None) return;
    // The PSP clock owns the screen while it is up (any reveal): do NOT drive the XMB/menu behind
    // it. All nav sources (dpad, HAT, analog stick) route through here, so this one gate blocks the
    // lot. The slide/swivel trigger and Power are handled upstream; the swipe-to-dismiss is touch.
    if (mPspClockOn || mPspClockReveal > 0.0f) return;
    mPs3AutoScrollTarget = -1;   // any manual nav cancels an in-flight auto-scroll
    mLastInputMs = android::uptimeMillis();   // dpad/HAT/stick = user activity
    mLastPressMs = mLastInputMs;
    // Idempotent: if this direction is already the held one, don't re-fire.
    // Guards against duplicate events (e.g. HAT re-reporting same value)
    // from double-stepping the selection.
    if (dir == mNavHeldDir) return;
    mNavRepeatHold = false;
    // Fire the step immediately; a brief tap should always move one slot.
    switch (dir) {
    case NavDir::Up:    handleUp();    break;
    case NavDir::Down:  handleDown();  break;
    case NavDir::Left:  handleLeft();  break;
    case NavDir::Right: handleRight(); break;
    case NavDir::None:  break;
    }
    mNavHeldDir      = dir;
    mNavFd           = mCurrentInputFd;
    mNavHeldStartMs  = android::uptimeMillis();
    mNavLastRepeatMs = mNavHeldStartMs;
    mNavRepeatCount  = 0;
}

void NanoMenu::navRelease(NavDir dir) {
    // dir == None means "whichever direction is currently held" — used
    // by axis handlers that can't tell press-up from press-down on the
    // same axis without extra state.
    if (dir == NavDir::None || dir == mNavHeldDir) {
        mNavRepeatHold   = false;
        mNavHeldDir      = NavDir::None;
        mNavHeldStartMs  = 0;
        mNavLastRepeatMs = 0;
        mNavRepeatCount  = 0;
    }
}

void NanoMenu::tickNavRepeat() {
    if (mNavHeldDir == NavDir::None) return;
    // Suppress auto-repeat while waiting for the launch key to be released
    // (the launch path consumes events specially and shouldn't receive
    // synthetic scroll steps). OSK and WiFi/BT screens all dispatch
    // through the handle* functions, so they benefit from auto-repeat
    // just like the XMB list.
    if (mWaitForRelease || mNavRepeatHold) return;

    const int64_t now = android::uptimeMillis();

    // Firmware/web schedule (index.html processKeyRepeat): the first repeat waits
    // the full initial delay, the SECOND is a full slow step, and only THEN does
    // the geometric acceleration begin -> intervals 300, 200, 143, 102, 73, 52,
    // 50... This deliberate slow start is the "debounce" feel; using count (not
    // count-1) in the exponent skipped the 200ms step and burst to top speed a
    // step early.
    // The DSi TOP-LEVEL categories carousel (mPs3Xmb under the DSi theme, no OSK/media player in
    // front) keeps the DSi's fixed 250ms/150ms cadence, matching launcher.js. Once DRILLED INTO a
    // list - a system's game list, the app list, a settings screen - hold-to-scroll accelerates
    // like the XMB so long lists are fast to page through (user request). OSK and the media players
    // keep the accelerating feel. (Minima always accelerates; it never sets ndsCadence.)
    const bool ndsCadence = mNdsTheme && mPs3Xmb && mNdsAtRoot &&
                            !mOskActive && !mVidActive && !mMpActive && !mPvActive;
    int64_t interval;
    if (ndsCadence) {
        interval = (mNavRepeatCount == 0) ? kNdsNavInitialDelayMs : kNdsNavCadenceMs;
    } else if (mNavRepeatCount == 0) {
        interval = kNavInitialDelayMs;                 // 300ms before the first repeat
    } else {
        interval = (int64_t)((float)kNavSlowIntervalMs /
                             powf(kNavAccelMult, (float)(mNavRepeatCount - 1)));
        if (interval < kNavMinIntervalMs) interval = kNavMinIntervalMs;
    }
    if (now - mNavLastRepeatMs < interval) return;

    switch (mNavHeldDir) {
    case NavDir::Up:    handleUp();    break;
    case NavDir::Down:  handleDown();  break;
    case NavDir::Left:  handleLeft();  break;
    case NavDir::Right: handleRight(); break;
    case NavDir::None:  break;
    }
    mLastInputMs     = now;   // a held direction is ongoing user activity
    mNavLastRepeatMs = now;
    mNavRepeatCount++;
}

// L1/R1 bumper page-skip through a drilled DSi / Minima list or game carousel: jump a page of
// items at once (traditional fast list scrolling). Reuses the theme's own directional nav so the
// scroll / animation / selection state stays consistent, and Minima's per-frame SFX diff collapses
// the burst to a single cursor cue. Vertical for the Minima list and DSi settings lists, horizontal
// for the DSi game / app carousel. dir < 0 = back a page, dir > 0 = forward a page.
void NanoMenu::ndsBumperSkip(int dir) {
    // NextUI's L1/R1 jump to the previous / next initial-letter group on the (alphabetically
    // sorted) game list. Reproduce that on an alpha-sorted list level here; on a list that is not
    // alpha-sorted (settings screens, the recency-ordered Recently Played) fall back to the fixed
    // page skip so the bumper still advances a screenful.
    if (minimaAppGridActive()) { minimaAppGridPage(dir); return; }   // Minima Applications grid: a page of rows
    std::vector<Ps3Item>* items = nullptr; int* selPtr = nullptr;
    if (!mPs3Stack.empty())                                          { items = &mPs3Stack.back().items; selPtr = &mPs3Stack.back().sel; }
    else if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size())   { items = &mPs3Cats[mPs3CatIdx].items; selPtr = &mPs3ItemIdx; }
    const int n = items ? (int)items->size() : 0;
    if (items && selPtr && n >= 8) {
        // Group key = the label's first character folded to an upper-case letter; digits and
        // symbols share one leading '#' group (matching NextUI's getIndexChar bucketing).
        auto keyOf = [&](int i) -> char {
            const std::string& s = (*items)[i].label;
            char c = s.empty() ? '#' : s[0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            return (c >= 'A' && c <= 'Z') ? c : '#';
        };
        int ordered = 0;
        for (int i = 1; i < n; i++) if (keyOf(i) >= keyOf(i - 1)) ordered++;
        if (ordered * 100 >= (n - 1) * 80) {   // >=80% non-decreasing -> treat as an alpha-sorted list
            int sel = *selPtr; if (sel < 0) sel = 0; if (sel >= n) sel = n - 1;
            const char cur = keyOf(sel);
            int target = sel;
            if (dir > 0) {                                   // next group: first row past this letter
                int i = sel + 1; while (i < n && keyOf(i) == cur) i++;
                if (i < n) target = i;
            } else {                                         // start of this group, else the previous group
                int gs = sel; while (gs > 0 && keyOf(gs - 1) == cur) gs--;
                if (gs < sel) target = gs;
                else if (gs > 0) { const char pl = keyOf(gs - 1); int j = gs - 1; while (j > 0 && keyOf(j - 1) == pl) j--; target = j; }
                else target = 0;
            }
            if (target != sel) {
                *selPtr = target;
                mListWrapSnap  = true;    // snap the scroll window to the jump instead of gliding through
                mDisplayDirty  = true;    // minimaSfxTick fires the cursor SFX on the sel change next frame
            }
            return;
        }
    }
    const int PAGE = 10;
    bool vertical = mMinimaTheme || ndsCurLevelIsList();
    for (int i = 0; i < PAGE; i++) {
        if (vertical) { if (dir < 0) handleUp();   else handleDown(); }
        else          { if (dir < 0) handleLeft(); else handleRight(); }
    }
}

// Glide the Applications cursor to a freshly installed app "as if the nav button were
// held": step one row toward mPs3AutoScrollTarget on the same accelerating cadence as
// tickNavRepeat, seeding the item ease each step so it scrolls smoothly (not a snap).
void NanoMenu::tickAutoScroll() {
    if (mPs3AutoScrollTarget < 0) return;
    if (mPs3Stack.empty() || mPs3Stack.back().title != "Applications") { mPs3AutoScrollTarget = -1; return; }
    if (mNavHeldDir != NavDir::None) { mPs3AutoScrollTarget = -1; return; }   // user takes over
    int n = (int)mPs3Stack.back().items.size();
    if (n <= 0) { mPs3AutoScrollTarget = -1; return; }
    if (mPs3AutoScrollTarget > n - 1) mPs3AutoScrollTarget = n - 1;           // list shrank
    int& s = ps3CurSel();
    if (s == mPs3AutoScrollTarget) { mPs3AutoScrollTarget = -1; return; }     // arrived
    const int64_t now = android::uptimeMillis();
    int64_t interval;
    if (mPs3AutoScrollCount == 0) interval = kNavInitialDelayMs;
    else {
        interval = (int64_t)((float)kNavSlowIntervalMs /
                             powf(kNavAccelMult, (float)(mPs3AutoScrollCount - 1)));
        if (interval < kNavMinIntervalMs) interval = kNavMinIntervalMs;
    }
    if (now - mPs3AutoScrollLastMs < interval) return;
    mPs3ItemAnimFrom  = mPs3AnimItem;     // ease from the current animated position...
    mPs3ItemAnimStart = mEffectTime;      // ...to the new sel (same as a real nav step)
    s += (mPs3AutoScrollTarget > s) ? 1 : -1;
    mPs3AutoScrollLastMs = now;
    mPs3AutoScrollCount++;
    mDisplayDirty = true;
}

// ---------------------------------------------------------------------------
// Bluetooth bluesleep LPM wakelock helpers (sleep-time SoC-suspend fix).
//
// On the Allwinner/xradio Brick the vendor bt_chip_warmup.sh leaves
// /proc/bluetooth/sleep/lpm = 1 (LPM enabled) at boot even though Bluetooth is
// never turned on. The xradio_btlpm kernel driver then holds the "bluesleep"
// wakeup_source continuously (confirmed: it is held 100% of every screen-off
// window), which blocks suspend-to-RAM, so the SoC never deep-sleeps and the
// battery drains ~4-5%/hr while "asleep". When BT is powered off the LPM
// machinery is pure waste, so before driving a real suspend we disable LPM
// (releases bluesleep, verified via /sys/kernel/debug/wakeup_sources) and
// restore it on wake. Gated on rfkill (BT off) so we never disturb an active
// BT session, and on a kill-switch property. Everything no-ops if the nodes are
// absent (other devices / no xradio BT), so this is Brick-safe but harmless
// elsewhere.
//
// We must NEVER read() /proc/bluetooth/sleep/lpm. On other devices that expose
// this node (e.g. the Anbernic RG DS / rk3568 with a Broadcom bluesleep driver)
// the proc read handler is broken: bluesleep_read_proc_lpm writes straight into
// the caller's user buffer without copy_to_user, so under PAN the read faults in
// kernel context ("kernel access to user memory outside uaccess routines") and
// panics the box. Pressing power at the XMB home used to hit exactly this. So we
// only ever WRITE the node (0 to release, 1 to restore) and remember what we
// changed with our own latch instead of reading the current value. On those
// devices the node is read-only (0444), so the write fails cleanly and the whole
// feature self-disables there.
// ---------------------------------------------------------------------------
static bool nanoBtPoweredOff() {
    // rfkill0 == "sunxi-bt" (type bluetooth) on the Brick; state 0 == powered off.
    int fd = open("/sys/class/rfkill/rfkill0/state", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;            // cannot tell -> conservative (do nothing)
    char st = 0;
    ssize_t n = read(fd, &st, 1);
    close(fd);
    return n == 1 && st == '0';
}
static bool nanoBtLpmSet(int v) {
    int fd = open("/proc/bluetooth/sleep/lpm", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char c = v ? '1' : '0';
    ssize_t n = write(fd, &c, 1);
    close(fd);
    return n == 1;
}
// Drive BT LPM for the FRAMEWORK-owned sleep path (overlay / SurfaceFlinger home):
// when an app is foreground the framework, not nano, blanks the panel and drives
// suspend on screen-off, so enterDrmSleep never runs and the held bluesleep wakelock
// would still block suspend-to-RAM. On screen-off with BT powered off, release it;
// restore it (always, if we changed it) on screen-on. Idempotent; `disabled` is the
// caller's per-path latch. Same gating/kill-switch as the enterDrmSleep path.
void nanoBtLpmSuspendGate(bool screenOff, bool& disabled) {
    if (screenOff == disabled) return;   // already in the desired state
    if (screenOff) {
        if (property_get_bool("persist.gammaos.nano.btlpmsleep", true)
            && nanoBtPoweredOff() && nanoBtLpmSet(0)) {
            disabled = true;
            ALOGI("NanoMenu: BT off, screen off -> released bluesleep so the SoC can suspend");
        }
    } else {
        nanoBtLpmSet(1);                  // restore what we disabled
        disabled = false;
        ALOGI("NanoMenu: screen on -> restored BT LPM");
    }
}

// ---------------------------------------------------------------------------
// Event loop: drain every input fd, dispatch to navigation / power / OSK.
// ---------------------------------------------------------------------------

// Defined later in this file; forward-declared so the DRM sleep wait loop can honour
// a slide-open "wake" action.
static bool slideActionHas(const char* list, const char* act);

// Sleep the device after the Android Screen Timeout with no input on the menu.
// PowerManagerService.isNanoDisplayForcedOn keeps the panel lit while the menu
// is visible (the framework never sees the pad on a DRM-direct home), so its
// own timeout never fires on the menu; this is the menu-side equivalent. Runs on
// the render thread right after
// pollInput(), which stamps mLastInputMs / mLastPointerMs for every kind of input.
void NanoMenu::idleSleepTick() {
    otaSleepGuardTick();   // the OTA wakelock follows the flow, whatever the early returns below
    // The resident overlay coexists with a running app: the framework owns that
    // display and its timeout works there.
    if (mOverlayMode) return;
    if (mInDrmSleep.load(std::memory_order_relaxed)) return;
    if (!property_get_bool("sys.boot_completed", false)) return;   // settings + PowerManager not up

    // The setting is read through the settings shell (a popen), never on the
    // render thread: refresh it from a short-lived worker every 15 s so a change
    // made in Display Settings takes effect within that.
    const int64_t nowMs = android::uptimeMillis();
    if ((mIdleSleepTimeoutMs.load() == -2 || nowMs - mIdleSleepReadMs >= 15000)
        && !mIdleSleepReading.exchange(true)) {
        mIdleSleepReadMs = nowMs;
        std::thread([this]() {
            std::string v = readSettingValue(SettingSource::kSystem, "screen_off_timeout", "60000");
            long long ms = atoll(v.c_str());
            // -1 (nano's Never) and Integer.MAX_VALUE (Android's Never) both disable it.
            if (ms <= 0 || ms >= 2147483647LL) ms = -1;
            mIdleSleepTimeoutMs.store((int64_t)ms);
            mIdleSleepReading.store(false);
        }).detach();
    }
    const int64_t timeoutMs = mIdleSleepTimeoutMs.load();
    if (timeoutMs <= 0) return;   // unknown yet, or Never

    // Everything the framework side already exempts, plus the menu states where a
    // sleep would cut something the user is watching or waiting for. While the
    // home is not the thing in focus (a game or app runs, a launch is in flight,
    // the wizard, a video) nothing stamps mLastInputMs, so the timer restarts
    // when focus comes back: without that the home slept the instant a long
    // game session ended (reported 2026-09-22).
    static bool sIdleSleepUnfocused = false;
    const bool unfocused =
            mSetupWizardActive || mDrasticParked.load(std::memory_order_relaxed)
            || property_get_bool("sys.gammaos.nano.app_launched", false)
            || !mPanelApp.empty()   // dual screen: an app on the panel (Android's own timeout applies)
            || property_get_bool("sys.gammaos.nano.media_playing", false)   // in-process video
            || (mVidActive && mVidPlaying && !mVidStopped)
            || mLaunchFadeStart != 0 || mOverlayLaunchPending || mShowLaunchBusy || mWaitForRelease
            || mPowerPressTime != 0
            || mScrapeRunning    // a scrape or a folder copy in progress: sleeping would stall it
            || otaInProgress()   // an update being checked, downloaded or prepared
            || otaCopying();     // or its package being copied
    if (unfocused) { sIdleSleepUnfocused = true; return; }
    if (sIdleSleepUnfocused) {
        sIdleSleepUnfocused = false;
        mLastInputMs = mLastPointerMs = nowMs;   // focus regained: the timeout counts from here
        return;
    }

    const int64_t lastMs = mLastInputMs > mLastPointerMs ? mLastInputMs : mLastPointerMs;
    if (nowMs - lastMs < timeoutMs) return;

    ALOGI("NanoMenu: no input for %lld ms (screen timeout %lld ms) -> sleeping",
          (long long)(nowMs - lastMs), (long long)timeoutMs);
    if (sDrmActive) {
        // DRM-direct home: the same path as the power button and the lid. It
        // blanks our panels, drives PowerManager standby, and on wake stamps
        // mLastInputMs so the timer restarts (a false return means it shut down).
        enterDrmSleep();
    } else {
        // SurfaceFlinger-hosted home: the framework owns the display; ask it to
        // sleep (KEYCODE_SLEEP via the nano-dosleep service) and let the render
        // loop follow sys.screen.state as it does for a framework-driven sleep.
        property_set("sys.gammaos.nano.dosleep", "1");
    }
    mLastInputMs = mLastPointerMs = android::uptimeMillis();
}

bool NanoMenu::enterDrmSleep() {
    // The render thread is about to block in the wait loop below, so the render
    // heartbeat stops. Tell the watchdog this is intentional: otherwise it aborts
    // the oneshot home process after ~8s, the panel never relights, the power
    // button looks dead, and any background music is torn down (see the watchdog
    // in NanoMenuRender.cpp).
    mInDrmSleep.store(true, std::memory_order_relaxed);
    // A video player session does not survive sleep (the decoder is silent and a running
    // codec/worker through standby just wastes power); tear it fully down before parking.
    videoHardFree();
    // The DSi home BGM (menu_ambiance) must NOT keep playing behind the blanked panel
    // (user: suspend properly). Fully release it before parking, not just stop() it:
    // a stop() only pauses the AAudio stream, which keeps AudioFlinger's mixer
    // thread out of standby so it holds the AudioMix wakelock and blocks the suspend
    // below. It reopens on demand; the home restarts the ambiance on wake.
    if (mAmbiancePlaying) { mAmbiancePlayer.stop(); mAmbiancePlaying = false; }
    if (!mAmbianceOpening) mAmbiancePlayer.release();
    // If music is actively playing, keep it playing with the screen off: blank the
    // panel but do NOT drive a full system suspend (which would freeze the decoder
    // and AAudio threads), and hold a kernel wakelock so the SoC stays up. The
    // user pressed power expecting the track to keep going, like any music player.
    bool keepAudio = !mMpQueue.empty() && mMusicPlayer.isPlaying();
    // Set when we have disabled BT LPM for this suspend so we can restore it on wake.
    bool btLpmDisabled = false;
    const bool btLpmFix = property_get_bool("persist.gammaos.nano.btlpmsleep", true);
    // Release the bluesleep wakelock if (and only if) BT is powered off, so a real
    // suspend is not blocked. Idempotent; restored on wake.
    auto releaseBtLpm = [&]() {
        if (btLpmFix && !btLpmDisabled && nanoBtPoweredOff()
            && nanoBtLpmSet(0)) {
            btLpmDisabled = true;
            ALOGI("NanoMenu: BT off -> disabled BT LPM (released bluesleep) so the SoC can suspend");
        }
    };

    // Blank our DRM-owned panels: clear the slot-0 AHB FBOs (what drmFrameEnd
    // scans out) and turn every backlight off, so the wake-time recommit
    // relights onto black rather than a stale frame.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    if (sDrmZeroCopy && sAhbRingPrimary[0].glFbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, sAhbRingPrimary[0].glFbo);
        glClear(GL_COLOR_BUFFER_BIT);
        if (sAhbRingSecondary[0].glFbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, sAhbRingSecondary[0].glFbo);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        glClear(GL_COLOR_BUFFER_BIT);
    }
    drmFrameEnd(mDisplay, mSurface);
    nanobl::nanoBacklightSet(0);
    setBrightnessViaHal(0);
    // Drop to the powersave governor (lowest clocks) while the screen is off - audio
    // decode + the 1Hz poll run comfortably there. Restored on wake. Publish the
    // screen-off state so the BT stability monitor reverts to powersave (not the
    // user's perf mode) if A2DP audio stops while the screen is off.
    property_set("sys.gammaos.nano.screenoff", "1");
    nanoApplyPerfClock("powersave");

    // Drive the WHOLE device into a real PowerManager suspend (not just a
    // blanked busy-poll): the nano-dosleep init service injects KEYCODE_SLEEP
    // (nano's bootstrap mount namespace cannot run app_process directly) so
    // PowerManager runs its normal goToSleep -> doze -> suspend. Before
    // boot_completed PowerManager is not ready, so fall back to the legacy
    // blank + 60s-then-shutdown.
    // Drive proper Android standby (display off / low power) in BOTH cases once the
    // framework is up. The difference is the wakelock: when music is playing we hold
    // the nano_music kernel wakelock FIRST, which blocks suspend-to-RAM, so
    // PowerManager.goToSleep dozes (display off via the framework) but the SoC stays
    // up and the decode/AAudio threads keep running. Without engaging the framework
    // the lights HAL re-asserts the backlight and the panel never actually turns off.
    bool pmSleep = property_get_bool("sys.boot_completed", false);
    if (keepAudio) {
        int wl = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
        ssize_t n = (wl >= 0) ? write(wl, "nano_music", 10) : -1;
        if (wl >= 0) close(wl);
        if (n < 0)
            ALOGE("NanoMenu: failed to hold music wake_lock (errno %d) - audio may die on sleep", errno);
        else
            ALOGI("NanoMenu: screen off, music playing -- staying awake, audio continues");
        // Engage framework standby too (wakelock already held -> doze, not suspend).
        if (pmSleep) property_set("sys.gammaos.nano.dosleep", "1");
    } else if (pmSleep) {
        ALOGI("NanoMenu: services up -> PowerManager system sleep");
        property_set("sys.gammaos.nano.dosleep", "1");
        releaseBtLpm();   // BT off: drop the bluesleep wakelock so suspend-to-RAM works
    }

    bool asleep = true;
    int64_t sleepStart = android::uptimeMillis();
    int mpDoneTicks = 0;   // consecutive polls with the queue finished (debounce)
    // Held the moment a wake is detected and released only after the panel is
    // relit, so the SoC cannot re-suspend in the gap between the power-button
    // wake and the framework taking over the display. Without this the device
    // wakes, nothing holds a wakelock, and SystemSuspend re-suspends within ~2s -
    // so the user has to press power several times over ~30s to win the handoff.
    // nano grabs the power key at the home (the framework never sees it), so this
    // bridge is on nano; the in-app path is the framework's and is unaffected.
    bool wokeWakelock = false;
    auto holdWakeWakelock = [&]() {
        if (wokeWakelock) return;
        int wl = open("/sys/power/wake_lock", O_WRONLY | O_CLOEXEC);
        if (wl >= 0) { ssize_t n = write(wl, "nano_wake", 9); (void)n; close(wl); }
        wokeWakelock = true;
    };
    // Slide/hall wake: on devices whose swivel/lid trigger is a KEY (e.g. the RG Rotate's
    // gpio-keys KEY_F12), opening it must also wake here, or nano stays parked in this loop
    // and its DSI command-mode panel is never re-committed - the backlight relights but the
    // display stays blank until a power press. The trigger is the same prop-configured event
    // as the slide handler (key_type/key_code/key_active); "open" is the key leaving its
    // engaged value. Gated on the slide up-action including "wake" so a device that does not
    // want wake-on-open is unaffected. EV_SW SW_LID is already handled below.
    const int slideKeyType   = property_get_int32("persist.gammaos.rotate.key_type", EV_KEY);
    const int slideKeyCode   = property_get_int32("persist.gammaos.rotate.key_code", 88);
    const int slideKeyActive = property_get_int32("persist.gammaos.rotate.key_active", 1);
    bool slideWakeOnOpen = false;
    if (property_get_bool("persist.gammaos.rotate.enabled", false)) {
        char upAct[PROPERTY_VALUE_MAX] = {};
        property_get("persist.gammaos.rotate.up_action", upAct, "");
        slideWakeOnOpen = slideActionHas(upAct, "wake");
    }
    // Wait for the wake source with EPOLLWAKEUP. The framework EventHub reads
    // input exactly this way, which is WHY in-app wake is reliable on the first
    // press: with EPOLLWAKEUP the kernel holds a wakeup source from the instant
    // an input event is queued, through the epoll_wait that returns it, until
    // the next epoll_wait - so the SoC cannot re-suspend before this (frozen)
    // thread is scheduled to read the event. nano EVIOCGRABs the devices and
    // used to wait with a plain poll(), which has NO such guarantee: the power
    // press woke the kernel for ~2s, but if this thread wasn't scheduled in that
    // window the system re-suspended with the event still buffered, so the first
    // press (or two) was lost and the user had to mash power 3+ times. nano runs
    // as root so it has CAP_BLOCK_SUSPEND, the capability EPOLLWAKEUP requires.
    int wakeEpoll = epoll_create1(EPOLL_CLOEXEC);
    if (wakeEpoll >= 0) {
        for (int fd : mInputFds) {
            if (fd < 0) continue;
            struct epoll_event ev = {};
            ev.events = EPOLLIN | EPOLLWAKEUP;
            ev.data.fd = fd;
            epoll_ctl(wakeEpoll, EPOLL_CTL_ADD, fd, &ev);
        }
    } else {
        ALOGE("NanoMenu: epoll_create1 failed (errno %d) - wake may need extra presses", errno);
    }
    while (asleep) {
        // Block on the input fds so the CPU can idle / suspend (a busy poll
        // would keep it awake and defeat the suspend). With PowerManager
        // engaged, block indefinitely: the system suspends and this thread
        // freezes here until a wake source fires. Otherwise cap the wait at
        // the remaining 60s budget.
        int timeoutMs = -1;
        if (keepAudio) {
            timeoutMs = 1000;   // wake periodically to auto-advance the track
        } else if (!pmSleep) {
            int64_t left = 60000 - (android::uptimeMillis() - sleepStart);
            if (left <= 0) {
                ALOGI("NanoMenu: sleep timeout, shutting down");
                if (wakeEpoll >= 0) close(wakeEpoll);
                prepareShutdown("shutdown");
                mInDrmSleep.store(false, std::memory_order_relaxed);
                return false;
            }
            timeoutMs = (int)left;
        }
        if (wakeEpoll >= 0) {
            struct epoll_event evs[16];
            epoll_wait(wakeEpoll, evs, 16, timeoutMs);
        } else {
            // Fallback if epoll setup failed: the legacy poll() wait (no
            // EPOLLWAKEUP, so the multi-press hazard above can recur).
            struct pollfd pfds[16];
            int nf = 0;
            for (int fd : mInputFds) {
                if (fd >= 0 && nf < 16) { pfds[nf].fd = fd; pfds[nf].events = POLLIN; nf++; }
            }
            poll(pfds, nf, timeoutMs);
        }
        struct input_event wake;
        for (int wfd : mInputFds) {
            while (read(wfd, &wake, sizeof(wake)) == sizeof(wake)) {
                ssReleaseTaken(wake);
                // Wake on a power-button press OR the lid opening (SW_LID -> 0).
                if (wake.type == EV_KEY && wake.code == KEY_POWER
                    && wake.value == 1) {
                    asleep = false;
                    // The waking press bypasses the pollInput EV_KEY stamp
                    // (this loop consumes it), so stamp here or the menu could
                    // wake straight into the idle frame rate.
                    mLastInputMs = android::uptimeMillis();
                } else if (wake.type == EV_SW && wake.code == SW_LID
                           && wake.value == 0) {
                    asleep = false;
                    mLastInputMs = android::uptimeMillis();
                } else if (slideWakeOnOpen
                           && wake.type == slideKeyType && wake.code == slideKeyCode
                           && wake.value != 2                    // ignore EV_KEY auto-repeat
                           && wake.value != slideKeyActive) {     // trigger left its engaged value = opened
                    asleep = false;
                    mLastInputMs = android::uptimeMillis();
                }
            }
        }
        // Woke: pin the SoC up immediately so it cannot re-suspend before the
        // wake handling below relights the panel and the framework takes over.
        if (!asleep) holdWakeWakelock();
        // Keep the album playing through track changes while the screen is off
        // (audio-only, no GL touched). Mirrors musicTick's auto-advance + the
        // mMpAdvancing gate (async open keeps ended() true until the next track loads).
        if (mMpAdvancing && !mMusicPlayer.ended()) mMpAdvancing = false;
        if (keepAudio && !mMpQueue.empty() && mMusicPlayer.ended() && !mMpAdvancing) {
            mMpAdvancing = true;
            if (mMpRepeat == 2) mpPlayCurrent();
            else mpStep(1, true);
        }
        // No more audio to play: at the end of the queue (repeat off) mpStep issues a
        // Pause, so the player settles into isPaused() (it is never paused during a
        // track-to-track advance, which calls mpPlayCurrent). When that holds for a
        // couple of polls, release the music wakelock and hand off to a real low-power
        // system sleep, like normal Android when a playlist finishes.
        if (keepAudio && mMusicPlayer.isPaused() && !mMpQueue.empty()) {
            if (++mpDoneTicks >= 2) {
                int wl = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
                if (wl >= 0) { ssize_t n = write(wl, "nano_music", 10); (void)n; close(wl); }
                ALOGI("NanoMenu: music finished -> releasing wakelock, system sleep");
                keepAudio = false;
                pmSleep = property_get_bool("sys.boot_completed", false);
                if (pmSleep) { property_set("sys.gammaos.nano.dosleep", "1"); releaseBtLpm(); }
                sleepStart = android::uptimeMillis();   // restart the legacy 60s budget if PM is unavailable
            }
        } else {
            mpDoneTicks = 0;
        }
    }
    // Drop the EPOLLWAKEUP source now that we own the wake (holdWakeWakelock took
    // an explicit nano_wake wakelock above to bridge the relight below).
    if (wakeEpoll >= 0) { close(wakeEpoll); wakeEpoll = -1; }
    // Woke. We always drove framework standby (dosleep) when boot_completed, so wake
    // PowerManager via an injected KEYCODE_WAKEUP (it never saw the wake source, so it
    // will not auto-wake). Release the music wakelock LAST - after dowake - so the SoC
    // cannot suspend in the gap before KEYCODE_WAKEUP is delivered.
    if (pmSleep) {
        property_set("sys.gammaos.nano.dosleep", "0");
        property_set("sys.gammaos.nano.dowake", "1");
        ALOGI("NanoMenu: waking PowerManager (KEYCODE_WAKEUP)");
    }
    if (keepAudio) {
        int wl = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
        if (wl >= 0) { ssize_t n = write(wl, "nano_music", 10); (void)n; close(wl); }
    }
    // Restore BT LPM to the boot state we found it in, so BT-enable behaviour is
    // byte-identical to shipping outside of nano's suspend window.
    if (btLpmDisabled) {
        nanoBtLpmSet(1);
        ALOGI("NanoMenu: woke -> restored BT LPM");
    }
    usleep(200000);
    { struct input_event d; for (int dfd : mInputFds) {
        while (read(dfd, &d, sizeof(d)) == sizeof(d)) ssReleaseTaken(d); } }
    touchResync(-1, true);
    // Kernel resume re-enables the CRTCs with NO planes attached (rockchip
    // vop2 confirmed; every legacy page flip then EBUSYs forever and both
    // panels stay black behind a lit backlight). Re-commit the full modeset
    // + reset the flip bookkeeping BEFORE relighting so the panels come back
    // showing content. pollInput runs on the render thread, which owns all
    // DRM/GL state, so this is race-free. Idempotent and harmless on the
    // legacy pre-boot_completed path that never suspended.
    drmResumeRecommit();
    {
        int sysfs_val = mBrightness * mMaxBrightness / 255;
        if (sysfs_val < 1) sysfs_val = 1;
        nanobl::nanoBacklightSet(mBrightness);
        setBrightnessViaHal(sysfs_val);
    }
    property_set("sys.gammaos.nano.screenoff", "0");
    nanoRestorePerfClock();   // restore the user's performance mode (was powersave while off)
    // Hold nano_wake until the framework actually owns wakefulness. The wake key
    // is injected via `input keyevent 224` -> app_process, which on a low-RAM
    // device (1GB Brick) can take SECONDS to cold-start, deliver KEYCODE_WAKEUP
    // and have PowerManager wake the display. PowerManagerService sets
    // sys.screen.state=on exactly when it turns the display on and holds its
    // display suspend-blocker. Releasing nano_wake after a fixed delay let the
    // SoC re-suspend in that gap (kernel trace: "PM: suspend entry" at the same
    // instant as "woke up", then a press/re-suspend ping-pong on event0), so the
    // user had to mash power. nano_wake (a kernel wakelock) blocks BOTH the
    // framework SystemSuspend AND the vendor forced-suspend handshake, so keep it
    // until sys.screen.state=on, re-injecting the wake key once if it stalls, with
    // a hard cap so a truly stuck framework can't pin the render thread forever.
    if (pmSleep && wokeWakelock) {
        const int64_t waitStart = android::uptimeMillis();
        const int64_t maxWaitMs = 8000;
        bool reinjected = false;
        char ss[PROP_VALUE_MAX];
        for (;;) {
            property_get("sys.screen.state", ss, "off");
            if (strcmp(ss, "on") == 0) {
                ALOGI("NanoMenu: framework awake (screen on) after %lldms",
                      (long long)(android::uptimeMillis() - waitStart));
                // The composer re-applies baseparameter's BCSH on screen-on; put the stored
                // Screen Calibration back on top so a change not yet mirrored is not lost.
                screencal::applyFromProps();
                break;
            }
            int64_t elapsed = android::uptimeMillis() - waitStart;
            if (elapsed >= maxWaitMs) {
                ALOGW("NanoMenu: PowerManager wake unconfirmed after %lldms, releasing wakelock anyway",
                      (long long)elapsed);
                break;
            }
            if (!reinjected && elapsed >= 2000) {
                // The first KEYCODE_WAKEUP may have been lost to a re-suspend
                // before app_process delivered it; retry the injection once.
                property_set("sys.gammaos.nano.dowake", "1");
                reinjected = true;
                ALOGI("NanoMenu: re-injecting KEYCODE_WAKEUP (screen still off after 2s)");
            }
            usleep(100000);   // 100ms
        }
    }
    // Framework now holds its own display suspend-blocker (or we hit the cap).
    // Drop our bridge.
    if (wokeWakelock) {
        int wl = open("/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC);
        if (wl >= 0) { ssize_t n = write(wl, "nano_wake", 9); (void)n; close(wl); }
        wokeWakelock = false;
    }
    ALOGI("NanoMenu: woke up");
    mInDrmSleep.store(false, std::memory_order_relaxed);
    mDisplayDirty = true;   // the recommit above left the panels with no frame
    mSettledMs = 0;
    return true;
}

// The touchscreen over a screen that registers its inputs as touch zones (NanoTouch.h): the finger's
// down, moves and up go to the zones the last frame drew.
void NanoMenu::touchZonesFrame() {
    float px, py;
    if (!touchMapRaw(mTouchRawX, mTouchRawY, px, py)) { mTouchWasDown = mTouchDown; return; }
    const bool down = mTouchDown, downEdge = down && !mTouchWasDown, upEdge = !down && mTouchWasDown;
    mTouchWasDown = mTouchDown;
    const int64_t now = android::uptimeMillis();
    const float unit = (float)std::min(mWidth, mHeight);
    mTouch.setSlop(touchSlopPx());
    mTouch.setSwipeMin(unit * 0.08f);
    if (downEdge) {
        mTouch.down(px, py, now);
        TouchZones::Action act;
        if (mTouch.takeFired(act)) touchPress(std::move(act));
    } else if (down) {
        mTouch.move(px, py, now);
    } else if (upEdge) {
        mTouch.move(px, py, now);
        mTouch.up(now);
    } else {
        return;
    }
    mLastInputMs = now;
    mDisplayDirty = true;
}

void NanoMenu::touchPressButton(uint16_t code) {
    mKeyReplay.push_back({code, 1, true});
    mKeyReplay.push_back({code, 0, true});
}

void NanoMenu::touchHelpPrompt(const std::string& id, const TouchRect& r) {
    static const std::map<std::string, uint16_t> kButtons = {
        {"a", BTN_SOUTH}, {"b", BTN_EAST}, {"x", BTN_WEST}, {"y", BTN_NORTH},
        {"start", BTN_START}, {"back", BTN_SELECT}, {"select", BTN_SELECT},
        {"l", BTN_TL}, {"r", BTN_TR}, {"lt", BTN_TL2}, {"rt", BTN_TR2}, {"thumbstickclick", BTN_THUMBL},
    };
    // Two buttons in one prompt (the shoulders, the triggers): each half presses its own.
    static const std::map<std::string, std::pair<uint16_t, uint16_t>> kPairs = {
        {"lr", {BTN_TL, BTN_TR}}, {"ltrt", {BTN_TL2, BTN_TR2}},
    };
    auto pr = kPairs.find(id);
    if (pr != kPairs.end()) {
        const float half = r.w * 0.5f;
        touchButtonZone(pr->second.first, {r.x, r.y, half, r.h});
        touchButtonZone(pr->second.second, {r.x + half, r.y, half, r.h});
        return;
    }
    auto it = kButtons.find(id);
    if (it == kButtons.end()) return;   // a direction hint, not a button
    touchButtonZone(it->second, r);
}

// Where it was drawn, and around that up to a fingertip's size (about 6% x 9% of the screen's
// shorter side) as a margin that gives way to any other input under the finger.
void NanoMenu::touchButtonZone(uint16_t code, const TouchRect& r, bool onRelease) {
    const float unit = (float)std::min(mWidth, mHeight);
    auto press = [this, code]() { touchPressButton(code); };
    if (onRelease) {
        mTouch.tapMargin(r.grownTo(unit * 0.06f, unit * 0.09f), press);
        mTouch.tap(r, press);
        return;
    }
    mTouch.pressMargin(r.grownTo(unit * 0.06f, unit * 0.09f), press, r);
    mTouch.press(r, press);
}

// The next event to handle: a held-back key event first, then the device's own.
bool NanoMenu::nextInputEvent(int fd, struct input_event& ev) {
    mKeyReplaying = !mKeyReplay.empty();
    mKeyReplayTouch = mKeyReplaying && mKeyReplay.front().touch;
    if (!mKeyReplaying) return read(fd, &ev, sizeof(ev)) == sizeof(ev);
    ev = {};
    ev.type = EV_KEY;
    ev.code = mKeyReplay.front().code;
    ev.value = mKeyReplay.front().value;
    mKeyReplay.pop_front();
    return true;
}

// Start and Select are held back until they are let go. Let go on its own, the button's press and
// release are handled then; pressed together, they open or close the Quick Menu once both are up;
// with another key pressed meanwhile, they do nothing. Returns true when the event is taken.
bool NanoMenu::startSelectKey(uint16_t code, int32_t value) {
    const int i = code == BTN_START ? 0 : 1;
    if (value == 2) return true;
    if (value == 1) {
        if (!mSsHeld[1 - i]) mSsChord = mSsUsed = false;
        mSsHeld[i] = true;
        mSsFd[i] = mCurrentInputFd;
        if (mSsHeld[0] && mSsHeld[1]) mSsChord = true;
        return true;
    }
    if (!mSsHeld[i]) return true;   // its press was taken elsewhere (an Anbernic chord, or before nano showed)
    mSsHeld[i] = false;
    if (mSsChord) {
        if (!mSsHeld[0] && !mSsHeld[1] && !mSsUsed && !mSetupWizardActive) quickMenuButtonReleased();
        return true;
    }
    if (!mSsUsed) {
        mKeyReplay.push_back({code, 1, mKeyReplayTouch});
        mKeyReplay.push_back({code, 0, mKeyReplayTouch});
    }
    return true;
}

// Back is held back until it is let go (devkeys::backRelease): let go on its own it goes back then,
// or, where Back doubles as the function button, a hold of 1 to 3 s opens the Quick Menu; with
// another key pressed meanwhile it does nothing. Pressed while the Anbernic button is held it is a
// chord, and neither acts. Returns true when the event is taken.
bool NanoMenu::backKey(int32_t value) {
    if (value == 2) return true;
    const int64_t now = android::uptimeMillis();
    if (value == 1) {
        mBackHeld = true;
        mBackUsed = mAnbernicArmed;
        mAnbernicArmed = false;
        mBackFd = mCurrentInputFd;
        mBackDownMs = now;
        return true;
    }
    if (!mBackHeld) return true;   // its press was taken elsewhere, or before nano showed
    mBackHeld = false;
    switch (devkeys::backRelease(devkeys::current().sharedBack, mBackUsed, (long)(now - mBackDownMs))) {
    case devkeys::BackRelease::Back:
        mKeyReplay.push_back({KEY_BACK, 1, false});
        mKeyReplay.push_back({KEY_BACK, 0, false});
        break;
    case devkeys::BackRelease::QuickMenu:
        if (!mSetupWizardActive && property_get_bool("persist.gammaos.nano.mode_quick_settings", true))
            quickMenuButtonReleased();
        break;
    case devkeys::BackRelease::Nothing:
        break;
    }
    return true;
}

// A Start or Select release consumed before startSelectKey (a test screen): the
// press it held back is gone.
void NanoMenu::ssReleaseTaken(const struct input_event& ev) {
    if (ev.type == EV_KEY && ev.value == 0 && (ev.code == BTN_START || ev.code == BTN_SELECT))
        mSsHeld[ev.code == BTN_START ? 0 : 1] = false;
}

// Events from an input device lost (it closed, or the kernel dropped its queue): a release from it
// may never be read. A held Start, Select, Back or Anbernic button from it is dropped, and a button
// still held elsewhere does nothing when let go; a held direction from it stops repeating.
void NanoMenu::inputEventsLost(int fd) {
    mInputResync.erase(fd);
    nanoabxy::forget(fd);
    if (mNavFd == fd) {
        navRelease(NavDir::None);
        mStickXTriggered = mStickYTriggered = false;
        mNavFd = -1;
    }
    if (mAnbernicFd == fd) mAnbernicHeld = mAnbernicArmed = false;
    if (mTouchFromPointer && mTouchDown && mPointerFd == fd) {   // a held click whose release may be lost: let go unless still held
        constexpr size_t kBits = 8 * sizeof(unsigned long);
        unsigned long keys[(KEY_MAX + kBits) / kBits] = {};
        if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0 || !((keys[BTN_LEFT / kBits] >> (BTN_LEFT % kBits)) & 1UL)) {
            mTouchFromPointer = false; mPointerFd = -1;
            mTouchDown = mTouchWasDown = false;   // no release reaches the handlers: nothing acts
            mTouchPressUsed = false;
            touchGestureAbort();
        }
    }
    for (int i = 0; i < 2; i++) {
        if (mSsHeld[i] && mSsFd[i] == fd) {
            mSsHeld[i] = false;
            mSsUsed = true;
        }
    }
    if (mBackHeld && mBackFd == fd) mBackHeld = false;
    // A lift lost with the queue must not hold a gesture: the panel is read once its queue is empty.
    if (mTouchPanels.count(fd) && std::find(mTouchResyncFds.begin(), mTouchResyncFds.end(), fd) == mTouchResyncFds.end())
        mTouchResyncFds.push_back(fd);
}

// A device sending touch events: whether it is a touchscreen (direct, or at least not a pointer or
// button pad, which some touchscreen drivers leave unmarked) and whether it has slots.
NanoMenu::TouchPanel& NanoMenu::touchPanel(int fd) {
    auto it = mTouchPanels.find(fd);
    if (it != mTouchPanels.end()) return it->second;
    TouchPanel& p = mTouchPanels[fd];
    constexpr size_t kBits = 8 * sizeof(unsigned long);
    unsigned long props[(INPUT_PROP_CNT + kBits - 1) / kBits] = {};
    if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) >= 0) {
        auto has = [&](int b) { return ((props[b / kBits] >> (b % kBits)) & 1UL) != 0; };
        p.direct = has(INPUT_PROP_DIRECT) || !(has(INPUT_PROP_POINTER) || has(INPUT_PROP_BUTTONPAD));
    }
    unsigned long abs[(ABS_CNT + kBits - 1) / kBits] = {};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0)
        p.mt = ((abs[ABS_MT_SLOT / kBits] >> (ABS_MT_SLOT % kBits)) & 1UL) != 0;
    struct input_absinfo a{};
    if (p.mt && ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &a) >= 0) p.slot = a.value;   // the device keeps its selector
    return p;
}

// The single-finger path and the gestures follow one panel: the one whose finger landed first while
// no gesture was live. When that panel is all up the gesture ends; a panel still down then is ignored
// until it lifts (the gesture never moves to it), and the next panel to land owns the next gesture.
// One finger on each screen of a dual-screen device is not two, and a resting thumb on one screen
// leaves the other's taps alive once its own gesture ended.
void NanoMenu::touchPanelsMirror() {
    auto isDown = [](const TouchPanel& p) { return p.btn || p.contacts() > 0; };
    for (auto& kv : mTouchPanels) if (!isDown(kv.second)) kv.second.ignored = false;
    auto it = mTouchPanels.find(mTouchPanelFd);
    if (it != mTouchPanels.end() && !isDown(it->second)) {
        for (auto& kv : mTouchPanels) if (kv.first != mTouchPanelFd && isDown(kv.second)) kv.second.ignored = true;
        it = mTouchPanels.end();
        mTouchPanelFd = -1;
    }
    if (it == mTouchPanels.end()) {
        for (auto k = mTouchPanels.begin(); k != mTouchPanels.end(); ++k)
            if (k->second.direct && !k->second.ignored && isDown(k->second)) { it = k; mTouchPanelFd = k->first; break; }
    }
    if (it == mTouchPanels.end()) {
        std::fill(mTouchId, mTouchId + kTouchSlots, -1);
        if (!mTouchFromPointer) mTouchDown = false;
        return;
    }
    const TouchPanel& p = it->second;
    std::copy(p.id, p.id + kTouchSlots, mTouchId);
    std::copy(p.sx, p.sx + kTouchSlots, mTouchSX);
    std::copy(p.sy, p.sy + kTouchSlots, mTouchSY);
    if (!mTouchFromPointer) {
        mTouchDown = isDown(p);
        mTouchRawX = p.sx[0]; mTouchRawY = p.sy[0];   // slot 0 = primary finger
    }
}

// What is down after a resync or a removal: when it changed, the gesture under way is let go without
// acting, and fingers still down are ignored until all are up.
void NanoMenu::touchAfterChange(bool changed) {
    touchPanelsMirror();
    if (changed) touchGestureAbort();
    if (touchContacts() == 0 && !(mTouchDown && !mTouchFromPointer)) {
        mTouchMulti = false; mTouchGestureLive = false; mTouchGestureOsk = false; mTouchPressUsed = false;
        mPvPinchActive = false;
    } else if (changed) {
        mTouchMulti = true; mTouchMultiMoved = true; mTouchMultiMax = 0;
        mTouchMultiMs = android::uptimeMillis();
        std::fill(mTouchMultiId, mTouchMultiId + kTouchSlots, -1);
        mTouchGestureLive = true; mPvPinchActive = false;
    }
    if (changed) mTouchWasDown = mTouchDown;
}

// Reads a touchscreen's contacts, slot, positions and BTN_TOUCH from the device: after its events
// were drained unread, dropped (SYN_DROPPED), or by the watchdog. A device that is gone or can't be
// read has nothing down. Only the given panel is read (fd -1: all): at the report that ends a drop,
// else once it was read to EAGAIN. Unchanged and not discarded, nothing is touched (a resting finger
// under the watchdog).
void NanoMenu::touchResync(int onlyFd, bool discarded) {
    constexpr size_t kBits = 8 * sizeof(unsigned long);
    bool changed = false;
    for (auto& kv : mTouchPanels) {
        const int fd = kv.first;
        TouchPanel& p = kv.second;
        if (!p.direct || (onlyFd >= 0 && fd != onlyFd)) continue;
        const TouchPanel was = p;
        bool ok = mTouchInputOff != 1;
        if (ok && p.mt) {
            struct { uint32_t code; int32_t v[kTouchSlots]; } mt;
            mt.code = ABS_MT_TRACKING_ID;
            std::fill(mt.v, mt.v + kTouchSlots, -1);
            ok = ioctl(fd, EVIOCGMTSLOTS(sizeof(mt)), &mt) >= 0;
            if (ok) {
                std::copy(mt.v, mt.v + kTouchSlots, p.id);
                mt.code = ABS_MT_POSITION_X;
                if (ioctl(fd, EVIOCGMTSLOTS(sizeof(mt)), &mt) >= 0) std::copy(mt.v, mt.v + kTouchSlots, p.sx);
                mt.code = ABS_MT_POSITION_Y;
                if (ioctl(fd, EVIOCGMTSLOTS(sizeof(mt)), &mt) >= 0) std::copy(mt.v, mt.v + kTouchSlots, p.sy);
                struct input_absinfo a{};
                if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &a) >= 0) p.slot = a.value;
            }
        }
        unsigned long keys[(KEY_MAX + kBits) / kBits] = {};
        ok = ok && ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0;
        p.btn = ok && ((keys[BTN_TOUCH / kBits] >> (BTN_TOUCH % kBits)) & 1UL) != 0;
        if (!ok || !p.mt) std::fill(p.id, p.id + kTouchSlots, -1);
        // A slotted panel's tracking ids show whether a contact changed in what was dropped; a
        // slotless panel cannot tell, so a contact it had is let go.
        const bool hadAny = was.btn || was.contacts() > 0;
        if (p.btn != was.btn || !std::equal(p.id, p.id + kTouchSlots, was.id) || (discarded && hadAny && !p.mt))
            changed = true;
    }
    touchAfterChange(changed);
    mTouchLastEvMs = android::uptimeMillis();
}

// A removed device: its panel is forgotten (its fd number may come back), with no ioctl on any other.
void NanoMenu::touchPanelGone(int fd) {
    mTouchResyncFds.erase(std::remove(mTouchResyncFds.begin(), mTouchResyncFds.end(), fd), mTouchResyncFds.end());
    auto it = mTouchPanels.find(fd);
    if (it == mTouchPanels.end()) return;
    const bool hadAny = it->second.direct && (it->second.btn || it->second.contacts() > 0);
    mTouchPanels.erase(it);
    touchAfterChange(hadAny);
}

// The DSi bottom bar's Back (-1) and OK (+1) legends under a touch (DS units), as wide as their
// drawn labels (renderNdsSubmenu's bar: Back at x8, OK ending at x248, 11 px text); 0 for neither.
// flash: a hit lights the legend up.
int NanoMenu::ndsBarHit(float dsX, float dsY, bool flash) {
    if (dsY < 170.0f || dsY > 192.0f) return 0;
    const float fs = 11.0f / (float)FONT_CHAR_H;
    const float backR = 16.0f + measureText(trDyn("Back"), fs), okL = 240.0f - measureText(trDyn("OK"), fs);
    const int hit = dsX <= backR ? -1 : dsX >= okL ? 1 : 0;
    if (hit && flash) {
        if (hit < 0) ndsPressFlash(0.0f, 171.0f, backR, 21.0f);
        else         ndsPressFlash(okL, 171.0f, 256.0f - okL, 21.0f);
    }
    return hit;
}

// A DSi bottom-panel button pressed: it lights up briefly (DS units), drawn by drawNdsPressFlash.
void NanoMenu::ndsPressFlash(float x, float y, float w, float h) {
    mNdsFlashR = TouchRect{x, y, w, h};
    mNdsFlashMs = (int64_t)android::uptimeMillis();
    mDisplayDirty = true;
}

// A launch a touch pressed waits for the finger to lift before handing off, so the app or game never
// gets a finger already down (read from the devices: the hand-off paths stop reading events). At
// most 3 s past the launch effect; a launch from a key or a click never waits.
bool NanoMenu::launchHeldByFinger() {
    launchStartNote();
    if (!mLaunchByTouch || mLaunchFadeStart == 0
        || (int64_t)uptimeMillis() - mLaunchFadeStart > launchFadeHoldMs() + 3000) return false;
    constexpr size_t kBits = 8 * sizeof(unsigned long);
    for (const auto& kv : mTouchPanels) {
        if (!kv.second.direct) continue;
        unsigned long keys[(KEY_MAX + kBits) / kBits] = {};
        if (ioctl(kv.first, EVIOCGKEY(sizeof(keys)), keys) >= 0
            && ((keys[BTN_TOUCH / kBits] >> (BTN_TOUCH % kBits)) & 1UL)) return true;
    }
    return false;
}

// Both multi-finger taps act only on Nano's own settled screens: no launch under way, no PSP clock,
// Control Center, BIOS dialog, manual, setup or keyboard.
bool NanoMenu::multiTapAllowed() {
    return !mWaitForRelease && mLaunchFadeStart == 0 && !(mOverlayMode && mOverlayLaunchPending)
           && !(mPspClockOn || mPspClockReveal > 0.0f) && !controlCenterActive() && !mBiosDlgActive
           && !mPdfActive && !mSetupWizardActive && !mOskActive && !mTouchGestureOsk;
}

// A second finger makes the gesture a multi-finger one until every finger is up: the press so far is
// dropped (touchGestureAbort) and no touch handler sees the rest of it. A gesture that began with the
// keyboard up is left alone, for typing with two thumbs. Let go without moving, within half a second,
// two fingers open or close the Quick Menu, as the Anbernic button does, and three the Applications
// view; four or more do nothing.
bool NanoMenu::multiTouchFrame() {
    if (mTouchInputOff == 1) {   // its lifts are dropped
        for (auto& kv : mTouchPanels) { std::fill(kv.second.id, kv.second.id + kTouchSlots, -1); kv.second.btn = false; }
        if (!mTouchFromPointer) mTouchDown = false;
    }
    touchPanelsMirror();
    const int n = touchContacts();
    const bool fingers = n > 0 || (mTouchDown && !mTouchFromPointer);
    const int64_t now = android::uptimeMillis();
    const bool held = fingers || (mTouchDown && mTouchFromPointer);   // a held mouse click too
    if (!mTouchMulti) {
        if (mTouchPressUsed && !held) {
            touchPressTick(true);   // let go inside the wait: the press acts now
            mTouchGestureLive = false; mTouchGestureOsk = false;
            mTouchPressUsed = false; mTouchWasDown = false;   // a pressed button's release does nothing
            return true;
        }
        if (!fingers) {
            mTouchGestureLive = false; mTouchGestureOsk = false;
            return mTouchPressUsed;   // a held click on a button: nothing more until it lets go
        }
        if (!mTouchGestureLive) { mTouchGestureLive = true; mTouchGestureOsk = mOskActive; mTouchPressUsed = false; }
        // A button pressed as the first finger landed: the rest of the gesture does nothing, other
        // fingers included once it has acted. A second finger before that drops it (touchGestureAbort).
        if (mTouchPressUsed && (!mTouchPressAct || n < 2 || mTouchGestureOsk)) return true;
        if (n < 2 || mTouchGestureOsk) return false;
        mTouchPressUsed = false;
        mTouchMulti = true; mTouchMultiMoved = false; mTouchMultiMax = 0; mTouchMultiMs = now;
        std::fill(mTouchMultiId, mTouchMultiId + kTouchSlots, -1);
        touchGestureAbort();
    }
    mTouchMultiMax = std::max(mTouchMultiMax, n);
    const int rx = std::max(1, mTouchMaxX - mTouchMinX), ry = std::max(1, mTouchMaxY - mTouchMinY);
    for (int i = 0; i < kTouchSlots; i++) {
        if (mTouchId[i] < 0) continue;
        if (mTouchMultiId[i] != mTouchId[i]) {   // a finger new to the gesture: where it starts
            mTouchMultiId[i] = mTouchId[i]; mTouchMultiX[i] = mTouchSX[i]; mTouchMultiY[i] = mTouchSY[i];
        } else if (std::abs(mTouchSX[i] - mTouchMultiX[i]) * 25 > rx
                   || std::abs(mTouchSY[i] - mTouchMultiY[i]) * 25 > ry) {
            mTouchMultiMoved = true;   // a pinch or a drag
        }
    }
    if (fingers) return true;
    mTouchMulti = false; mTouchGestureLive = false; mTouchGestureOsk = false;
    mTouchWasDown = false;   // the handlers see no release
    mPvPinchActive = false; mPvDragActive = false; mPvPanning = false;
    if (!mTouchMultiMoved && now - mTouchMultiMs < 500 && multiTapAllowed()) {
        if (mTouchMultiMax == 2) quickMenuButtonReleased();
        else if (mTouchMultiMax == 3) threeFingerTap();
    }
    return true;
}

// A second finger landed: what the first finger pressed, grabbed or dragged is let go without acting.
void NanoMenu::touchGestureAbort() {
    mTouch.cancel();
    mTouchPressAct = nullptr;
    // The DSi carousel and lists (the dual-screen bottom panel): a carousel or scrollbar pill the finger
    // moved goes back; the deferred presses (back, arrows, track jumps) are dropped.
    if (mNdsDragMode == 1 || mNdsDragMode == 2) mNdsCamera = mNdsDragDownCam;
    mNdsDragMode = 0; mNdsThumbHeld = false; mNdsScrubbing = false; mNdsFlingVel = 0.0f;
    mNdsListThumb = false; mNdsListScrub = false; mNdsListFlingVel = 0.0f;
    mNdsTouchMoved = true;
    mPvDragActive = false; mPvPanning = false;
    mPdfTouchDrag = false;
    mPspSwipeDragging = false; mPspSwipeRawPx = 0.0f;
    mDisplayDirty = true;
}

// A press button under a landing finger (a touch zone's, or a DSi hit-tester's): lit at once, it acts
// kTouchPressDeferMs later, or as the finger lifts if that is sooner. The rest of the gesture does nothing.
void NanoMenu::touchPress(std::function<void()> act) {
    mTouchPressUsed = true; mTouchWasDown = mTouchDown;
    mTouchPressAct = std::move(act);
    mTouchPressScreen = touchPressScreen();
    mTouchPressDueMs = (int64_t)android::uptimeMillis() + kTouchPressDeferMs;
    mDisplayDirty = true;
}

void NanoMenu::touchPressTick(bool lifted) {
    if (!mTouchPressAct || (!lifted && (int64_t)android::uptimeMillis() < mTouchPressDueMs)) return;
    std::function<void()> act = std::move(mTouchPressAct);
    mTouchPressAct = nullptr;
    if (touchPressScreen() != mTouchPressScreen) return;   // a dialog or wizard screen opened or closed meanwhile
    act();
    mDisplayDirty = true;
}

// A dialog by its title and choices (its body may update while it shows), a wizard by its screen.
std::string NanoMenu::touchPressScreen() const {
    std::string s;
    if (mPs3DlgActive && !mPs3DlgClosing) {
        s = "d" + mPs3DlgTitle;
        for (const auto& o : mPs3DlgOptions) s += '\n' + o;
    }
    if (mPs3WizActive) s += "\tw" + std::to_string(mPs3WizId);
    return s;
}

// Which input started the launch under way: mLastInputTouch, set by a finger's or click's landing
// and cleared by a controller press, still holds it when this runs after the launching event. A key
// launch drops the gesture of a finger resting on the screen.
void NanoMenu::launchStartNote() {
    const bool launching = mWaitForRelease || mLaunchFadeStart != 0;
    if (launching && !mLaunchNoted) {
        mLaunchByTouch = mLastInputTouch;
        if (!mLaunchByTouch && (mTouchDown || touchContacts() > 0)) touchGestureAbort();
    }
    mLaunchNoted = launching;
}

// Three fingers tapped: the Applications view opens or closes, as with Anbernic + Start. Only on
// Nano's own home, never over a running app.
void NanoMenu::threeFingerTap() {
    if (!mPs3Xmb || !multiTapAllowed()) return;
    if (mOverlayMode && (!mOverlayShown || quickAppBehind())) return;
    if (applicationsViewOpen()) closeApplicationsView();
    else openApplicationsView();
}

// The Anbernic button, or Start + Select, let go: open or close the Quick Menu. A chooser opened
// from the Quick Menu is cancelled so it still closes it.
void NanoMenu::quickMenuButtonReleased() {
    if (!mPs3Xmb || (mOverlayMode && !mOverlayShown)) return;
    if (quickMenuOpen() && (mPs3DlgActive || mPs3OptActive)) {
        if (mPs3OptActive) closeXmbOpt();
        if (mPs3DlgActive) closePs3Dialog(false);
        if (mPs3DlgActive || mPs3OptActive) return;   // a dialog that must stay up
    }
    if (!ndsInModal()) toggleQuickMenu();
}

// The HOME function key held on its own for the hold (devkeys::functionHold): Home.
void NanoMenu::functionHomeTick() {
    if (!mAnbernicArmed || mAnbernicHomeDownMs == 0
        || android::uptimeMillis() - mAnbernicHomeDownMs < devkeys::kFunctionHoldMs) return;
    mAnbernicArmed = false;
    mAnbernicHomeDownMs = 0;
    if (!anbernicPhysicallyDown()) return;   // its release was read elsewhere
    functionHomeHeld();
}

// Home, from the HOME function key's hold. Over a running app the app closes to the home, as Close
// Current App does. In Nano's own menus every level, dialog, keyboard and player closes (music plays
// on, minimised) and the home shows its top: the ES-DE home, the DSi or Minima root, or on the XMB
// the first row of the category the home starts on (Startup Menu, else Game).
void NanoMenu::functionHomeHeld() {
    ALOGI("NanoMenu: Home button held -> home");
    bmapClose(false);
    if (mOverlayMode && mOverlayShown && !mOverlayWallpaper && quickAppBehind()) {
        overlayQuitToHome();
        return;
    }
    if (mVidActive) closeVideoPlayer();
    if (mMpActive) { if (mpIsOpening()) closeMusicPlayer(); else minimizeMusicPlayer(); }
    if (mPvActive) closePhotoViewer();
    if (mPhotoMultiActive) photoMultiClose();
    if (mPdfActive) pdfClose();
    if (mPs3TzActive) closeTimezoneGlobe(false);
    if (mPs3LangActive) closeLanguagePicker(false);
    if (mBiosDlgActive) biosDialogClose();
    if (mWifiManageActive) wifiManageClose();
    // What leaving these levels with Back frees.
    for (const auto& lvl : mPs3Stack) {
        if (lvl.screenKind == PHOTO_GRID) closePhotoGrid();
        else if (lvl.screenKind == GS_ICONGRID) closeIconGridPicker();
    }
    if (mEqGammaEqDepth >= 0) { stopEqPreview(); freeEqPcm(); mEqGammaEqDepth = -1; }
    mNotifs.clear();
    if (mEsdeTheme || mEsdeSettingsMinima) {
        esdeReturnHome();
        return;
    }
    if (mOskActive) {
        mOskActive = false; mOsk.closing = false; mOsk.anim = 0.0f;
        mOskPasswordMode = false; mOskPlaintext = false;
        mOskPasswordPrompt.clear(); mOskPasswordCallback = nullptr; mOskQuery.clear();
    }
    mPs3DlgActive = mPs3OptActive = mPs3DlgClosing = mPs3OptClosing = false;
    mPs3WizActive = mPs3TzActive = mPs3LangActive = mPs3BrightSlider = false;
    if (mGSearchActive) gsearchClose();
    if (mMenuState == MENU_WIFI) closeWifiScreen();
    else if (mMenuState == MENU_BT) closeBtScreen();
    mMenuState = MENU_MAIN;
    mPs3Stack.clear();
    mQsReturn = QsReturn();
    mAppsReturn = QsReturn(); mAppsSummoned = false;
    int home = -1;
    char bootTo[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.boot_to", bootTo, "");
    const std::string want = strncmp(bootTo, "cat:", 4) == 0 ? std::string(bootTo + 4) : std::string("Game");
    for (size_t i = 0; i < mPs3Cats.size() && home < 0; i++)
        if (mPs3Cats[i].name == want) home = (int)i;
    for (size_t i = 0; i < mPs3Cats.size() && home < 0; i++)
        if (mPs3Cats[i].name == "Game") home = (int)i;
    if (home >= 0) {
        mPs3CatIdx = home;
        mPs3ItemIdx = 0;
        if (home < (int)mPs3CatItemSel.size()) mPs3CatItemSel[home] = 0;
    }
    mPs3AnimItem = (float)mPs3ItemIdx; mPs3ItemAnimStart = -1.0f; mPs3SubAnimStart = -1.0f;
    mPs3CatAnimActive = false; mPs3CatT = 1.0f; mPs3CatFromOffset = 0.0f;
    mNdsAtRoot = true;
    mNdsCamera = (float)mPs3CatIdx; mNdsScrubbing = false; mNdsFlingVel = 0.0f;
    mDisplayDirty = true;
}

bool NanoMenu::keyHeld(int code) const {
    const int nlongs = (KEY_MAX + 1 + 8 * (int)sizeof(long) - 1) / (8 * (int)sizeof(long));
    unsigned long bits[nlongs];
    for (int fd : mInputFds) {
        if (fd < 0) continue;
        memset(bits, 0, sizeof(bits));
        if (ioctl(fd, EVIOCGKEY(sizeof(bits)), bits) < 0) continue;
        if (bits[code / (8 * sizeof(long))] & (1UL << (code % (8 * sizeof(long)))))
            return true;
    }
    return false;
}

// DSi carousel touch (bottom panel). Maps the raw digitizer through the shared
// touchMapRaw (same path as oskTouchFrame/xmbTouchFrame, correct on DRM + SF) into
// carousel DS coords, then dispatches a tap (a side tile selects it, the centre tile
// launches, the L/R buttons step, the scrollbar jumps) or a horizontal drag that scrolls
// the carousel. Mirrors the web launcher.js touch model.
// DSi carousel touch, a 1:1 port of the web app's menuTouch* (main.js): the carousel
// follows the finger continuously (camera = downCam - dx/65), a release fling coasts
// with 0.85/frame ease-out then snaps, the scrollbar pill tracks the finger absolutely
// (camera = (x-33)/5), the L/R arrows step, a blank-track press jumps, and a tap on the
// centred tile launches while a tap on a side tile selects it. The camera physics + snap
// live in renderNdsCarousel; this only sets the owner state (scrub / fling / thumb).

// Map the current raw touch into DS 256x192 coordinates using the SAME device rect the DSi
// carousel/list/side-panel is rendered into (ndsCarouselRect): the bottom band in the stacked
// single-panel layout, the whole panel otherwise. This keeps every DSi touch handler aligned
// with what is drawn (fixes taps landing in the wrong row / on the top screen when stacked).
// Returns whether touchMapRaw resolved; dsX/dsY fall back to the last down position when not.
bool NanoMenu::ndsMapTouchDs(float& dsX, float& dsY) {
    float px, py; bool mapped = touchMapRaw(mTouchRawX, mTouchRawY, px, py);
    float crx, cry, crw, crh; ndsCarouselRect(crx, cry, crw, crh);
    float scale = crh / 192.0f;
    if (256.0f * scale > crw + 0.5f) scale = crw / 256.0f;   // width-limited: don't overflow
    if (scale < 1e-3f) { dsX = mNdsTouchDownX; dsY = mNdsTouchDownY; return false; }
    const float offY = cry + (crh - 192.0f * scale) * 0.5f;
    const float cx   = crx + crw * 0.5f;                     // 256-wide DS chrome centred in the rect
    dsX = mapped ? 128.0f + (px - cx) / scale : mNdsTouchDownX;
    dsY = mapped ? (py - offY) / scale : mNdsTouchDownY;
    return mapped;
}

void NanoMenu::ndsTouchFrame() {
    if (!mOverlayMode && mLaunchFadeStart > 0) { mTouchWasDown = mTouchDown; return; }  // frozen during launch
    float dsX, dsY; const bool mapped = ndsMapTouchDs(dsX, dsY);   // maps into the DSi carousel rect (bottom band when stacked)

    // Focused carousel level (stacked nav): categories at the root, else the category/submenu.
    int nItems = ndsFocusCount();
    const float camMax = (nItems > 0) ? (float)(nItems - 1) : 0.0f;
    auto scrub = [&](float cam){                       // launcher.scrub: finger owns the camera
        if (cam < 0.0f) cam = 0.0f; if (cam > camMax) cam = camMax;
        mNdsCamera = cam; mNdsScrubbing = true; mNdsFastScroll = false; mDisplayDirty = true;
    };
    auto selStep = [this](int off){                     // nav step + slide (the arrows; may run later)
        const int cur = ndsFocusSel(), last = std::max(0, ndsFocusCount() - 1);
        const int ni = std::max(0, std::min(last, cur + off));
        if (ni != cur) { ndsCommitSelect(ni); mDisplayDirty = true; }
    };
    auto pixelToSlot = [&](float x)->int{               // launcher.pixelToSlot (tap -> slot)
        float off = x - 128.0f, a = fabsf(off);
        float d = (a <= 65.0f) ? a / 65.0f : 1.0f + (a - 65.0f) / 58.0f;
        int s = (int)lroundf(mNdsCamera + (off < 0.0f ? -1.0f : 1.0f) * d);
        if (s < 0) s = 0; if (s > (int)camMax) s = (int)camMax; return s;
    };

    bool down = mTouchDown;
    bool downEdge = down && !mTouchWasDown;
    bool upEdge   = !down && mTouchWasDown;

    if (downEdge && mapped) {
        // A touch that stops a glide only stops it: it taps no card.
        const bool stopsGlide = mNdsFlingVel != 0.0f || mNdsFastScroll;
        mNdsTouchMoved = stopsGlide; mNdsTouchDownX = dsX; mNdsTouchDownY = dsY;
        mNdsDragDownCam = mNdsCamera; mNdsDragLastCam = mNdsCamera;
        mNdsScrubbing = false; mNdsFlingVel = 0.0f; mNdsFastScroll = false;   // any prior glide stops on a fresh touch
        // ---- back button (top-left) ---- only inside a category/submenu (never at the DSi
        // root): a visible, tappable way to escape a level. The back button and the L/R arrows
        // are presses (touchPress), and the rest of the gesture does nothing; the blank track acts
        // on release.
        // A touch that stops a glide presses nothing either.
        if (!stopsGlide && !mNdsAtRoot && dsX >= 0.0f && dsX <= 24.0f && dsY >= 0.0f && dsY <= 26.0f) {   // half-size back button (touch-generous)
            ndsPressFlash(0.0f, 0.0f, 24.0f, 26.0f);
            mNdsDragMode = 0;
            touchPress([this]() { ndsNavBack(); });
            return;
        } else if (dsY >= 170.0f && dsY <= 192.0f) {        // ---- scrollbar band ----
            if (stopsGlide && (dsX <= 18.0f || dsX >= 237.0f)) {
                mNdsDragMode = 0;                           // stopped the glide: nothing more
            } else if (dsX <= 18.0f || dsX >= 237.0f) {     // L / R arrow
                ndsPressFlash(dsX <= 18.0f ? 0.0f : 237.0f, 170.0f, 19.0f, 22.0f);
                mNdsDragMode = 0;
                touchPress([selStep, off = dsX <= 18.0f ? -1 : +1]() { selStep(off); });
                return;
            } else {
                const float pillStep = ndsPillStep(nItems);
                float thumbLeft = 19.0f + pillStep * mNdsCamera;
                if (thumbLeft < 19.0f) thumbLeft = 19.0f; if (thumbLeft > 208.0f) thumbLeft = 208.0f;
                if (dsX >= thumbLeft && dsX < thumbLeft + 29.0f) {        // grab the pill
                    mNdsDragMode = 2; mNdsThumbHeld = true; scrub((dsX - 33.0f) / pillStep);
                } else {                                                 // blank track -> fast glide to slot
                    mNdsDragMode = 5;
                }
            }
        } else if (dsY >= 78.0f && dsY <= 165.0f) {         // ---- carousel drag band ----
            mNdsDragMode = 1;
        } else {
            mNdsDragMode = 0;
        }
    } else if (down && mapped && mNdsDragMode != 0) {       // ---- move ----
        if (mNdsDragMode == 1) {                            // carousel band: decide browse vs hierarchy swipe
            float dx = dsX - mNdsTouchDownX, dy = dsY - mNdsTouchDownY;
            // A predominantly VERTICAL drag is a hierarchy swipe (drill down / back up), a touch
            // way to walk the stacked-carousel nav (user request); a horizontal drag is the
            // normal finger-follow carousel browse. Commit on the first significant travel.
            if (!mNdsTouchMoved && fabsf(dy) > 14.0f && fabsf(dy) > fabsf(dx) * 1.3f) {
                mNdsDragMode = 6;                           // vertical hierarchy swipe
            } else {                                        // horizontal carousel finger-follow (65px/slot)
                if (fabsf(dx) > kNdsTouchSlopDs) mNdsTouchMoved = true;
                float prev = mNdsCamera;
                scrub(mNdsDragDownCam - dx / 65.0f);
                mNdsFlingVel = mNdsCamera - prev;           // per-move velocity (used at release)
                mNdsDragLastCam = mNdsCamera;
                mNdsTouchMoveMs = (int64_t)uptimeMillis();
            }
        } else if (mNdsDragMode == 6) {                     // vertical hierarchy swipe (fires once per gesture)
            float dy = mNdsTouchDownY - dsY;                // + = swiped UP
            if (!mNdsTouchMoved) {                          // mNdsTouchMoved doubles as the one-shot latch here
                if (dy > 40.0f)       { mNdsTouchMoved = true; ndsNavSelect(false); }   // swipe up -> drill into the item
                else if (dy < -40.0f) { mNdsTouchMoved = true; ndsNavBack(); }          // swipe down -> back up a level
            }
        } else if (mNdsDragMode == 2) {                     // thumb absolute follow
            scrub((dsX - 33.0f) / ndsPillStep(nItems));
        } else if (mNdsDragMode == 5) {                     // blank track: a drag is no jump
            if (fabsf(dsX - mNdsTouchDownX) > kNdsTouchSlopDs || fabsf(dsY - mNdsTouchDownY) > kNdsTouchSlopDs)
                mNdsTouchMoved = true;
        }
    } else if (upEdge) {                                    // ---- release ----
        int mode = mNdsDragMode; mNdsDragMode = 0;
        if (mode == 1) {
            if (mNdsTouchMoved) {                            // fling then snap (launcher.flingScrub)
                // Held still for 80 ms before the lift: no fling.
                float vel = (int64_t)uptimeMillis() - mNdsTouchMoveMs > 80 ? 0.0f : mNdsFlingVel;
                mNdsScrubbing = false;
                if (fabsf(vel) > 0.03f) mNdsFlingVel = vel;
                else { int s = (int)lroundf(mNdsCamera); mNdsCamera = (float)s; mNdsFlingVel = 0.0f; ndsCommitSelect(s); }
            } else {                                         // a TAP
                mNdsScrubbing = false; mNdsFlingVel = 0.0f;
                int slot = pixelToSlot(mNdsTouchDownX);
                if (slot == ndsFocusSel() && mNdsTouchDownY >= 84.0f && mNdsTouchDownY <= 150.0f) {
                    ndsNavSelect();                          // centred card -> enter category / drill / launch
                    // A touch tap has no physical select-key release, so the wait-for-release
                    // handler in pollInput never stamps the launch fade and the menu hangs
                    // holding DRM master with no way back. If the select launched an app, the
                    // finger is already up, so stamp the fade here (drills/enters set no wait).
                    if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0)
                        mLaunchFadeStart = uptimeMillis();
                } else {
                    ndsCommitSelect(slot);                   // side card -> select + slide to it
                }
            }
        } else if (mode == 2) {                              // thumb release -> snap
            mNdsThumbHeld = false; mNdsScrubbing = false; mNdsFlingVel = 0.0f;
            int s = (int)lroundf(mNdsCamera); mNdsCamera = (float)s; ndsCommitSelect(s);
        } else if (mode == 5 && !mNdsTouchMoved) {           // blank track (not a touch that stopped a glide)
            int slot = (int)lroundf((mNdsTouchDownX - 33.0f) / ndsPillStep(nItems));
            if (slot < 0) slot = 0; if (slot > (int)camMax) slot = (int)camMax;
            // launcher.scrollTo: the pill glides to the pressed slot with the DS's FAST
            // momentum (ease-out ~40%/frame), NOT the slow nav slide. Only when it moves.
            if (slot != ndsFocusSel() || fabsf(mNdsCamera - (float)slot) > 0.001f) {
                ndsCommitSelect(slot); mNdsFastScroll = true;
            }
        }
    }
    mTouchWasDown = mTouchDown;
}

// DSi settings-list submenu touch (matches renderNdsSubmenu's layout): tap a glossy row
// to select + enter it, tap the "Back" hint at the bottom-left to pop the level. Vertical
// list scrolling is by the D-pad (the list already follows the selection); this keeps the
// touch surface honest to the visible rows.
void NanoMenu::ndsSubmenuTouch() {
    if (!mOverlayMode && mLaunchFadeStart > 0) { mTouchWasDown = mTouchDown; return; }  // frozen during launch
    // Resolve the current list level generically (a submenu OR the drilled category, e.g. the Quick
    // Menu) exactly as renderNdsSubmenu + ndsCurLevelIsList; the selection index lives in stack.sel
    // for a submenu, mPs3ItemIdx for a category.
    std::vector<Ps3Item>* itemsP = nullptr; int* selPtr = nullptr;
    if (!mPs3Stack.empty()) { itemsP = &mPs3Stack.back().items; selPtr = &mPs3Stack.back().sel; }
    else if (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) { itemsP = &mPs3Cats[mPs3CatIdx].items; selPtr = &mPs3ItemIdx; }
    if (!itemsP) { mTouchWasDown = mTouchDown; return; }
    std::vector<Ps3Item>& lvlItems = *itemsP;
    float dsX, dsY; const bool mapped = ndsMapTouchDs(dsX, dsY);   // maps into the DSi carousel rect (bottom band when stacked)

    int n = (int)lvlItems.size();
    bool down = mTouchDown, downEdge = down && !mTouchWasDown, upEdge = !down && mTouchWasDown;
    // Match renderNdsSubmenu's layout: a fitting list is centred (_btnY), a long list scrolls.
    const float listTop = 30.0f, listBot = 168.0f, bh = 24.0f;
    const int fitRows = (int)((listBot - listTop) / 32.0f);
    const bool scrolling = n > fitRows;
    const float pitch = scrolling ? 32.0f : (n >= 4 ? 32.0f : 40.0f);
    const float top0 = scrolling ? listTop : roundf(94.0f - (float)(n - 1) * pitch * 0.5f - 12.0f);
    const float scroll = scrolling ? mNdsSubScroll : 0.0f;

    // Scrollbar geometry (mirrors drawNdsListScrollbar): column DS x233..251, up arrow y26..43, down
    // arrow y151..168, glossy thumb in the groove y43..151. thumbFrac/scrollFrac as the renderer draws.
    const float sbx = 233.0f, sbw = 19.0f, aH = 17.0f;
    const float grooveTop = 26.0f + aH, grooveBot = 168.0f - aH, grooveH = grooveBot - grooveTop;
    const float maxScroll = (float)(n - fitRows);
    auto thumbHof = [&]() { float f = (float)fitRows / (float)n; float h = fmaxf(12.0f, grooveH * f); return h > grooveH ? grooveH : h; };
    // Keep the highlighted row = the row at the vertical centre of the visible band, so the D-pad
    // ease-to-selection (render mode c) does NOT snap the scroll back after a scrollbar interaction.
    auto syncSel = [&]() { int c = (int)lroundf(mNdsSubScroll + (float)(fitRows / 2)); if (c < 0) c = 0; if (c > n - 1) c = n - 1; if (selPtr) *selPtr = c; };

    if (downEdge && mapped) {
        // A touch that stops a coasting list only stops it: it taps no row.
        mNdsTouchMoved = mNdsListFlingVel != 0.0f;
        mNdsTouchDownX = dsX; mNdsTouchDownY = dsY; mNdsSubDownSel = *selPtr;
        mNdsListScrubDown = mNdsSubScroll; mNdsListFlingVel = 0.0f; mNdsListScrub = false; mNdsListThumb = false;
        mNdsDragMode = 0;
        // The scrollbar arrows and the Back / OK bars are presses (touchPress), and the rest of the
        // gesture does nothing. The thumb grabs; the blank groove jumps on release.
        // A touch that stops a coasting list presses nothing either.
        const bool canPress = !mNdsTouchMoved;
        const bool upArrow = canPress && scrolling && dsX >= sbx && dsX < sbx + sbw && dsY >= 26.0f && dsY < 26.0f + aH;
        const bool dnArrow = canPress && scrolling && dsX >= sbx && dsX < sbx + sbw && dsY >= grooveBot && dsY < 168.0f;
        if (upArrow || dnArrow) {
            ndsPressFlash(sbx, upArrow ? 26.0f : grooveBot, sbw, aH);
            // The level is looked up again when it acts: the selection pointer may not outlive the wait.
            touchPress([this, upArrow, maxScroll, fitRows, n]() {
                int* sel = !mPs3Stack.empty() ? &mPs3Stack.back().sel
                         : (mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()) ? &mPs3ItemIdx : nullptr;
                if (!sel) return;
                mNdsSubScroll = upArrow ? fmaxf(0.0f, mNdsSubScroll - 1.0f) : fminf(maxScroll, mNdsSubScroll + 1.0f);
                *sel = std::max(0, std::min(n - 1, (int)lroundf(mNdsSubScroll + (float)(fitRows / 2))));   // syncSel
                mDisplayDirty = true;
            });
            return;
        }
        // The Y Remove / Restore prompt in the bar's centre, as renderNdsSubmenu draws it (DS units).
        if (const char* ya = canPress && dsY >= 170.0f && dsY <= 192.0f ? menuYAction() : nullptr) {
            const float halfW = (10.0f + 3.0f + measureText(trDyn(ya), 11.0f / (float)FONT_CHAR_H)) * 0.5f + 4.0f;
            if (ndsBarHit(dsX, dsY, false) == 0 && fabsf(dsX - 128.0f) <= halfW) {
                ndsPressFlash(128.0f - halfW, 171.0f, halfW * 2.0f, 21.0f);
                touchPress([this]() { if (menuYAction()) touchPressButton(BTN_WEST); });
                return;
            }
        }
        if (canPress && ndsBarHit(dsX, dsY, true) != 0) {
            touchPress([this, bar = ndsBarHit(dsX, dsY, false)]() {
                if (bar < 0) ndsNavBack();                  // "Back" bar -> pop the level / return to the root
                else {
                    ndsNavSelect(true);                     // "OK" bar -> activate the current selection
                    if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0) mLaunchFadeStart = uptimeMillis();
                }
            });
            return;
        }
        if (scrolling && dsX >= sbx && dsX < sbx + sbw) {
            if (dsY >= grooveTop && dsY < grooveBot) {                         // groove
                float thumbH = thumbHof();
                float scrollFrac = maxScroll > 0.0f ? mNdsSubScroll / maxScroll : 0.0f;
                float thumbY = grooveTop + (grooveH - thumbH) * scrollFrac;
                if (dsY >= thumbY && dsY < thumbY + thumbH) mNdsListThumb = true;      // grab the thumb 1:1
                else mNdsDragMode = 10;                                               // blank groove
            }
        }
    } else if (down && mapped) {
        float prevScroll = mNdsSubScroll;
        if (fabsf(dsY - mNdsTouchDownY) > kNdsTouchSlopDs || fabsf(dsX - mNdsTouchDownX) > kNdsTouchSlopDs) mNdsTouchMoved = true;
        if (mNdsListThumb && scrolling && n > 0) {
            // Scrollbar thumb drag: the thumb centre tracks the finger 1:1 (no fling on release).
            float thumbH = thumbHof();
            float f = (grooveH - thumbH) > 0.0f ? (dsY - grooveTop - thumbH * 0.5f) / (grooveH - thumbH) : 0.0f;
            if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
            mNdsSubScroll = f * maxScroll; syncSel(); mDisplayDirty = true;
        } else if (mNdsTouchMoved && scrolling && n > 0 &&
                   (mNdsListScrub || (mNdsTouchDownX >= 34.0f && mNdsTouchDownX <= 220.0f))) {
            // Content pixel-scroll (web scrub): the list follows the finger 1:1; velocity feeds the fling.
            mNdsListScrub = true;
            mNdsSubScroll = mNdsListScrubDown + (mNdsTouchDownY - dsY) / pitch;
            if (mNdsSubScroll < 0.0f) mNdsSubScroll = 0.0f;
            if (mNdsSubScroll > maxScroll) mNdsSubScroll = maxScroll;
            mNdsListFlingVel = mNdsSubScroll - prevScroll;   // rows/frame
            if (mNdsSubScroll != prevScroll) mNdsTouchMoveMs = (int64_t)uptimeMillis();
            mDisplayDirty = true;
        }
    } else if (upEdge && mNdsDragMode == 10) {              // blank groove: jump the thumb centre under the finger
        mNdsDragMode = 0;
        if (scrolling && !mNdsTouchMoved) {
            float thumbH = thumbHof();
            float f = (grooveH - thumbH) > 0.0f ? (mNdsTouchDownY - grooveTop - thumbH * 0.5f) / (grooveH - thumbH) : 0.0f;
            if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
            mNdsSubScroll = f * maxScroll;
            syncSel(); mDisplayDirty = true;
        }
    } else if (upEdge && (mNdsListThumb || (mNdsTouchMoved && mNdsListScrub))) {
        // Release a scrollbar/content drag: the thumb stops dead, a content drag coasts on its fling.
        if (mNdsListThumb || (int64_t)uptimeMillis() - mNdsTouchMoveMs > 80) mNdsListFlingVel = 0.0f;   // held still: no coast
        mNdsListThumb = false; mNdsListScrub = false;
    } else if (upEdge && !mNdsTouchMoved) {                 // a TAP
        if (mNdsTouchDownY >= listTop && mNdsTouchDownY <= listBot &&
            mNdsTouchDownX >= 34.0f && mNdsTouchDownX <= 220.0f && n > 0) {
            int i = (int)floorf((mNdsTouchDownY - top0) / pitch + scroll);
            float rowY = top0 + ((float)i - scroll) * pitch;             // reject taps in the gap
            if (i >= 0 && i < n && mNdsTouchDownY >= rowY && mNdsTouchDownY <= rowY + bh) {
                *selPtr = i; mDisplayDirty = true;
                ndsNavSelect(true);                        // activate the tapped row (root/category/stack aware)
                if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0)
                    mLaunchFadeStart = uptimeMillis();      // touch launch: drive the fade (see ndsTouchFrame)
            }
        }
    }
    mTouchWasDown = mTouchDown;
}

// DSi picker list touch (Time Zone / System Language), matching renderNdsPickerList's layout
// (bx17 bw222, listTop30 listBot168, pitch26). Tap a row to pick it and confirm (DSi "touch
// confirms" model); drag to scroll a long list; tap the "Back" hint to cancel or "OK" to confirm
// the current selection. The picker index (mTzSelected / mLangSelected) is what the renderer and
// the XMB apply path (closeTimezoneGlobe / closeLanguagePicker) both read.
void NanoMenu::ndsPickerTouch() {
    if (!mOverlayMode && mLaunchFadeStart > 0) { mTouchWasDown = mTouchDown; return; }  // frozen during launch
    const bool isLang = mPs3LangActive;
    int* selPtr = nullptr; int n = 0;
    if (mPs3TzActive)        { selPtr = &mTzSelected;   n = (int)mTzEntries.size(); }
    else if (mPs3LangActive) { selPtr = &mLangSelected; n = LOCALE_COUNT; }
    if (!selPtr || n <= 0) { mTouchWasDown = mTouchDown; return; }

    float dsX, dsY; const bool mapped = ndsMapTouchDs(dsX, dsY);   // maps into the DSi carousel rect (bottom band when stacked)

    bool down = mTouchDown, downEdge = down && !mTouchWasDown, upEdge = !down && mTouchWasDown;
    const float listTop = 30.0f, listBot = 168.0f, bh = 24.0f, pitch = 26.0f;
    const int fitRows = (int)((listBot - listTop) / pitch);
    const bool scrolling = n > fitRows;
    const float top0 = scrolling ? listTop : roundf(94.0f - (float)(n - 1) * pitch * 0.5f - 12.0f);
    const float scroll = scrolling ? mNdsSubScroll : 0.0f;

    if (downEdge && mapped) {
        mNdsTouchMoved = false; mNdsTouchDownX = dsX; mNdsTouchDownY = dsY; mNdsSubDownSel = *selPtr;
        if (const int bar = ndsBarHit(dsX, dsY, true)) {   // the Back / OK bar is a press
            touchPress([this, bar]() {
                if (mPs3LangActive) closeLanguagePicker(bar > 0);
                else if (mPs3TzActive) closeTimezoneGlobe(bar > 0);
            });
            return;
        }
    } else if (down && mapped) {
        if (fabsf(dsY - mNdsTouchDownY) > kNdsTouchSlopDs || fabsf(dsX - mNdsTouchDownX) > kNdsTouchSlopDs) mNdsTouchMoved = true;
        if (mNdsTouchMoved && scrolling) {
            int d = (int)lroundf((mNdsTouchDownY - dsY) / pitch);
            int ns = mNdsSubDownSel + d; if (ns < 0) ns = 0; if (ns > n - 1) ns = n - 1;
            if (ns != *selPtr) { *selPtr = ns; mDisplayDirty = true; }   // language: live preview follows in the dispatch
        }
    } else if (upEdge && !mNdsTouchMoved) {                              // a TAP
        if (mNdsTouchDownY >= listTop && mNdsTouchDownY <= listBot &&
            mNdsTouchDownX >= 17.0f && mNdsTouchDownX <= 227.0f) {
            int i = (int)floorf((mNdsTouchDownY - top0) / pitch + scroll);
            float rowY = top0 + ((float)i - scroll) * pitch;            // reject taps in the gap
            if (i >= 0 && i < n && mNdsTouchDownY >= rowY && mNdsTouchDownY <= rowY + bh) {
                *selPtr = i;
                if (isLang) closeLanguagePicker(true); else closeTimezoneGlobe(true);   // pick + apply + close
            }
        }
    }
    mTouchWasDown = mTouchDown;
}

// DSi settings-options SIDE PANEL touch (matches renderNdsSidePanel): tap a glossy row to
// select + activate it, drag to scroll a long list, tap the "Back" hint to dismiss, or drag
// the slider track. Rebuilds the same visible-row -> real-index map the renderer uses so the
// tapped row drives the correct mPs3OptSel / mPs3OptSubSel / mPs3DlgSel.
void NanoMenu::ndsSidePanelTouch() {
    if (!mOverlayMode && mLaunchFadeStart > 0) { mTouchWasDown = mTouchDown; return; }  // frozen during launch
    float dsX, dsY; const bool mapped = ndsMapTouchDs(dsX, dsY);   // maps into the DSi carousel rect (bottom band when stacked)

    // Rebuild the visible-row -> real-index map exactly as renderNdsSidePanel does.
    const bool optSrc = (mPs3OptActive || mPs3OptClosing);
    bool subOpen = false, slider = false;
    std::vector<int> realIdx; int n = 0, curSel = 0;
    if (optSrc) {
        subOpen = mPs3OptSubOpen && mPs3OptSel >= 0 && mPs3OptSel < (int)mPs3OptSubRows.size()
                  && !mPs3OptSubRows[mPs3OptSel].empty();
        if (subOpen) {
            n = (int)mPs3OptSubRows[mPs3OptSel].size();
            for (int j = 0; j < n; j++) realIdx.push_back(j);
            curSel = mPs3OptSubSel;
        } else {
            int m = (int)mPs3OptLabels.size();
            for (int i = 0; i < m; i++) {
                if (i < (int)mPs3OptSep.size() && mPs3OptSep[i]) continue;
                if (i == mPs3OptSel) curSel = (int)realIdx.size();
                realIdx.push_back(i);
            }
            n = (int)realIdx.size();
        }
    } else {
        slider = mPs3DlgSlider && mPs3DlgOptions.empty();
        n = (int)mPs3DlgOptions.size();
        for (int i = 0; i < n; i++) realIdx.push_back(i);
        curSel = mPs3DlgSel;
    }

    bool down = mTouchDown, downEdge = down && !mTouchWasDown, upEdge = !down && mTouchWasDown;

    if (slider) {                                          // drag the value track (DS x 40..216, y ~108..136)
        // A touch that starts on the track drags it (and keeps dragging if the finger drifts off
        // it vertically); a tap on the bottom bar is Back (left) or OK (right), as in the lists.
        if (downEdge && mapped) {
            mNdsTouchDownX = dsX; mNdsTouchDownY = dsY; mNdsTouchMoved = false;
            if (const int bar = ndsBarHit(dsX, dsY, true)) {   // the Back / OK bar is a press
                touchPress([this, bar]() {
                    if (bar < 0) ps3XmbBack();
                    else ps3XmbSelect();                // commit the value, like the D-pad confirm
                });
                return;
            }
        }
        const bool onTrack = mNdsTouchDownY >= 104.0f && mNdsTouchDownY <= 140.0f;
        if (down && mapped && onTrack) {
            float t = (dsX - 40.0f) / (216.0f - 40.0f); if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            float v = mPs3DlgSldMin + t * (mPs3DlgSldMax - mPs3DlgSldMin);
            float steps = roundf((v - mPs3DlgSldMin) / mPs3DlgSldStep);
            v = mPs3DlgSldMin + steps * mPs3DlgSldStep;
            if (v < mPs3DlgSldMin) v = mPs3DlgSldMin; if (v > mPs3DlgSldMax) v = mPs3DlgSldMax;
            if (v != mPs3DlgSldVal) {
                mPs3DlgSldVal = v; mDisplayDirty = true;
                screenCalSliderPreview();
                if (mShaderParamEdit >= 0 && mShaderParamEdit < (int)mShaderParams.size()) {
                    mShaderParams[mShaderParamEdit].cur = v; shaderApplyParamLive(mShaderParamEdit);
                }
            }
        }
        mTouchWasDown = mTouchDown; return;
    }

    // list layout mirrors renderNdsSidePanel.
    const float listTop = 30.0f, listBot = 168.0f, bh = 24.0f, bx = 34.0f, bw = 186.0f;
    const int fitRows = (int)((listBot - listTop) / 32.0f);
    const bool scrolling = n > fitRows;
    const float pitch = scrolling ? 32.0f : (n >= 4 ? 32.0f : 40.0f);
    const float top0 = scrolling ? listTop : roundf(94.0f - (float)(n - 1) * pitch * 0.5f - 12.0f);
    const float scroll = scrolling ? mNdsSubScroll : 0.0f;

    if (downEdge && mapped) {
        mNdsTouchMoved = false; mNdsTouchDownX = dsX; mNdsTouchDownY = dsY; mNdsSubDownSel = curSel;
        if (const int bar = ndsBarHit(dsX, dsY, true)) {   // the Back / OK bar is a press
            touchPress([this, bar, optSrc]() {
                if (bar < 0) ps3XmbBack();                  // "Back" bar -> dismiss / back out a submenu
                else {
                    // "OK" bar -> confirm/apply the current selection (matches the D-pad confirm).
                    if (optSrc) xmbOptEnter(); else ps3XmbSelect();
                    if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0) mLaunchFadeStart = uptimeMillis();
                }
            });
            return;
        }
    } else if (down && mapped) {
        if (fabsf(dsY - mNdsTouchDownY) > kNdsTouchSlopDs || fabsf(dsX - mNdsTouchDownX) > kNdsTouchSlopDs) mNdsTouchMoved = true;
        if (mNdsTouchMoved && scrolling && n > 0) {         // vertical drag steps the selection (drives the scroll)
            int d = (int)lroundf((mNdsTouchDownY - dsY) / pitch);
            int ns = mNdsSubDownSel + d; if (ns < 0) ns = 0; if (ns > n - 1) ns = n - 1;
            if (optSrc && !subOpen) { if (ns < (int)realIdx.size()) mPs3OptSel = realIdx[ns]; }
            else if (optSrc && subOpen) mPs3OptSubSel = ns;
            else mPs3DlgSel = ns;
            mDisplayDirty = true;
        }
    } else if (upEdge) {
        if (!mNdsTouchMoved && mNdsTouchDownY >= listTop && mNdsTouchDownY <= listBot &&
                   mNdsTouchDownX >= bx && mNdsTouchDownX <= bx + bw && n > 0) {
            int i = (int)floorf((mNdsTouchDownY - top0) / pitch + scroll);
            float rowY = top0 + ((float)i - scroll) * pitch;
            if (i >= 0 && i < n && mNdsTouchDownY >= rowY && mNdsTouchDownY <= rowY + bh) {
                if (optSrc && !subOpen) { mPs3OptSel = realIdx[i]; xmbOptEnter(); }
                else if (optSrc && subOpen) { mPs3OptSubSel = i; xmbOptEnter(); }
                else { mPs3DlgSel = i; ps3XmbSelect(); }   // confirm/apply the chosen option
                if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0)
                    mLaunchFadeStart = uptimeMillis();      // a row could launch: drive the fade (see ndsTouchFrame)
                mDisplayDirty = true;
            }
        }
    }
    mTouchWasDown = mTouchDown;
}

// DSi message-box DIALOG touch (matches renderNdsDialog): tap a button (left = option 0,
// right = option 1, or the single OK), or tap the scrim outside the panel to cancel. Mirrors
// the XMB dialog tap model (set mPs3DlgSel, then ps3XmbSelect applies it).
void NanoMenu::ndsDialogTouch() {
    if (!mOverlayMode && mLaunchFadeStart > 0) { mTouchWasDown = mTouchDown; return; }  // frozen during launch
    float dsX, dsY; const bool mapped = ndsMapTouchDs(dsX, dsY);   // maps into the DSi carousel rect (bottom band when stacked)

    bool down = mTouchDown, downEdge = down && !mTouchWasDown, upEdge = !down && mTouchWasDown;
    const bool gi = ndsGameInfoActive(), paged = ndsDlgInfoPaged();
    const int nOpt = (int)mPs3DlgOptions.size();
    const float by = 18.0f;                                 // panel top (settled); buttons at by+118 h32
    const float btnY = by + 118.0f, btnH = 32.0f;
    if (downEdge && mapped) {
        mNdsTouchDownX = dsX; mNdsTouchDownY = dsY; mNdsTouchMoved = false;
        // The buttons, the L/R pager pills and the game page's Back chevron are presses (touchPress),
        // and the rest of the gesture does nothing.
        auto pressed = [&](std::function<void()> act) {
            ndsPressFlash(dsX - 12.0f, dsY - 10.0f, 24.0f, 20.0f);   // around the finger on the pressed control
            touchPress(std::move(act));
        };
        auto choose = [this](int i) {
            return [this, i]() { if (mPs3DlgActive && !mPs3DlgClosing) { mPs3DlgSel = i; ps3XmbSelect(); } };
        };
        if (gi || paged) {
            // pill hit regions differ: game page pills sit in the screen corners (x14/x242, y181),
            // the dialog pager flanks the OK button (x30/x226, y150).
            const float lx = gi ? 14.0f : 30.0f, rx = gi ? 242.0f : 226.0f, ppy = gi ? 181.0f : 150.0f;
            if (dsY >= ppy - 9.0f && dsY <= ppy + 14.0f) {
                if (fabsf(dsX - lx) <= 15.0f) { pressed([this]() { ndsInfoPage(-1); }); return; }
                if (fabsf(dsX - rx) <= 15.0f) { pressed([this]() { ndsInfoPage(+1); }); return; }
            }
            if (gi && dsX >= 0.0f && dsX <= 24.0f && dsY >= 0.0f && dsY <= 26.0f) { pressed([this]() { ps3XmbBack(); }); return; }
        }
        if (!gi && dsY >= btnY && dsY <= btnY + btnH) {
            if (nOpt >= 3) {
                for (int i = 0; i < 3; i++)
                    if (dsX >= kNdsDlg3BtnX[i] && dsX <= kNdsDlg3BtnX[i] + kNdsDlg3BtnW) { pressed(choose(i)); return; }
            } else if (nOpt >= 2) {
                if (dsX >= 37.0f && dsX <= 126.0f)       { pressed(choose(0)); return; }
                if (dsX >= 134.0f && dsX <= 223.0f)      { pressed(choose(1)); return; }
            } else if (!otaInProgress() && dsX >= 84.0f && dsX <= 172.0f) {   // the single OK (renderNdsDialog)
                pressed(choose(0)); return;
            }
        }
    } else if (down && mapped) {
        if (fabsf(dsY - mNdsTouchDownY) > kNdsTouchSlopDs || fabsf(dsX - mNdsTouchDownX) > kNdsTouchSlopDs) mNdsTouchMoved = true;
    } else if (upEdge) {
        // Game Information page / paginated info dialog: swipe-to-page; other taps on the game page
        // do nothing.
        if (gi || paged) {
            if (mNdsTouchMoved) {                            // horizontal swipe turns the page
                float dxTot = dsX - mNdsTouchDownX;
                if (fabsf(dxTot) > 30.0f && fabsf(dxTot) > fabsf(dsY - mNdsTouchDownY))
                    ndsInfoPage(dxTot < 0 ? +1 : -1);        // swipe left = next
                mTouchWasDown = mTouchDown; return;
            }
            if (gi) { mTouchWasDown = mTouchDown; return; }
            // paged dialog: fall through to the standard OK / scrim tap handling below
        }
        if (!mNdsTouchMoved) {
            bool inPanel = (mNdsTouchDownX >= 16.0f && mNdsTouchDownX <= 240.0f &&
                            mNdsTouchDownY >= by && mNdsTouchDownY <= by + 156.0f);
            bool onButtons = (mNdsTouchDownY >= btnY && mNdsTouchDownY <= btnY + btnH);
            if (onButtons && nOpt >= 2) {
                // between the buttons: nothing
            } else if (inPanel && nOpt <= 1) {
                if (nOpt == 1) mPs3DlgSel = 0;
                ps3XmbSelect();                             // single-button / message dialog: any panel tap = OK
            } else if (!inPanel) {
                ps3XmbBack();                               // tap the scrim outside the panel = cancel
            }
        }
    }
    mTouchWasDown = mTouchDown;
}

// True if a comma-separated slide-action list (persist.gammaos.rotate.down_action/up_action)
// contains the given action. down_action/up_action are now multi-action lists (e.g. "rotate,clock"),
// so nano scans the list for the actions it owns (rotate/natural drive sys.gammaos.rotate.state, and
// "clock" opens the PSP clock); the framework owns screenoff/wake/launch on its own key path.
static bool slideActionHas(const char* list, const char* act) {
    if (!list || !*list || !act) return false;
    const size_t alen = strlen(act);
    for (const char* p = list; *p; ) {
        while (*p == ' ' || *p == ',') ++p;
        const char* s = p;
        while (*p && *p != ',') ++p;
        const char* e = p;
        while (e > s && e[-1] == ' ') --e;
        if ((size_t)(e - s) == alen && strncmp(s, act, alen) == 0) return true;
    }
    return false;
}

// Map a Linux evdev keycode from a physical (USB/BT) keyboard to a printable ASCII
// codepoint for on-screen-keyboard text entry, honouring Shift for a US layout. Returns
// 0 for non-printable keys (arrows, Enter, function keys) so callers fall through to the
// normal navigation handling.
int NanoMenu::kbdCodeToCp(int code, bool shift) {
    switch (code) {
        case KEY_A: return shift ? 'A' : 'a';   case KEY_B: return shift ? 'B' : 'b';
        case KEY_C: return shift ? 'C' : 'c';   case KEY_D: return shift ? 'D' : 'd';
        case KEY_E: return shift ? 'E' : 'e';   case KEY_F: return shift ? 'F' : 'f';
        case KEY_G: return shift ? 'G' : 'g';   case KEY_H: return shift ? 'H' : 'h';
        case KEY_I: return shift ? 'I' : 'i';   case KEY_J: return shift ? 'J' : 'j';
        case KEY_K: return shift ? 'K' : 'k';   case KEY_L: return shift ? 'L' : 'l';
        case KEY_M: return shift ? 'M' : 'm';   case KEY_N: return shift ? 'N' : 'n';
        case KEY_O: return shift ? 'O' : 'o';   case KEY_P: return shift ? 'P' : 'p';
        case KEY_Q: return shift ? 'Q' : 'q';   case KEY_R: return shift ? 'R' : 'r';
        case KEY_S: return shift ? 'S' : 's';   case KEY_T: return shift ? 'T' : 't';
        case KEY_U: return shift ? 'U' : 'u';   case KEY_V: return shift ? 'V' : 'v';
        case KEY_W: return shift ? 'W' : 'w';   case KEY_X: return shift ? 'X' : 'x';
        case KEY_Y: return shift ? 'Y' : 'y';   case KEY_Z: return shift ? 'Z' : 'z';
        case KEY_1: return shift ? '!' : '1';   case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';   case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';   case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';   case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';   case KEY_0: return shift ? ')' : '0';
        case KEY_SPACE:      return ' ';
        case KEY_MINUS:      return shift ? '_' : '-';
        case KEY_EQUAL:      return shift ? '+' : '=';
        case KEY_LEFTBRACE:  return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_SEMICOLON:  return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_GRAVE:      return shift ? '~' : '`';
        case KEY_BACKSLASH:  return shift ? '|' : '\\';
        case KEY_COMMA:      return shift ? '<' : ',';
        case KEY_DOT:        return shift ? '>' : '.';
        case KEY_SLASH:      return shift ? '?' : '/';
        default:             return 0;
    }
}

void NanoMenu::armAppLaunchTrigger(const std::string& syncPkg) {
    mLaunchHandOffMs = elapsedRealtime();
    if (!mOverlayMode)
        emuSettingsSyncLaunch(!syncPkg.empty() ? syncPkg : android::base::GetProperty("sys.gammaos.nano.launch_app", ""));
    if (!mOverlayMode && !sDrmActive) { mLaunchTriggerDeferred = true; return; }
    property_set("service.bootanim.nano_retroarch", "1");
}

// The grabbing home's power hold: the Power menu opens at 1.5 s, and a hold to 7 s shuts down, as
// PhoneWindowManager does where it sees the key (nano-powerwatch backs both up).
// The Anbernic button (the device's function button, or Back where it doubles as one: devkeys) is
// down, read from the devices; *fdOut gets the one it is down on.
bool NanoMenu::anbernicPhysicallyDown(int* fdOut) {
    const std::vector<int> codes = devkeys::current().modifierCodes();
    for (int fd : mInputFds) {
        unsigned char state[KEY_MAX / 8 + 1] = {};
        if (ioctl(fd, EVIOCGKEY(sizeof(state)), state) < 0) continue;
        for (int code : codes) {
            if (state[code / 8] & (1 << (code % 8))) { if (fdOut) *fdOut = fd; return true; }
        }
    }
    return false;
}

void NanoMenu::powerHoldTick() {
    if (mPowerHoldMs == 0 || android::uptimeMillis() - mPowerHoldMs < 7000) return;
    mPowerHoldMs = 0;
    bool down = false;
    for (int fd : mInputFds) {
        unsigned char state[KEY_MAX / 8 + 1] = {};
        if (ioctl(fd, EVIOCGKEY(sizeof(state)), state) >= 0 && (state[KEY_POWER / 8] & (1 << (KEY_POWER % 8))))
            down = true;
    }
    if (!down) return;   // its release was read elsewhere
    ALOGI("NanoMenu: power held 7 s -> shut down");
    prepareShutdown("shutdown");
}

void NanoMenu::pollInput() {
    powerHoldTick();
    functionHomeTick();
    // Overlay launch transition: while a launch is pending (the overlay is held up
    // until the new app resumes), FREEZE the XMB - drain and ignore all input so the
    // user cannot keep navigating the menu while the app is starting (which looked
    // like the launch had not registered). The overlay dismisses onto the app once
    // it is ready (overlayPoll). Draining keeps stale presses from flushing later.
    // The boot screen covers a home that is already live: nothing may drive it unseen.
    // The power button still works there, on a home that handles it itself (it grabs the input): its
    // press ends the boot screen and is handled as usual, its release left queued for that.
    if ((mOverlayMode && mOverlayLaunchPending) || mBootScreenActive) {
        const bool bootPower = mBootScreenActive && !mOverlayMode
                               && android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false);
        bool powerPressed = false;
        struct input_event dev;
        for (int fd : mInputFds) {
            if (fd < 0) continue;
            while (read(fd, &dev, sizeof(dev)) == (ssize_t)sizeof(dev)) {
                ssReleaseTaken(dev);
                if (bootPower && dev.type == EV_KEY && dev.code == KEY_POWER && dev.value == 1) {
                    powerPressed = true;
                    break;
                }
            }
        }
        property_set("sys.gammaos.nano.nav", "");   // swallow scripted nav too
        touchResync(-1, true);
        if (powerPressed) {
            ALOGI("bootscreen: power pressed, closing it");
            bootScreenClose();
            mKeyReplay.push_back({KEY_POWER, 1});
        }
        return;
    }
    if (panelAppInputGate()) return;   // dual screen: the panel's app has the controller
    // Home (non-overlay) launch fade-out: once the launching select has been
    // released (mLaunchFadeStart stamped in the wait-for-release handler below),
    // hold the hand-off to the app until the XMB has faded to black (render() draws
    // the ramp), then exit. Makes launching a game/app a smooth fade, not a cut.
    // The DSi theme plays a longer launch effect (tile lift + sparkle ring + a 44f white
    // wash) before handing off, so it holds the exit until the wash has fully covered the
    // screen (~47 frames); every other theme keeps the quick 260ms fade.
    urlLaunchTick();
    if (mLaunchFadeStart > 0 && !mUrlLaunchPending &&
        (int64_t)uptimeMillis() - mLaunchFadeStart >= launchFadeHoldMs() && !launchHeldByFinger()) {
        // DSi theme: persist the carousel nav path so the fresh return process comes back to
        // the exact launched card (once, right before we hand off).
        if (mNdsTheme && mPs3Xmb) ndsSaveReturnPath();
        if (mEsdeTheme || mEsdeSettingsMinima) esdeSaveReturnPath();   // the same for the ES-DE home's system and game
        // SF-composited home: the app start was deferred so its window could not cover the
        // effect (armAppLaunchTrigger); fire it now that the effect has completed.
        if (mLaunchTriggerDeferred) {
            mLaunchTriggerDeferred = false;
            property_set("service.bootanim.nano_retroarch", "1");
        }
        mExitRequested = true;
    }
    // Deferred wrong-password re-prompt: wifiConnectWatch (a detached watch thread)
    // arms mWifiRepromptPending when a just-tried key was rejected. Consume it here
    // on the input thread so opening the password OSK is main-thread-safe, and only
    // while the user is still on the Wi-Fi screen with nothing else in front.
    if (mWifiRepromptPending.load(std::memory_order_relaxed)
        && mMenuState == MENU_WIFI && !mOskActive && !mWifiManageActive) {
        mWifiRepromptPending.store(false, std::memory_order_relaxed);
        mWifiPendingSsid     = mWifiRepromptSsid;
        mWifiPendingSecurity = mWifiRepromptSecurity;
        char promptBuf[192];
        snprintf(promptBuf, sizeof(promptBuf), trDyn("Re-enter Wi-Fi password \"%s\""), mWifiRepromptSsid.c_str());
        std::string prompt = promptBuf;
        openOskForPassword(prompt, [this](const std::string& pw) {
            if (pw.empty()) return;
            addAndConnectWifi(mWifiPendingSsid, mWifiPendingSecurity, pw, true);
        });
        mOskPlaintext = true;   // show the key in the clear so a stray char is visible
    }
    // Test navigation hook: `setprop sys.gammaos.nano.nav <action>` injects one
    // nav action (left/right/up/down/enter/back) then clears the prop. The
    // device analog of the web app's simulateInput, used for scripted on-device
    // 1:1 verification. No effect when the prop is empty (one cheap read/frame).
    {
        // The hook is empty for the entire life of a normal session. Reading the
        // property's serial (a cheap pointer-deref) and only doing the full read +
        // dispatch when it advances keeps this at ~one integer compare per frame
        // instead of a full name lookup. The serial bumps on every set (including
        // the self-clear below), so a real injection is never missed. We recapture
        // the serial after any dispatch+clear so the bookkeeping does not chase our
        // own write. The prop is created lazily on first set, so retry find while null.
        static const prop_info* sNavPi = nullptr;
        static uint32_t sNavSerial = 0;
        if (!sNavPi) sNavPi = __system_property_find("sys.gammaos.nano.nav");
        if (sNavPi && __system_property_serial(sNavPi) != sNavSerial) {
        char navbuf[PROPERTY_VALUE_MAX];
        if (property_get("sys.gammaos.nano.nav", navbuf, "") > 0 && navbuf[0]) {
            mLastInputMs = android::uptimeMillis();   // scripted nav = activity
            if      (!strcmp(navbuf, "left"))  handleLeft();
            else if (!strcmp(navbuf, "right")) handleRight();
            else if (!strcmp(navbuf, "up"))    handleUp();
            else if (!strcmp(navbuf, "down"))  handleDown();
            // ES-DE options menu toggle (the Start button), scriptable for 1:1 verification.
            else if (!strcmp(navbuf, "menu")) {
                if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuClose();
                else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeMenuOpen();
            }
            else if (!strcmp(navbuf, "enter")) {
                handleSelect();
                // A scripted select has no key release, so stamp the launch effect here (as the
                // touch-tap path does) or a scripted launch never fades and never hands off.
                if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0)
                    mLaunchFadeStart = uptimeMillis();
            }
            else if (!strcmp(navbuf, "back"))  handleBack();
            // Turn File Transfer on (test hook: it is the same call the USB rows make).
            else if (!strcmp(navbuf, "mtp"))   setUsbMode("mtp");
            // Quick Menu > System Settings, the same call its row makes (test hook for the hand-off path).
            else if (!strcmp(navbuf, "settings")) launchAndroidSettings();
            // System Update > Update via Internet, the same call the chooser makes. With online updates
            // off (nano::kOnlineOtaEnabled) that opens the Storage Media flow, as every entry does.
            else if (!strcmp(navbuf, "ota_internet")) { if (!mOtaFlowActive) startOtaFlow(true); }
            // OSK scripting for 1:1 verification: `type:<text>` inserts each ASCII
            // character at the caret, `submit` commits the on-screen keyboard.
            else if (!strncmp(navbuf, "type:", 5)) {
                if (mOskActive) for (const char* p = navbuf + 5; *p; ++p) oskType(*p);
            }
            else if (!strcmp(navbuf, "submit")) { if (mOskActive) oskConfirm(); }
            // Now-Playing scripting for 1:1 verification: `tri` toggles the
            // control panel (physical Triangle/BTN_NORTH), `sq` cycles the
            // visualizer (physical Square/BTN_WEST). No effect off Now-Playing.
            else if (!strcmp(navbuf, "tri")) {
                if (mVidActive) { if (!mVidOpenInProgress.load(std::memory_order_relaxed) && !mVidGoToOpen && !mVidSceneOpen && !mVidResumeAsk) vidPanelToggle(); }
                else if (mMpActive) { if (mMpCpOpen) closeMpOpt(); else openMpOpt(); }
                else if (mPvActive) { if (mPvPanel) closePvPanel(); else openPvPanel(); }
                else if (mPs3Xmb) { if (mPs3OptActive) closeXmbOpt(); else openXmbOpt(); }
            }
            // Power submenu shortcut: PhoneWindowManager sets this on a power-HOLD when the
            // overlay / wallpaper home is visible (nano does not own the power key there), to open
            // the Quick Menu Power submenu - the grabbing DRM home calls openQuickPowerMenu()
            // directly from its own power path. Only meaningful when the menu is on screen.
            else if (!strcmp(navbuf, "powermenu")) {
                // PhoneWindowManager only fires this when the overlay/wallpaper home is on screen
                // with no foreground app, so the menu is guaranteed visible here.
                if (mPs3Xmb) openQuickPowerMenu();
            }
            else if (!strcmp(navbuf, "sq")) {
                if (mMpActive && !mOskActive) mpCycleVis();
                else if (mPs3Xmb && !mOskActive && !mPvActive && !mPs3OptActive && !mPs3DlgActive
                         && ps3TopScreenKind() == PHOTO_GRID) photoSortCycleY();   // Y: cycle Sort By on the grid
                else if (!mPvActive && mPs3Xmb && !mOskActive && mPs3Stack.empty()
                         && !mPs3OptActive && !mPs3DlgActive && mPhotoLoaded
                         && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                         && mPs3Cats[mPs3CatIdx].name == "Photo") photoSortCycleY();  // Y: cycle Sort By on the column
                else if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mPs3OptActive && !mPs3DlgActive
                         && !mVideoTest && mVideoLoaded && !mVideos.empty()
                         && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                         && mPs3Cats[mPs3CatIdx].name == "Video") videoSortCycleY();   // Y: cycle Sort By on the Video column
                else if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mMpActive && !mPs3OptActive && !mPs3DlgActive
                         && mMusicLoaded && !mMusicTracks.empty()
                         && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                         && mPs3Cats[mPs3CatIdx].name == "Music") musicSortCycleY();    // Y: cycle Sort By on the Music column
                else if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mPs3OptActive && !mPs3DlgActive
                         && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                         && mPs3Cats[mPs3CatIdx].name == "Game") gameSortCycleY();       // Y: cycle the system tile order on the Game column
            }
            // Game Systems list scripting: l1/r1 reorder the selected system,
            // x toggles its enabled state (the physical L1/R1/X buttons do the
            // same; the nav hook only injects dpad/A/B so these widen it).
            else if (!strcmp(navbuf, "l1") || !strcmp(navbuf, "r1") || !strcmp(navbuf, "x")) {
                if (mPs3Xmb && ps3TopScreenKind() == GS_LIST && !mPs3Stack.empty()) {
                    auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                    if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_GS_SYSTEM_ROW) {
                        if (!strcmp(navbuf, "l1"))      gsReorderSystem(its[sel].a, -1);
                        else if (!strcmp(navbuf, "r1")) gsReorderSystem(its[sel].a, +1);
                        else                            gsToggleSystem(its[sel].a);
                    }
                }
                // Home Categories editor: same interaction as the Game Systems list
                // (l1/r1 reorder the selected category, x toggles Shown/Hidden).
                else if (mPs3Xmb && ps3TopScreenKind() == CAT_ORDER && !mPs3Stack.empty()) {
                    auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                    if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_CATORDER_ROW) {
                        if (!strcmp(navbuf, "l1"))      catOrderReorder(its[sel].a, -1);
                        else if (!strcmp(navbuf, "r1")) catOrderReorder(its[sel].a, +1);
                        else                            catOrderToggle(its[sel].a);
                    }
                }
                // Per-item show/hide editor: x toggles the focused item Shown/Hidden. No reorder
                // (static rows keep their source order), so l1/r1 are inert here.
                else if (mPs3Xmb && ps3TopScreenKind() == ITEM_HIDE && !mPs3Stack.empty()) {
                    auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                    if (!strcmp(navbuf, "x") && sel >= 0 && sel < (int)its.size()
                        && its[sel].kind == PS3_ITEMHIDE_ROW)
                        itemHideToggle(its[sel].payloadStr);
                }
                // DSi / Minima drilled game/media list: l1/r1 fast page-skip (bumper scrolling),
                // mirroring the physical L1/R1. x has no effect here.
                else if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && !mNdsAtRoot
                         && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive) {
                    if (!strcmp(navbuf, "l1"))      ndsBumperSkip(-1);
                    else if (!strcmp(navbuf, "r1")) ndsBumperSkip(+1);
                }
                // Pure PS3 XMB: l1/r1 nav-hook page-skip the focused column/submenu.
                else if (mPs3Xmb && ps3TopScreenKind() == 0 && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive) {
                    if (!strcmp(navbuf, "l1"))      ps3XmbBumperSkip(-1);
                    else if (!strcmp(navbuf, "r1")) ps3XmbBumperSkip(+1);
                }
            }
            // Setup-wizard scripting for 1:1 verification: jump straight into the WiFi Internet
            // Connection wizard / the Manage Bluetooth wizard (bypasses deep menu navigation so the
            // DSi wizard reskin can be captured + tuned headlessly).
            else if (!strcmp(navbuf, "wizard")) { if (!mPs3WizActive) startNetWizard(); }
            else if (!strcmp(navbuf, "btwiz"))  { if (!mPs3WizActive) startBtWizard(0); }
            // Global search scripting: "search" opens the query keyboard (physical
            // Select); "search:<query>" runs the search directly (bypasses the OSK so
            // the categorized results overlay can be verified headlessly).
            else if (!strcmp(navbuf, "search")) { if (mPs3Xmb) gsearchOpen(); }
            else if (!strncmp(navbuf, "search:", 7)) { if (mPs3Xmb) gsearchBuild(std::string(navbuf + 7)); }
            // Video playback scripting: "vidplay:<substr>" opens the first library video whose
            // path contains <substr> directly in the player (bypasses column navigation so the
            // .ts/demuxer A/V path can be verified headlessly without resume-position drift).
            else if (!strncmp(navbuf, "vidplay:", 8)) {
                if (mPs3Xmb) {
                    videoEnsureLoaded();
                    std::string sub(navbuf + 8);
                    int found = -1;
                    for (size_t i = 0; i < mVideos.size(); i++)
                        if (mVideos[i].file.find(sub) != std::string::npos) { found = (int)i; break; }
                    if (found >= 0) {
                        std::vector<Ps3Item> one(1);
                        one[0].kind = PS3_VIDEO_FILE; one[0].a = found;
                        openVideoPlayer(one, 0);
                    }
                }
            }
            // Video Scene Search scripting: "vidscene" opens the chapter grid directly (bypasses the
            // control-panel grid nav), "vidchap:<n>" seeks to chapter n (0-based) so per-chapter preview
            // thumbnails can be populated + captured headlessly for 1:1 verification.
            else if (!strcmp(navbuf, "vidscene")) {
                if (mVidActive && !mVidSceneOpen && !mVidGoToOpen && !mVidOpenInProgress.load(std::memory_order_relaxed)) {
                    if (mVidCpOpen) vidPanelClose();
                    vidSceneOpen();
                }
            }
            else if (!strncmp(navbuf, "vidchap:", 8)) {
                if (mVidActive && !mVidOpenInProgress.load(std::memory_order_relaxed))
                    vidJumpToChapter(atoi(navbuf + 8));
            }
            // Live-stream scripting: "streamopen:<url>" opens an arbitrary IPTV/HLS URL directly
            // in the player (bypasses the channel-list navigation) so the live A/V pacing path
            // can be verified headlessly against a known live .ts/HLS stream.
            else if (!strncmp(navbuf, "streamopen:", 11)) {
                if (mPs3Xmb) {
                    videoEnsureLoaded();
                    std::vector<VidStreamRef> one(1);
                    one[0].url = std::string(navbuf + 11);
                    one[0].name = "Test Stream";
                    openIptvStream(one, 0);
                }
            }
            property_set("sys.gammaos.nano.nav", "");
        }
        // Recapture the serial AFTER the (possible) self-clear so the next frame is
        // a single integer compare and we do not re-fire on our own write.
        sNavSerial = __system_property_serial(sNavPi);
        }
    }
    // Bluetooth AVRCP media-control hook. NanoMediaBridge (system_server) owns an
    // AVRCP-eligible MediaSession and writes a one-shot transport command here on a
    // headphone/car button press; we dispatch it to the active player and self-clear,
    // mirroring the nav hook (one cheap serial compare per frame when idle).
    {
        static const prop_info* sMedPi = nullptr;
        static uint32_t sMedSerial = 0;
        if (!sMedPi) sMedPi = __system_property_find("sys.gammaos.nano.media");
        if (sMedPi && __system_property_serial(sMedPi) != sMedSerial) {
            char mbuf[PROPERTY_VALUE_MAX];
            if (property_get("sys.gammaos.nano.media", mbuf, "") > 0 && mbuf[0]) {
                mLastInputMs = android::uptimeMillis();   // AVRCP press counts as activity
                nanoMediaDispatch(mbuf);
                property_set("sys.gammaos.nano.media", "");
            }
            sMedSerial = __system_property_serial(sMedPi);
        }
    }
    // Publish now-playing state + metadata for the bridge (change-gated, zero writes when idle).
    nanoPublishMediaState();

    // Live controller Test / Calibration screens: keep redrawing so the analog
    // sticks/triggers animate, and exit on a ~1s Select hold (checked each frame
    // so it fires even when nothing else is moving).
    if (mGpTestActive || mGpCalibActive) {
        mDisplayDirty = true;
        if (mGpSelectDownMs && (long)android::uptimeMillis() - mGpSelectDownMs > 1000) {
            mGpTestActive = false; mGpCalibActive = false; mGpSelectDownMs = 0;
        } else if (mGpCalibActive) {
            gpCalibTick();
            // Slider steps also accept HAT0X (or the left stick) as left/right,
            // edge-latched so one flick = one step.
            if (mGpCalStep == 4 || mGpCalStep == 5) {
                float navX = gpAxisNorm(ABS_HAT0X);
                if (fabsf(navX) < 0.3f) navX = gpAxisNorm(ABS_X);
                if (fabsf(navX) < 0.4f) mGpCalNavLatch = 0;
                else if (mGpCalNavLatch == 0) { gpCalibNext(navX < 0 ? -1 : 1); mGpCalNavLatch = 1; }
            }
        }
    }

    // Colour picker: keep redrawing (live cursor), move the cursor from held input, and exit
    // (cancel) on a ~1s SELECT hold - the same hold-to-exit as the Test/Calibration screens.
    if (mCpActive) {
        mDisplayDirty = true;
        if (mGpSelectDownMs && (long)android::uptimeMillis() - mGpSelectDownMs > 1000) {
            mCpActive = false; mCpBinding = nullptr; mGpSelectDownMs = 0;
        } else {
            colorPickerTick();
        }
    }

    struct input_event ev;
    for (int fd : mInputFds) {
        while (nextInputEvent(fd, ev)) {
            launchStartNote();   // before this event can change mLastInputTouch
            mCurrentInputFd = fd;   // which device this event came from (Control Center touch routing)
            // The kernel dropped this device's queue: the rest of the packet is incomplete, so it is
            // skipped through the next SYN_REPORT, as Android's InputReader does.
            if (!mKeyReplaying) {
                if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                    inputEventsLost(fd);
                    mInputResync.insert(fd);
                    continue;
                }
                if (mInputResync.count(fd)) {
                    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                        mInputResync.erase(fd);
                        // The panel is read now, as Android's InputReader does: the packets after this
                        // one build on its slot and contacts.
                        auto rs = std::find(mTouchResyncFds.begin(), mTouchResyncFds.end(), fd);
                        if (rs != mTouchResyncFds.end()) { mTouchResyncFds.erase(rs); touchResync(fd, true); }
                    }
                    continue;
                }
            }
            // Any button edge counts as user activity for the idle frame-rate
            // timer. EV_KEY only fires on real state changes, so this cannot
            // be kept alive by analog-stick noise (sticks go through navPress
            // which is edge-triggered past the deadzone).
            if (ev.type == EV_KEY) mLastInputMs = android::uptimeMillis();
            if (!mKeyReplaying && ev.value != 0 && ((ev.type == EV_KEY && ev.value == 1)
                    || (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y))))
                mLastPressMs = android::uptimeMillis();
            // The Anbernic button's own state, for Anbernic + Volume (brightness). Its release can be
            // missed (inputEventsLost, the overlay raise and hide clear it).
            if (ev.type == EV_KEY && devkeys::isModifierCode(ev.code) && ev.value != 2) {
                mAnbernicHeld = (ev.value != 0);
                mAnbernicFd = fd;
            }
            // GammaOS hardware rotation key: while the overlay/home owns input (over a running app
            // the framework drops it via drop_input, so PhoneWindowManager never sees the swivel
            // switch), handle the rotate key here too. Setting sys.gammaos.rotate.state drives BOTH
            // nano's own overlay render (overlayUpdateSurfaceSize reads it) and the app underneath
            // (DisplayRotation forces the angle from the same prop via its poll). Ignore key-repeat.
            // GammaOS PSP slide clock: when persist.gammaos.nano.pspclock is set, the
            // same swivel key (KEY_F12) drives the full-screen PSP clock instead of the
            // display rotation - down = open, up = close (momentary, like closing a PSP
            // Go). Handled before the rotate branch so the clock gate takes precedence.
            // Gate: respond to the slide with the clock when the pspclock overlay toggle is
            // on (it can run alongside a rotate action, so it is its own toggle) OR the
            // slide's own down-action is "clock" (the clock is the chosen slide behaviour,
            // e.g. a plain button with no panel rotation). Computed only for the slide key.
            // The physical slide/swivel/fold trigger is NOT the same event on every device, so it
            // is fully prop-configurable (event type + code + active value):
            //   persist.gammaos.rotate.key_type   evdev EV_* type: 1 = EV_KEY (default), 5 = EV_SW
            //   persist.gammaos.rotate.key_code   evdev code:      88 = KEY_F12 (default), 1 = SW_TABLET_MODE
            //   persist.gammaos.rotate.key_active ev.value meaning "engaged": 1 (default; set 0 for
            //                                     an inverted switch)
            // Most devices report the swivel as KEY_F12 (EV_KEY); some (e.g. the TrimUI) report a
            // switch, EV_SW SW_TABLET_MODE. The defaults match the KEY_F12 devices exactly.
            // slideVal: 1 = engaged (rotated / clock open), 0 = released. EV_KEY auto-repeat
            // (value 2) is not a state change and is ignored; an EV_SW switch has no repeat.
            const int trigType = property_get_int32("persist.gammaos.rotate.key_type", EV_KEY);
            const int trigCode = property_get_int32("persist.gammaos.rotate.key_code", 88);
            const int trigActive = property_get_int32("persist.gammaos.rotate.key_active", 1);
            bool slideTrigger = false; int slideVal = 0;
            if (ev.type == trigType && ev.code == trigCode
                && !(trigType == EV_KEY && ev.value == 2)) {
                slideTrigger = true;
                slideVal = (ev.value == trigActive) ? 1 : 0;
            }
            // Device filter: when persist.gammaos.rotate.dev_name names a specific
            // input device, only honour the slide from the fd whose EVIOCGNAME matches.
            // This lets a device with several event nodes reporting the same code/type
            // (e.g. a shared gpio-keys) pin the swivel to the intended one. Empty =
            // any device. Cheap: only queried on the frames the trigger actually fired.
            if (slideTrigger) {
                char devName[PROPERTY_VALUE_MAX] = {};
                property_get("persist.gammaos.rotate.dev_name", devName, "");
                if (devName[0]) {
                    char nm[256] = {0};
                    if (ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm) < 0 || strcmp(nm, devName) != 0)
                        slideTrigger = false;
                }
            }
            bool pspClockSlide = false;
            if (slideTrigger) {
                pspClockSlide = property_get_bool("persist.gammaos.nano.pspclock", false);
                if (!pspClockSlide && property_get_bool("persist.gammaos.rotate.enabled", false)) {
                    char da[PROPERTY_VALUE_MAX] = {};
                    property_get("persist.gammaos.rotate.down_action", da, "rotate");
                    pspClockSlide = slideActionHas(da, "clock");
                }
            }
            if (pspClockSlide) {
                // Re-roll the entrance-avalanche seed on each fresh open (false->true)
                // so the burst/icon stream differs per swivel, like the web's per-run
                // hashIconRnd. Only on the down transition, not key-repeat/close.
                if (slideVal == 1 && !mPspClockOn) mPspIconSeed += 17;
                if (slideVal == 1) esdeOverlayCloseAll();   // it would hide the clock
                mPspClockOn = (slideVal == 1);
                // DSi/Minima have no XMB category rail or wave that legitimately sits behind the
                // clock, so the slide clock is ALWAYS a standalone overlay there. This evdev
                // handler runs for the home / wallpaper mode (nano is the live surface, not parked
                // behind an app); without tagging standalone, renderPs3Xmb would run the full XMB
                // entrance (the category icons fade in and the wave composites) to show the clock -
                // i.e. it "switches to the XMB menu" instead of dropping the clock cleanly over the
                // dimmed home wallpaper. Tag standalone on open so the XMB chrome is suppressed;
                // drawPspClock's teardown clears it at reveal 0. The over-app summon already tags
                // standalone via overlayShow, and pure XMB (both theme flags false) keeps its own
                // chrome/wave as the real home behind the clock.
                if (slideVal == 1 && (mNdsTheme || mMinimaTheme)) mPspClockStandalone = true;
                // Ambient-glyph brighten/speed surge just after a toggle (web
                // index.html:16476 sets this on togglePspClock). 1.0 on open, 0.6 on close.
                mPspGlyphBurst = mPspClockOn ? 1.0f : 0.6f;
                // If hardware rotation is ALSO enabled, the swivel must STILL rotate:
                // nano is the only latch-free writer of sys.gammaos.rotate.state while an
                // app/overlay is up (the framework drops F12 in InputDispatcher when the
                // app has focus, so the framework writer stops and rotation gets stuck -
                // user bug #7). Drive the prop here before swallowing so a device with
                // BOTH the PSP clock and the rotate feature keeps rotating in every mode.
                if (property_get_bool("persist.gammaos.rotate.enabled", false)) {
                    char act[PROPERTY_VALUE_MAX] = {};
                    property_get(slideVal ? "persist.gammaos.rotate.down_action"
                                          : "persist.gammaos.rotate.up_action",
                                 act, slideVal ? "rotate" : "natural");
                    if (slideActionHas(act, "rotate"))       property_set("sys.gammaos.rotate.state", "1");
                    else if (slideActionHas(act, "natural")) property_set("sys.gammaos.rotate.state", "0");
                }
                continue;   // swallow so the switch never navigates the menu
            }
            if (slideTrigger
                && property_get_bool("persist.gammaos.rotate.enabled", false)) {
                char act[PROPERTY_VALUE_MAX] = {};
                property_get(slideVal ? "persist.gammaos.rotate.down_action"
                                      : "persist.gammaos.rotate.up_action",
                             act, slideVal ? "rotate" : "natural");
                if (!strcmp(act, "rotate"))       property_set("sys.gammaos.rotate.state", "1");
                else if (!strcmp(act, "natural")) property_set("sys.gammaos.rotate.state", "0");
                // screenoff/wake/launch/none are framework-side; nano only drives rotation here.
                continue;   // swallow so the switch never navigates the menu
            }
            // The touchscreen's own events pass the captures below: their hints take touch.
            const bool touchEv = (ev.type == EV_KEY && ev.code == BTN_TOUCH) ||
                                 (ev.type == EV_ABS && ev.code >= ABS_MT_SLOT && ev.code <= ABS_MT_TOOL_Y);
            // A controller button or d-pad press: the keyboard it opens is the controller one.
            if (!mKeyReplayTouch && ((ev.type == EV_KEY && ev.value == 1 && ev.code >= BTN_MISC
                 && !(ev.code >= BTN_MOUSE && ev.code < BTN_JOYSTICK) && !(ev.code >= BTN_DIGI && ev.code < BTN_WHEEL))
                || (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) && ev.value != 0)))
                mLastInputTouch = false;
            // Any key press but a touch or a click: a touch press still waiting would act on what the
            // key changed.
            if (!mKeyReplayTouch && ev.type == EV_KEY && ev.value == 1 && ev.code != BTN_TOUCH
                && !(ev.code >= BTN_MOUSE && ev.code < BTN_JOYSTICK) && !(ev.code >= BTN_DIGI && ev.code < BTN_WHEEL))
                mTouchPressAct = nullptr;
            // Test / Calibration screen: mirror every raw button/axis into the
            // live-state maps for the visualisation and swallow it so it does not
            // navigate the menu behind. gpScreenHandleKey drives the hold-to-exit.
            // Custom button-action capture: latch the first press to bind a
            // mapping. Same raw-evdev interception as the Test/Calib screens.
            if (mGpCaptureActive && !touchEv &&
                (ev.type == EV_KEY || ev.type == EV_ABS)) {
                ssReleaseTaken(ev);
                gpCaptureEvent(fd, ev.type, ev.code, ev.value);
                if (ev.type == EV_KEY) gpCaptureHandleKey(ev.code, ev.value);
                continue;
            }
            if (!mKeyReplaying) nanoabxy::unswap(fd, ev);   // the capture above records the raw code
            if ((mGpTestActive || mGpCalibActive) && !touchEv &&
                (ev.type == EV_KEY || ev.type == EV_ABS)) {
                ssReleaseTaken(ev);
                gpCaptureEvent(fd, ev.type, ev.code, ev.value);
                if (ev.type == EV_KEY) gpScreenHandleKey(ev.code, ev.value);
                continue;
            }
            // Colour picker: mirror raw button/axis into the live-state maps (colorPickerTick
            // reads them for smooth hue/brightness movement) and consume the event so it does
            // not navigate the menu behind. Press edges (confirm/cancel/sat) go to handleKey.
            if (mCpActive && !touchEv && (ev.type == EV_KEY || ev.type == EV_ABS)) {
                ssReleaseTaken(ev);
                gpCaptureEvent(fd, ev.type, ev.code, ev.value);
                if (ev.type == EV_KEY) colorPickerHandleKey(ev.code, ev.value);
                continue;
            }
            // From here on the device's function button reads as KEY_F10 and its Back button as
            // KEY_BACK, whatever codes they send (the pickers above record the raw code).
            const bool homeFnKey = ev.type == EV_KEY && devkeys::isHomeFunctionCode(ev.code);
            if (ev.type == EV_KEY && ev.code != KEY_F10 && devkeys::isFunctionCode(ev.code)) ev.code = KEY_F10;
            else if (ev.type == EV_KEY && ev.code != KEY_BACK && devkeys::current().isBack(ev.code)) ev.code = KEY_BACK;
            // The button map takes every key while it is up, power and volume aside (NanoMenuButtonMap.cpp).
            if (mBmap && bmapKey(fd, ev.type, ev.code, ev.value)) continue;
            if (!mKeyReplaying) {
                if (ev.type == EV_KEY && ev.code == KEY_BACK) {
                    if (backKey(ev.value)) continue;
                } else if (mBackHeld && ev.value != 0
                           && ((ev.type == EV_KEY && ev.value == 1)
                               || (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)))) {
                    mBackUsed = true;
                }
            }
            // Anbernic + the Switch Screen Button (the top button) moves the controller between the
            // screens in dual screen mode; over an app PhoneWindowManager reads it (screenSwapPoll).
            if (!mKeyReplaying && mAnbernicHeld && ev.type == EV_KEY && ev.value == 1 && (!mOverlayMode || mOverlayShown)
                && anbernicPhysicallyDown() && screenChordKey(fd, ev.code)) {
                mAnbernicArmed = false;
                screenSwap();
                continue;
            }
            // Anbernic + the Button Map Button (the bottom button) opens the button map.
            if (!mKeyReplaying && mAnbernicHeld && ev.type == EV_KEY && ev.value == 1 && anbernicPhysicallyDown()
                && bmapChordKey(fd, ev.code, ev.value)) {
                mAnbernicArmed = false;
                continue;
            }
            // Anbernic + Select turns the display shader on or off (the selected shader is kept), and
            // Anbernic + Start opens or closes the Applications view. The Quick Menu does not open on
            // the Anbernic button's release; the button's release is taken by startSelectKey.
            if (!mKeyReplaying && mAnbernicHeld && ev.type == EV_KEY && ev.value != 0
                && (ev.code == BTN_SELECT || ev.code == BTN_START)
                && anbernicPhysicallyDown()) {
                mAnbernicArmed = false;
                if (ev.value == 1 && ev.code == BTN_SELECT) {
                    const bool on = property_get_bool("persist.gammaos.shader.enable", false);
                    // A persist write waits for init's fsync: not on the render thread.
                    std::thread([on] { property_set("persist.gammaos.shader.enable", on ? "0" : "1"); }).detach();
                    ALOGI("NanoMenu: Anbernic+Select -> display shader %s", on ? "off" : "on");
                    mDisplayDirty = true;
                } else if (ev.value == 1 && mPs3Xmb && !mWaitForRelease && mLaunchFadeStart == 0
                           && !(mPspClockOn || mPspClockReveal > 0.0f)) {
                    if (applicationsViewOpen()) closeApplicationsView();
                    else openApplicationsView();
                }
                continue;
            }
            // A button pressed while the Anbernic button is held makes it a chord: the Quick Menu does
            // not open on its release, and the chord's button does nothing here. A Volume key still
            // goes on, as Anbernic + Volume (brightness).
            if (mAnbernicArmed && ev.value != 0 && ev.type == EV_KEY
                && (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)) {
                mAnbernicArmed = false;
            } else if (mAnbernicArmed && ev.value != 0
                && ((ev.type == EV_KEY && ev.code != KEY_F10 && ev.code != BTN_MODE && ev.code != BTN_TOUCH
                     && ev.code != KEY_POWER)
                    || (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)))) {
                mAnbernicArmed = false;
                continue;
            }
            // Start + Select + the Button Map Button opens it too, in the menus only (bmapChordKey).
            if (!mKeyReplaying && mSsHeld[0] && mSsHeld[1] && ev.type == EV_KEY && ev.value == 1
                && ev.code != BTN_START && ev.code != BTN_SELECT && bmapChordKey(fd, ev.code, ev.value)) {
                mSsUsed = true;
                continue;
            }
            if (!mKeyReplaying) {
                if (ev.type == EV_KEY && (ev.code == BTN_START || ev.code == BTN_SELECT)) {
                    if (startSelectKey(ev.code, ev.value)) continue;
                } else if ((mSsHeld[0] || mSsHeld[1]) && ev.value != 0
                           && ((ev.type == EV_KEY && ev.value == 1)
                               || (ev.type == EV_ABS && (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)))) {
                    mSsUsed = true;
                }
            }
            // Touchscreen -> OSK. Read straight from the shared evdev stream:
            // BTN_TOUCH gates finger down/up, ABS_MT_POSITION_X/Y carry the
            // position (digitizer range read lazily via EVIOCGABS), and
            // SYN_REPORT flushes one frame to the OSK hit-test. Only acts while
            // the OSK is up, so the touchscreen stays ignored elsewhere as before.
            if (touchEv) {
                // The vendor joypad driver's "virtual_gamepad" is a touchscreen of its own (it turns
                // buttons into screen taps for games); only real touchscreens drive Nano's touch state.
                auto tn = mInputFdNames.find(fd);
                if (tn != mInputFdNames.end() && tn->second == "virtual_gamepad") continue;
                if (mTouchInputOff < 0)
                    mTouchInputOff = android::base::GetBoolProperty("persist.gammaos.nano.touch_input", true) ? 0 : 1;
                if (mTouchInputOff) continue;   // Touch Input off: drop the touchscreen (a mouse still clicks)
                // The panel is off, or not showing the UI. In dual screen mode its own app takes the
                // touch itself (sys.gammaos.nano.touch_display; screenSwapPoll).
                if (mTargetMonitor && !mUiOnPanel) {
                    if (!mPanelApp.empty() && ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1)
                        mLastInputMs = android::uptimeMillis();
                    continue;
                }
                if (!touchPanel(fd).direct) continue;   // a pad's touchpad
                mTouchLastEvMs = mLastInputMs = android::uptimeMillis();   // a drag keeps the full frame rate
            }
            if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                // mTouchDown follows the gesture's panel (touchPanelsMirror, at the SYN_REPORT).
                TouchPanel& p = touchPanel(fd);
                p.btn = (ev.value != 0);
                if (!p.btn && !p.mt) std::fill(p.id, p.id + kTouchSlots, -1);   // a slotless panel: all up
                if (ev.value) mLastInputTouch = true;
                mTouchFromPointer = false;   // a real finger now owns the touch, not the mouse
                continue;
            }
            // Multi-touch slot select (Type-B). The first kTouchSlots slots are tracked (the pinch and
            // the multi-finger taps); the primary (slot 0) drives the single-finger path.
            if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
                touchPanel(fd).slot = ev.value;
                continue;
            }
            if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
                TouchPanel& p = touchPanel(fd);
                if (p.mt && p.slot >= 0 && p.slot < kTouchSlots) p.id[p.slot] = ev.value;   // slotless: BTN_TOUCH only
                mTouchFromPointer = false;   // a real digitizer contact now owns the touch
                continue;
            }
            if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
                TouchPanel& p = touchPanel(fd);
                if (p.slot >= 0 && p.slot < kTouchSlots) p.sx[p.slot] = ev.value;   // slot 0: the primary finger (mirrored)
                mTouchFromPointer = false;                    // a real finger now owns the touch
                if (mTouchMaxX <= mTouchMinX) {
                    struct input_absinfo a{};
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0 && a.maximum > a.minimum) {
                        mTouchMinX = a.minimum; mTouchMaxX = a.maximum;
                    }
                }
                continue;
            }
            if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
                TouchPanel& p = touchPanel(fd);
                if (p.slot >= 0 && p.slot < kTouchSlots) p.sy[p.slot] = ev.value;
                if (mTouchMaxY <= mTouchMinY) {
                    struct input_absinfo a{};
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0 && a.maximum > a.minimum) {
                        mTouchMinY = a.minimum; mTouchMaxY = a.maximum;
                    }
                }
                continue;
            }
            // Mouse pointer: accumulate relative motion into a logical-pixel cursor, route the
            // wheel to list scrolling, and turn clicks into taps through the existing touch
            // hit-test. nano already opens+grabs any plugged-in mouse, so these events already
            // arrive here; without this they fall through unhandled. Standard relative mice use
            // EV_REL (absolute pointers report ABS_X/Y and go through the stick path).
            if (ev.type == EV_REL) {
                if (ev.code == REL_X || ev.code == REL_Y) {
                    if (ev.code == REL_X) mCursorX += (float)ev.value;
                    else                  mCursorY += (float)ev.value;
                    if (mCursorX < 0) mCursorX = 0; else if (mCursorX > mWidth)  mCursorX = mWidth;
                    if (mCursorY < 0) mCursorY = 0; else if (mCursorY > mHeight) mCursorY = mHeight;
                    mCursorVisible = true;
                    mLastPointerMs = mLastInputMs = android::uptimeMillis();
                    continue;
                }
                if (ev.code == REL_WHEEL && ev.value != 0) {
                    // One list step per detent, driven directly so a concurrently held d-pad
                    // direction's auto-repeat is not disturbed. Respect the PSP-clock input block.
                    // The touch keyboard has nothing to scroll (a step would swap in the controller one).
                    if (!(mPspClockOn || mPspClockReveal > 0.0f) && !(mOskActive && mOsk.touch)) {
                        if (ev.value > 0) handleUp(); else handleDown();
                    }
                    mCursorVisible = true;
                    mLastPointerMs = mLastInputMs = android::uptimeMillis();
                    continue;
                }
                continue;   // ignore other relative axes (REL_HWHEEL etc.)
            }
            // Mouse buttons: left = a tap at the cursor (routed via touchMapRaw's pointer
            // override on the next SYN_REPORT, so every per-screen tap hit-tester works), right
            // = back, middle = the option menu. Caught before the gamepad/nav switches so the
            // 0x110-0x112 codes are never misread as gamepad buttons.
            if (ev.type == EV_KEY &&
                (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE)) {
                mCursorVisible = true;
                mLastPointerMs = android::uptimeMillis();
                if (ev.code == BTN_LEFT) {
                    if (ev.value) mLastInputTouch = true;
                    // The setup wizard is button-driven (no touch handler), so a click there must
                    // not fall through to the hidden XMB home behind it.
                    if (!mSetupWizardActive) {
                        mTouchFromPointer = true;       // touchMapRaw hands back (mCursorX,mCursorY)
                        mTouchDown = (ev.value != 0);   // the following SYN_REPORT flushes the tap
                        mPointerFd = fd;
                    }
                } else if (ev.value == 1) {
                    if (ev.code == BTN_RIGHT) {
                        if (mSetupWizardActive) handleSetupBack(); else handleBack();
                    } else if (mPs3Xmb && !mOskActive && !mSetupWizardActive && !mBiosDlgActive
                               && !mEsdeViewerActive && !mEsdeInfoActive) {
                        openXmbOpt();   // middle = options
                    }
                }
                continue;
            }
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                // The raw-touch panel corrections (swap/flipX/flipY) are shared by the
                // OSK and the XMB touch navigation, so read them once regardless of
                // which is up - otherwise XMB touch before the OSK is ever opened would
                // run with uncorrected axes on DRM-rotated/flipped panels.
                if (!mOskTouchTuneRead) {
                    mOskTouchSwap  = android::base::GetBoolProperty("persist.gammaos.nano.osk_touch_swap", false);
                    mOskTouchFlipX = android::base::GetBoolProperty("persist.gammaos.nano.osk_touch_flipx", false);
                    mOskTouchFlipY = android::base::GetBoolProperty("persist.gammaos.nano.osk_touch_flipy", false);
                    mOskTouchTuneRead = true;
                }
                // Dual-screen SetupWizard: the bottom panel (the only touch digitizer on the RG DS) is
                // fully released and drawn black while setup runs; the entire wizard - net wizard, OSK/IME,
                // pickers - renders on the PRIMARY panel and is button/D-pad driven. Swallow all touch here
                // so a blind tap on the blank bottom panel cannot fire a phantom key/row press into that
                // now-hidden UI. Single-screen setup is unaffected (hasSecondaryDisplay()==false, its
                // on-panel OSK keeps touch).
                if (mSetupWizardActive && hasSecondaryDisplay()) {
                    continue;
                }
                // A gesture a second finger joined goes to no handler (the photo viewer's pinch aside).
                // A fresh press drops a deferred DSi press left from an earlier gesture (once the panel
                // state for this report is in mTouchDown).
                touchPanelsMirror();
                if (mTouchDown && !mTouchWasDown) mNdsDragMode = 0;
                if (multiTouchFrame()) {
                    // The photo viewer's pinch, where its branch below would get the touch.
                    const bool pvTop = mPvActive && !controlCenterActive() && !(mPspClockOn || mPspClockReveal > 0.0f)
                                       && !mOskActive && !mBiosDlgActive && !mPdfActive
                                       && !((mEsdeViewerActive || mEsdeInfoActive) && !mVidActive);
                    if (mTouchMulti && pvTop && mTouchDown && (touchContacts() >= 2 || mPvPinchActive)) pvTouchGesture();
                }
                // A launch under way: no touch handler acts (a finger resting through a key launch, or
                // the lift after a touch launch).
                else if (!mOverlayMode && (mWaitForRelease || mLaunchFadeStart != 0))
                                     mTouchWasDown = mTouchDown;
                // Bottom-screen Control Center (over a single-screen app): the CC owns touch on the
                // BOTTOM digitizer (tiles / sliders / wake-from-sleep). ccTouchFrame self-gates on the
                // bottom device fd, so top-panel touches fall through (harmless; the app is isolated).
                // The button map: drag, fling and swipe move it, a tap outside it closes it.
                else if (bmapActive()) bmapTouchFrame();
                else if (controlCenterActive())
                                     ccTouchFrame();
                // PSP clock up: its zones (a full-screen block + swipe up to dismiss) take the touch so the
                // XMB/menu behind cannot be driven (input block, #2); the clock follows the dragging finger.
                else if (mPspClockOn || mPspClockReveal > 0.0f)
                                     { pspClockTouchDrag(); touchZonesFrame(); }
                // The keyboard on the secondary (bottom) panel keeps its own hit-test; on the primary its
                // keys are zones.
                else if (mOskActive) { if (mOskOnSecondary) oskTouchFrame(); else touchZonesFrame(); }
                // The Game Settings BIOS dialog: its zones (on the RG DS it is on the top panel, button-driven).
                else if (mBiosDlgActive) { if (!hasSecondaryDisplay()) touchZonesFrame(); }
                else if (mPdfActive) { pdfTouchPan(); touchZonesFrame(); }   // a game's manual: drag pans, the rest are zones
                else if ((mEsdeViewerActive || mEsdeInfoActive) && !mVidActive)   // ES-DE media viewer / information
                                     touchZonesFrame();
                else if (mPvActive)  { if (!pvTouchGesture()) touchZonesFrame(); }   // photo viewer: pinch / pan / drag, then zones
                else if (mVidActive) touchZonesFrame();   // video player
                else if (mMpActive)  touchZonesFrame();   // music Now Playing
                // Standalone Wi-Fi screen (Settings > Network): its list and manage dialog register
                // touch zones. Must sit before the DSi carousel branches (ndsInModal does not cover MENU_WIFI).
                else if (mMenuState == MENU_WIFI && !mSetupWizardActive && !mPs3WizActive)
                                     touchZonesFrame();
                // DSi theme: its screens register touch zones on the primary panel. On a dual-screen
                // device (RG DS) they draw on the bottom panel, a secondary display, whose touch stays
                // on the DSi hit-testers.
                // DSi theme WiFi/Bluetooth setup wizard. Gated on mPs3WizActive (NOT mPs3Xmb) so it
                // also works in the first-run setup flow.
                else if (mNdsTheme && mPs3WizActive)
                                     { if (hasSecondaryDisplay()) ndsWizTouch(); else touchZonesFrame(); }
                // Minima theme WiFi/Bluetooth setup wizard: its painter registers touch zones. Must precede
                // the generic Minima branches below (the OSK touch branch runs earlier, for WK_TEXT).
                else if (mMinimaTheme && mPs3WizActive)
                                     touchZonesFrame();
                // DSi theme global search results (query OSK closed).
                else if (mNdsTheme && mPs3Xmb && mGSearchActive)
                                     { if (hasSecondaryDisplay()) gsearchTouch(); else touchZonesFrame(); }
                // DSi theme modals: the Triangle option menu + list/slider choosers are the DSi
                // settings-options list; confirm dialogs are the DSi message box.
                else if (mNdsTheme && mPs3Xmb && mPs3OptActive)
                                     { if (hasSecondaryDisplay()) ndsSidePanelTouch(); else touchZonesFrame(); }
                else if (mNdsTheme && mPs3Xmb && mPs3DlgActive)
                                     { if (!hasSecondaryDisplay()) touchZonesFrame();
                                       else if (ndsDlgIsSidePanel()) ndsSidePanelTouch(); else ndsDialogTouch(); }
                else if (mNdsTheme && mPs3Xmb && (mPs3TzActive || mPs3LangActive))   // Time Zone / System Language picker
                                     { if (hasSecondaryDisplay()) ndsPickerTouch(); else touchZonesFrame(); }
                else if (mNdsTheme && mPs3Xmb && !ndsInModal())
                                     { if (!hasSecondaryDisplay()) touchZonesFrame();
                                       else if (ndsCurLevelIsList()) ndsSubmenuTouch();   // settings screens: DSi list
                                       else                          ndsTouchFrame(); }   // app/game levels: stacked carousel
                // Minima theme (mMinimaTheme, also mPs3Xmb=true / mNdsTheme=false): the option menu,
                // dialog / info page and home list painters register touch zones.
                else if (mMinimaTheme && mPs3Xmb && mPs3OptActive)
                                     touchZonesFrame();
                else if (mMinimaTheme && mPs3Xmb && mPs3DlgActive)
                                     touchZonesFrame();
                else if (mMinimaTheme && mPs3Xmb && !ndsInModal())
                                     touchZonesFrame();
                // ES-DE home (system and game views, its menu): no Nano menu, dialog or option panel over it.
                else if (mEsdeTheme && mPs3Xmb && mPs3Stack.empty() && !mPs3OptClosing
                         && (!ndsInModal() || ((mEsdeMenuActive || mEsdeMenuClosing) && !mPs3OptActive && !mPs3DlgActive
                                               && !mOskActive && !mGSearchActive && !mPs3WizActive && !mPs3TzActive
                                               && !mPs3LangActive && !mPs3BrightSlider && !mScrapeProgActive))
                         && !mWaitForRelease && mLaunchFadeStart == 0 && !(mOverlayMode && mOverlayLaunchPending))
                                     touchZonesFrame();
                else                 touchZonesFrame();   // XMB home, option panel, dialogs, pickers, other modals
                // GammaOS touch-launch handoff safety net. A launch triggered by a touch tap sets
                // mWaitForRelease, but a touch has no physical select-key release, so the key-release
                // handler below never stamps the launch fade - and the non-overlay home instance
                // (mOverlayMode=false) then hangs ON TOP of the launched game, double-rotated and
                // unresponsive (the "cold-boot touch-launch: nano stays foreground" bug). The finger
                // is already up after a tap, so stamp the fade here for EVERY touch-launch path. Some
                // per-handler sites already do this (the DSi carousel, dialogs); this is the catch-all
                // for the rest (the PS3 XMB direct tap, the option-menu Start). The overlay instance
                // hands off differently (overlayLaunchGame, no fade-then-exit), so it is exempt.
                if (mWaitForRelease && !mOverlayMode && mLaunchFadeStart == 0)
                    mLaunchFadeStart = uptimeMillis();
                continue;
            }
            // Wait-for-release: after a launch is triggered, keep running
            // until the select key is released. This ensures Android's
            // InputReader sees the full press-release cycle before RetroArch
            // gets focus, preventing phantom A-button presses.
            if (mWaitForRelease) {
                if (ev.type == EV_KEY && ev.value == 0
                    && (ev.code == KEY_ENTER || ev.code == BTN_SOUTH)) {
                    // Don't hand off immediately: start the launch fade-out
                    // (render() fades the XMB to black). The exit fires once the
                    // fade completes (the mLaunchFadeStart check at the top of
                    // pollInput), so the game/app launch fades out instead of a cut.
                    if (mLaunchFadeStart == 0) {
                        ALOGD("NanoMenu: select released, fading out then launching");
                        mLaunchFadeStart = uptimeMillis();
                    }
                }
                continue; // discard all other events while waiting
            }
            // The Game Settings BIOS dialog takes every key: its own (biosDlgKey), the d-pad (on to
            // navPress, for repeat), and nothing else reaches the menu behind. Power, volume, the
            // Anbernic button and the touchscreen go on as usual.
            if (mBiosDlgActive && ev.type == EV_KEY && ev.code != KEY_POWER && ev.code != KEY_VOLUMEUP
                && ev.code != KEY_VOLUMEDOWN && ev.code != KEY_F10 && ev.code != BTN_MODE && ev.code != BTN_TOUCH
                && ev.code != KEY_UP && ev.code != KEY_DOWN && ev.code != KEY_LEFT && ev.code != KEY_RIGHT) {
                if (ev.value == 1) biosDlgKey(ev.code);
                continue;
            }
            // Track SELECT button state; in XMB mode, press refreshes game lists
            if (ev.type == EV_KEY && ev.code == BTN_SELECT) {
                if (ev.value == 1) {
                    // Select again cancels the search it opened: its keyboard, or its results.
                    if (mOskActive && !mOskOverApp && mOskPasswordPrompt == "Search") {
                        const bool popup = mOsk.miniOpen;   // closeOsk closes only the accent popup
                        closeOsk();
                        if (!popup) mOskPasswordCallback = nullptr;
                        ps3Sfx(PS3_SFX_BACK);
                    }
                    else if (mOskActive) {}   // the keyboard's language button cycles its language
                    else if (mGSearchActive && !mPs3DlgActive && !mPs3OptActive) {
                        gsearchClose();
                        ps3Sfx(PS3_SFX_BACK);
                        mDisplayDirty = true;
                    }
                    // Photo viewer: SELECT toggles the EXIF/Information overlay (the
                    // "Display" control), matching the real PS3 photo viewer - which is
                    // why the viewer panel shows the SELECT pill only on that control.
                    else if (mPvActive && !mPvWpMode && !mPvTrimMode
                             && !mPs3DlgActive && !mPvPlChooserActive) {
                        mPvInfo = !mPvInfo; mPvPanel = false;
                    }
                    // Video player: SELECT toggles the Display OSD bar (web vidToggleInfo:
                    // osd=!osd, panel=false), so it also dismisses the control panel.
                    else if (mVidActive) {
                        if (!mVidOpenInProgress.load(std::memory_order_relaxed)) {   // ignore while opening
                            mVidOsd = !mVidOsd;
                            if (mVidCpOpen) vidPanelClose();
                        }
                    }
                    // PS3 XMB: SELECT invokes the global search on RELEASE (tap), not on
                    // press. Arm the tap here; the release edge fires it.
                    else if (mPs3Xmb && !mMpActive && !mPs3OptActive && !mPs3DlgActive
                             && !mPs3WizActive && !mPs3TzActive && !mPs3LangActive && !mBiosDlgActive) {
                        mSelectSearchArmed = true;
                    }
                    else if (mXmbMode) forceRescanAllSystems();
                }
                // SELECT tap-to-search: fire on the real release edge.
                if (ev.value == 0) {
                    if (mSelectSearchArmed
                        && mPs3Xmb && !mOskActive && !mMpActive && !mPs3OptActive
                        && !mPs3DlgActive && !mPs3WizActive && !mPs3TzActive && !mPs3LangActive && !mBiosDlgActive) {
                        // ES-DE gamelist: the focused game's options (Search Games is one of them), and
                        // Select again closes them, back to the game list.
                        if (!mPs3Stack.empty() && isGameOptionsLevel(mPs3Stack.front())
                            && (mEsdeTheme || mEsdeSettingsMinima)) {
                            mPs3Stack.clear();
                            ps3Sfx(PS3_SFX_BACK);
                            mDisplayDirty = true;
                        } else if (mEsdeTheme && mPs3Stack.empty() && mEsdeInGamelist && kioskMode()) {
                            // Kiosk Mode: no Game Options from a game list.
                        } else if (!(mEsdeTheme && mPs3Stack.empty() && !mEsdeMenuActive && esdeOpenGameOptions())) {
                            gsearchOpen();
                        }
                    }
                    mSelectSearchArmed = false;
                }
            }
            if (ev.type == EV_KEY && ev.code == BTN_START) {
                // Photo viewer: during a running slideshow, START toggles play/pause.
                if (ev.value == 1 && mPvActive && mPvSlideshow && !mPvWpMode
                    && !mPvTrimMode && !mPs3DlgActive && !mPvPlChooserActive) {
                    mPvPaused = !mPvPaused;
                    if (!mPvPaused) mPvSlideNext = mEffectTime * 1000.0f + mPvSlideMs;
                }
                // Video player: START toggles play/pause (web START shortcut).
                else if (ev.value == 1 && mVidActive) { if (!mVidOpenInProgress.load(std::memory_order_relaxed)) vidTogglePlay(); }
                mStartHeld = (ev.value != 0);
            }
            // Power button handling
            if (ev.type == EV_KEY && ev.code == KEY_POWER) {
                if (ev.value == 0) mPowerHoldMs = 0;
                // PhoneWindowManager owns the power button unless nano grabs input.
                //
                // Overlay XMB: PWM detects nano mode and TOGGLES the overlay (show/hide)
                // on a power-hold, so nano must not act on power there.
                //
                // The same applies to any home that does NOT grab input exclusively
                // (persist.gammaos.nano.grab_input): PWM handles the whole gesture and
                // PowerManager drives sleep and the backlight. If nano also acted on the
                // open, ungrabbed power fd both would run: PWM would wake the panel and
                // nano would immediately blank it again through enterDrmSleep, which is
                // exactly the "backlight comes up for a second then shuts off, press
                // twice to wake" behaviour. Only the input-grabbing DRM-direct home,
                // where the framework never sees the key, drives sleep itself.
                if (mOverlayMode ||
                    !android::base::GetBoolProperty("persist.gammaos.nano.grab_input", false)) {
                    continue;
                }
                if (ev.value == 1) {
                    // On a device without a DRM-direct path the framework owns display
                    // power: if the screen is already off, a power press is a WAKE
                    // (PowerManager turns the panel back on), not a request to sleep.
                    // Consume it so nano does not re-enter enterDrmSleep and sleep again;
                    // the render loop resumes when sys.screen.state flips back on.
                    if (!sDrmActive) {
                        char ss[PROPERTY_VALUE_MAX] = {};
                        property_get("sys.screen.state", ss, "on");
                        if (!strcmp(ss, "off")) { mPowerPressTime = 0; continue; }
                    }
                    mPowerPressTime = android::uptimeMillis();
                    // Poll for a long press in a tight loop. The home menu is visible on a
                    // grabbing DRM home here (we are past the overlay / non-grab bail above), so a
                    // hold opens the Quick Menu Power submenu rather than shutting down outright.
                    bool powerHold = false;
                    for (int poll = 0; poll < 40; poll++) { // 40 * 50ms = 2s
                        usleep(50000);
                        // Check if key was released
                        struct input_event pe;
                        bool released = false;
                        for (int pfd : mInputFds) {
                            nanoabxy::forget(pfd);
                            while (read(pfd, &pe, sizeof(pe)) == sizeof(pe)) {
                                ssReleaseTaken(pe);
                                if (pe.type == EV_KEY && pe.code == KEY_POWER
                                    && pe.value == 0) {
                                    released = true;
                                }
                            }
                        }
                        touchResync(-1, true);
                        if (released) break;
                        if (android::uptimeMillis() - mPowerPressTime > 1500) {
                            // 1.5s hold while the menu is visible -> open the Quick Menu Power
                            // submenu (Restart / Power Off / Recovery / ...) so the user chooses,
                            // rather than an instant shutdown.
                            ALOGI("NanoMenu: power hold 1.5s -> Quick Menu Power submenu");
                            powerHold = true;
                            break;
                        }
                    }
                    if (powerHold) {
                        mPowerHoldMs = mPowerPressTime;   // still held at 7 s: shut down (powerHoldTick)
                        mPowerPressTime = 0;
                        openQuickPowerMenu();
                        continue;
                    }
                    // Key was released before 1.5s — short press = sleep.
                    mPowerPressTime = 0;
                    // Not while an update is being downloaded or prepared: a sleep would stall the
                    // download, and parking the render thread would stall the handoff to the flasher.
                    // A hold still opens the power menu, so the user can deliberately restart.
                    if (otaInProgress()) {
                        ALOGI("NanoMenu: power short press ignored, system update in progress");
                        continue;
                    }
                    otaCancelCopy();   // an update package being copied can be picked again later
                    ALOGI("NanoMenu: power short press, sleeping");
                    if (!enterDrmSleep()) return;
                }
                continue;
            }
            // Lid (hall-effect) switch, DRM-direct home only. Closing the lid
            // blanks + recommits our own DRM panel around the suspend (opening
            // wakes, handled inside enterDrmSleep's wait loop). The FRAMEWORK
            // still drives the actual system sleep on the lid (its goToSleep ->
            // sys.screen.state=off -> force_sleep -> sleep.sh -> echo mem); the
            // two cooperate. In SF mode (no DRM master) the framework owns the
            // whole lid flow, so we do nothing. Overlay mode also defers to it.
            if (ev.type == EV_SW && ev.code == SW_LID
                && sDrmActive && !mOverlayMode) {
                if (ev.value != 0 && otaInProgress()) {
                    // The update keeps running with the lid shut (the nano_ota wakelock keeps the
                    // SoC up); it will be on screen when the lid opens.
                    ALOGI("NanoMenu: lid closed, not sleeping: system update in progress");
                } else if (ev.value != 0) {   // lid closed
                    otaCancelCopy();
                    ALOGI("NanoMenu: lid closed, sleeping");
                    if (!enterDrmSleep()) return;
                }
                continue;
            }
            // Directional key release clears hold-to-repeat state so the
            // auto-scroll tick stops. Only directional keys matter here;
            // other keys don't participate in the repeat scheduler.
            if (ev.type == EV_KEY && ev.value == 0) {
                switch (ev.code) {
                case KEY_UP:    navRelease(NavDir::Up);    break;
                case KEY_DOWN:  navRelease(NavDir::Down);  break;
                case KEY_LEFT:  navRelease(NavDir::Left);  break;
                case KEY_RIGHT: navRelease(NavDir::Right); break;
                case BTN_SOUTH: if (mOskActive) oskARelease(); break;
                case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: mKbdShiftHeld = false; break;
                case KEY_F10: case BTN_MODE:
                    if (!mAnbernicArmed) break;
                    mAnbernicArmed = false;
                    if (mAnbernicHomeDownMs != 0) {
                        const long held = (long)(android::uptimeMillis() - mAnbernicHomeDownMs);
                        mAnbernicHomeDownMs = 0;
                        // Past the hold, Home still runs if functionHomeTick has not got to it yet.
                        if (devkeys::functionRelease(true, false, false, held) == devkeys::FunctionRelease::Nothing) {
                            functionHomeHeld();
                            break;
                        }
                    }
                    quickMenuButtonReleased();
                    break;
                default: break;
                }
            }
            if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 2)) {
                // Volume keys: Anbernic + VOL = brightness, VOL alone = volume. mAnbernicHeld follows
                // the button's own events, and is cleared wherever its release can be missed
                // (inputEventsLost, the overlay raise and hide), so plain volume stays volume.
                if (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN) {
                    if (mAnbernicHeld && anbernicPhysicallyDown()) {   // a release can have been drained
                        adjustBrightness(ev.code == KEY_VOLUMEUP ? 1 : -1);
                    } else if (ev.value == 1) {
                        adjustVolume(ev.code == KEY_VOLUMEUP ? 1 : -1);
                    }
                    continue;
                }
                // Physical-keyboard shift state (drives OSK uppercase/symbols below).
                if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                    mKbdShiftHeld = true; continue;
                }
                // Physical-keyboard text entry into the on-screen keyboard: type printable
                // characters and Backspace directly (press + autorepeat), so a plugged-in USB
                // keyboard can enter search queries, Wi-Fi passwords and folder names. Non-
                // printable keys (arrows, Enter, Esc) fall through to the normal OSK nav below.
                if (mOskActive) {
                    if (ev.code == KEY_BACKSPACE) { oskBackspace(); continue; }
                    if (ev.code == KEY_TAB) continue;   // Tab = options elsewhere; no-op in the OSK
                    int cp = kbdCodeToCp(ev.code, mKbdShiftHeld);
                    if (cp) { oskInsertCp((uint32_t)cp); continue; }
                }
                if (ev.value == 1) {
                    // Setup wizard intercepts all input when active.
                    // WiFi/BT sub-screens during setup still use the
                    // normal OSK + WiFi/BT handlers since the setup
                    // wizard routes A/B through them. Only the top-level
                    // Start button and step navigation is different.
                    if (mSetupWizardActive && !mOskActive
                        && mMenuState != MENU_WIFI && mMenuState != MENU_BT) {
                        // The network step runs the PS3 net wizard; route its
                        // buttons to the wiz* handlers (Select/Back already chain
                        // through ps3XmbSelect/Back -> wizConfirm/wizBack; Up/Down/
                        // Left/Right via navPress; X = re-scan the AP list).
                        // The BIOS step's dialog takes its buttons (not while the page slides).
                        const bool dpad = ev.code == KEY_UP || ev.code == KEY_DOWN || ev.code == KEY_LEFT || ev.code == KEY_RIGHT;
                        if (biosDlgShown() && !dpad && (mSetupTransitioning || biosDlgKey(ev.code))) continue;
                        if (mPs3WizActive) {
                            switch (ev.code) {
                            case BTN_SOUTH: case KEY_ENTER: handleSelect(); break;
                            case BTN_EAST:  case KEY_BACK:  handleBack();   break;
                            case KEY_UP:    navPress(NavDir::Up);    break;
                            case KEY_DOWN:  navPress(NavDir::Down);  break;
                            case KEY_LEFT:  navPress(NavDir::Left);  break;
                            case KEY_RIGHT: navPress(NavDir::Right); break;
                            case BTN_NORTH: wizRescan(); break;
                            // Start skips the wifi / bluetooth step entirely.
                            case BTN_START: handleSetupStart(); break;
                            default: break;
                            }
                            continue;
                        }
                        switch (ev.code) {
                        case KEY_UP:    navPress(NavDir::Up);   break;
                        case KEY_DOWN:  navPress(NavDir::Down); break;
                        case BTN_SOUTH: case KEY_ENTER:
                            handleSetupSelect(); break;
                        case BTN_EAST: case KEY_BACK:
                            handleSetupBack(); break;
                        case KEY_LEFT:  navPress(NavDir::Left);  break;
                        case KEY_RIGHT: navPress(NavDir::Right); break;
                        case BTN_START:
                            handleSetupStart(); break;
                        default: break;
                        }
                        continue;
                    }
                    // The ES-DE media viewer and game information take every key while up.
                    if (esdeOverlayKey(ev.code)) continue;
                    // Only handle menu nav on initial press, not repeat.
                    // For directional keys we route through navPress() so the
                    // hold-to-repeat tick can drive continuous scrolling while
                    // the key stays down; the release is handled separately
                    // (ev.value == 0 branch below).
                    switch (ev.code) {
                    case KEY_UP:
                        navPress(NavDir::Up); break;
                    case KEY_DOWN:
                        navPress(NavDir::Down); break;
                    case BTN_SOUTH:
                        handleSelect(); break;
                    case KEY_ENTER:
                    case KEY_KPENTER:
                        if (mOskActive) oskConfirm();
                        else handleSelect();
                        break;
                    case BTN_START:
                        // Gamepad Start: confirm the OSK query / password.
                        // The OSK help text advertises "Start:Submit" and
                        // "Start:Search" so this must also trigger submit
                        // on devices that don't map the physical Start
                        // button to KEY_ENTER.
                        if (mOskActive) oskConfirm();
                        else if (mSetupWizardActive) handleSetupStart();
                        // Kiosk Mode: Start opens and closes Applications instead of the ES-DE menu.
                        else if (kioskMode() && (mEsdeTheme || mEsdeSettingsMinima) && mPs3Xmb
                                 && (kioskAppsOpen() || (!ndsInModal() && mPs3Stack.empty()))) {
                            if (kioskAppsOpen()) closeApplicationsView();
                            else openApplicationsView();
                        }
                        // Start closes the Applications view, as Anbernic + Start does.
                        else if (mPs3Xmb && applicationsViewOpen() && !ndsInModal()
                                 && !(mPspClockOn || mPspClockReveal > 0.0f)) closeApplicationsView();
                        // ES-DE home: Start toggles the ES-DE options menu, and leaves a screen opened from it.
                        else if (mEsdeMenuReturn && !mPs3Stack.empty()) esdeMenuExitAll();
                        else if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuClose();
                        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeMenuOpen();
                        break;
                    case KEY_HOMEPAGE:
                        // Home leaves the ES-DE menu, and the screens opened from it, for the game list.
                        if (!mSetupWizardActive && esdeMenuInContext()) esdeMenuExitAll();
                        break;
                    case KEY_BACK:
                        if (!mSetupWizardActive && esdeMenuInContext()) { esdeMenuExitAll(); break; }
                        [[fallthrough]];
                    case BTN_EAST: case KEY_ESC:
                        if (mSetupWizardActive) handleSetupBack();
                        else handleBack();
                        break;
                    case KEY_LEFT:
                        navPress(NavDir::Left); break;
                    case KEY_RIGHT:
                        navPress(NavDir::Right); break;
                    case BTN_WEST: // Y button (Nintendo layout: BTN_WEST = Y); PS3 Square in music
                        if (mScrapeProgActive) {   // Square cancels a scrape
                            if (mScrapeRunning && !mBgScrapeRun.empty()) scraperCancel();
                            break;
                        }
                        if (mOskActive) { oskPaste(); break; }   // Y while typing: paste from the system clipboard
                        if (mMpActive && !mOskActive) { mpCycleVis(); break; }   // Square: cycle the visualizer
                        if (mPvActive && !mOskActive) {   // Square in the viewer: 2D/3D switch
                            if (!mPvWpMode && !mPvTrimMode && !mPvPlChooserActive) pvShow3D();
                            break;
                        }
                        // ES-DE home: the left button is ES-DE's X (VIEW MEDIA). It must not reach the
                        // hidden XMB column below.
                        if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) break;
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) { esdeOpenGameInfo(); break; }   // VIEW MEDIA
                        // Photo album grid: Y cycles the Sort By order (+ banner). Group
                        // Content lives in the Triangle option menu (a submenu).
                        if (mPs3Xmb && !mOskActive && !mPs3OptActive && !mPs3DlgActive
                            && ps3TopScreenKind() == PHOTO_GRID) { photoSortCycleY(); break; }
                        // Photo column root: Y cycles the Sort By order (+ banner). Group
                        // Content moved to the Triangle option menu (user: sort folders with Y).
                        if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mPs3OptActive && !mPs3DlgActive
                            && mPhotoLoaded && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                            && mPs3Cats[mPs3CatIdx].name == "Photo") { photoSortCycleY(); break; }
                        // Video column root: Y cycles the Sort By order (Title / Date / Length).
                        if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mPs3OptActive && !mPs3DlgActive
                            && !mVideoTest && mVideoLoaded && !mVideos.empty()
                            && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                            && mPs3Cats[mPs3CatIdx].name == "Video") { videoSortCycleY(); break; }
                        // Music column root: Y cycles the album Sort By order (Title / Date / Tracks).
                        if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mMpActive && !mPs3OptActive && !mPs3DlgActive
                            && mMusicLoaded && !mMusicTracks.empty()
                            && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                            && mPs3Cats[mPs3CatIdx].name == "Music") { musicSortCycleY(); break; }
                        // Game column root: Y cycles the system tile order (Default / A-Z / Most Games / Manufacturer).
                        if (mPs3Xmb && !mOskActive && mPs3Stack.empty() && !mPs3OptActive && !mPs3DlgActive
                            && mPs3CatIdx >= 0 && mPs3CatIdx < (int)mPs3Cats.size()
                            && mPs3Cats[mPs3CatIdx].name == "Game") { gameSortCycleY(); break; }
                        if (mMenuState == MENU_WIFI) { handleWifiScreenY(); break; }
                        if (mMenuState == MENU_BT)   { handleBtScreenY();   break; }
                        // Icon grid picker: Y opens the name-filter OSK.
                        if (mPs3Xmb && ps3TopScreenKind() == GS_ICONGRID) {
                            openOskForPassword("Filter Icons", [this](const std::string& v) {
                                mIconGridFilter = v;   // strcasestr makes the match case-insensitive
                                applyIconGridFilter();
                            });
                            mOskPasswordMode = false; mOskPlaintext = true;
                            mOskQuery = mIconGridFilter; mOsk.caret = (int)mOskQuery.size();
                            break;
                        }
                        // Emulator picker: Y opens the search (as its Search All Emulators row does).
                        if (mPs3Xmb && ps3TopScreenKind() == GS_EMUPICK) { emuPickerSearch(); break; }
                        // Game Systems list: Y removes a custom system (built-ins only disable).
                        if (mPs3Xmb && !ndsInModal() && ps3TopScreenKind() == GS_LIST) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_GS_SYSTEM_ROW) {
                                int si = its[sel].a;
                                if (si >= 0 && si < (int)mXmbSystems.size() && !mXmbSystems[si].builtin)
                                    gsOpenRemoveConfirm(si);
                            }
                            break;
                        }
                        // Scan-folders screen: Y removes the selected folder. A user scan source asks
                        // to confirm (destructive); a built-in default folder toggles off/on inline.
                        if (mPs3Xmb && !ndsInModal() && ps3TopScreenKind() == GS_FOLDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size()) {
                                if (its[sel].kind == PS3_GS_SCANSRC)
                                    gsOpenRemoveScanSourceConfirm(its[sel].a);
                                else if (its[sel].kind == PS3_GS_DEFFOLDER)
                                    gsDisableDefaultFolder(its[sel].payloadStr);
                                else if (its[sel].kind == PS3_GS_DEFFOLDER_OFF)
                                    gsEnableDefaultFolder(its[sel].payloadStr);
                            }
                            break;
                        }
                        // Music folders screen: Y removes the selected music folder.
                        if (mPs3Xmb && !ndsInModal() && ps3TopScreenKind() == MUSIC_FOLDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_MUSIC_FOLDER_ROW)
                                musicRemoveFolder(its[sel].a);
                            break;
                        }
                        // Photo folders screen: Y removes the selected photo folder.
                        if (mPs3Xmb && !ndsInModal() && ps3TopScreenKind() == PHOTO_FOLDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_PHOTO_FOLDER_ROW)
                                photoRemoveFolder(its[sel].a);
                            break;
                        }
                        // Video folders screen: Y removes the selected video folder.
                        if (mPs3Xmb && !ndsInModal() && ps3TopScreenKind() == VIDEO_FOLDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_VIDEO_FOLDER_ROW)
                                videoRemoveFolder(its[sel].a);
                            break;
                        }
                        // Y on a focused app (ANY theme, at the home, no modal / player): pin/unpin
                        // it to the Game-home Pinned Apps list, mirroring the game Favourites toggle.
                        // Guarded to PS3_APP rows only, so Y still opens a game's Info below.
                        if (mPs3Xmb && !mOskActive && !mPs3OptActive && !mPs3DlgActive && !mPs3WizActive
                            && !mPs3TzActive && !mMpActive && !mPvActive && !mVidActive) {
                            std::vector<Ps3Item>& fi = ps3CurItems();
                            int fs = ps3CurSel();
                            if (fs >= 0 && fs < (int)fi.size() && fi[fs].kind == PS3_APP
                                && !fi[fs].payloadStr.empty()) { toggleAppPinFocused(); break; }
                        }
                        // Y on a focused game with scraped art (ANY theme, at the home, no modal / player):
                        // open its Information page directly, without the Triangle option menu. Returns
                        // false (falls through) when the focus has no scraped art.
                        if (mPs3Xmb && !mOskActive && !mPs3OptActive && !mPs3DlgActive && !mPs3WizActive
                            && !mPs3TzActive && !mMpActive && !mPvActive && !mVidActive
                            && openInfoForFocusedItem()) break;
                        // Y-to-search was removed (it never worked): the PS3 XMB now
                        // uses Select for a categorical global search (gsearchOpen).
                        // Home PS3 XMB: Y (Square) is otherwise unused - the wallpaper
                        // changer moved to Settings > Theme Settings > Wallpaper, and
                        // per-item options live on Triangle/X.
                        if (tryOpenSearchEngineChooser()) break;   // Square/X on Internet Search: pick the engine
                        break;
                    case BTN_NORTH: // X button (Nintendo layout: BTN_NORTH = X); PS3 Triangle in music
                    case KEY_TAB:   // physical keyboard: Tab = Triangle / options menu
                        if (mOskActive) { oskBackspace(); break; }
                        if (mVidActive) { if (mVidOpenInProgress.load(std::memory_order_relaxed) || mVidGoToOpen || mVidSceneOpen || mVidResumeAsk) break; vidPanelToggle(); break; }   // Triangle: video control panel
                        if (mMpActive) { if (mMpCpOpen) closeMpOpt(); else openMpOpt(); break; }   // Triangle: control panel
                        if (mPvActive) { if (mPvPanel) closePvPanel(); else openPvPanel(); break; }   // Triangle: photo control panel
                        if (mPs3WizActive) { wizRescan(); break; }   // X: re-scan on the AP list
                        if (mMenuState == MENU_WIFI) { handleWifiScreenX(); break; }
                        if (mMenuState == MENU_BT)   { handleBtScreenX();   break; }
                        // Icon grid picker: X opens the file browser to import your own PNG icon.
                        if (mPs3Xmb && ps3TopScreenKind() == GS_ICONGRID) { gsOpenIconFilePicker(); break; }
                        // Game Systems list: X toggles the selected system's enabled state.
                        if (mPs3Xmb && ps3TopScreenKind() == GS_LIST) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_GS_SYSTEM_ROW)
                                gsToggleSystem(its[sel].a);
                            break;
                        }
                        // Home Categories editor: X toggles the selected category Shown/Hidden.
                        if (mPs3Xmb && ps3TopScreenKind() == CAT_ORDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_CATORDER_ROW)
                                catOrderToggle(its[sel].a);
                            break;
                        }
                        // Per-item show/hide editor: X toggles the selected item Shown/Hidden.
                        if (mPs3Xmb && ps3TopScreenKind() == ITEM_HIDE) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_ITEMHIDE_ROW)
                                itemHideToggle(its[sel].payloadStr);
                            break;
                        }
                        // ES-DE home: the top button is ES-DE's Y (FAVORITES). It must not open the
                        // hidden XMB column's option menu below. In the menu's Applications it adds
                        // the app to, or removes it from, Android Games.
                        if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) {
                            if (mEsdeMenuActive) esdeMenuToggleAndroidGame();
                            break;
                        }
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) { esdeToggleFavorite(); break; }
                        // Triangle on the focused Internet Search item picks the engine
                        // (before the generic option menu).
                        if (tryOpenSearchEngineChooser()) break;
                        // X acts as PS3 Triangle on the home XMB: open the per-item
                        // option menu (Start / Play / Information). The wallpaper
                        // changer it used to cycle moved to Settings > Theme Settings >
                        // Wallpaper. openXmbOpt() self-guards (no-op over a live overlay
                        // app or while another modal owns input).
                        if (mPs3Xmb) { if (mPs3OptActive) closeXmbOpt(); else openXmbOpt(); }
                        break;
                    case BTN_TL: case KEY_L:
                        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && (ndsGameInfoActive() || ndsDlgInfoPaged())) { ndsInfoPage(-1); break; }  // DSi/Minima: previous info page
                        if (mOskActive) {
                            oskToggleShift();
                            break;
                        }
                        if (mMpActive) { mpPrev(); break; }   // L1: previous track in Now Playing
                        if (mPvActive) { pvStep(-1); break; }   // L1: previous photo in the viewer
                        // ES-DE home: L1 is quick system select.
                        if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) break;
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) { esdeQuickSystemSelect(-1); break; }
                        // Game Systems list: L1 moves the selected system up.
                        if (mPs3Xmb && ps3TopScreenKind() == GS_LIST) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_GS_SYSTEM_ROW)
                                gsReorderSystem(its[sel].a, -1);
                            break;
                        }
                        // Home Categories editor: L1 moves the selected category up.
                        if (mPs3Xmb && ps3TopScreenKind() == CAT_ORDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_CATORDER_ROW)
                                catOrderReorder(its[sel].a, -1);
                            break;
                        }
                        // DSi / Minima: L1 fast-skips a page back through a drilled game / media list.
                        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && !mNdsAtRoot
                            && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive) {
                            ndsBumperSkip(-1); break;
                        }
                        // Pure PS3 XMB: L1 fast page-skips up through the focused item list.
                        if (mPs3Xmb && ps3TopScreenKind() == 0 && !mPs3OptActive && !mPs3DlgActive
                            && !mGSearchActive && !mMpActive && !mPvActive && !mVidActive) { ps3XmbBumperSkip(-1); break; }
                        // Shut any open Settings sub-screen before leaving XMB
                        // so its scan thread exits instead of churning in bg.
                        if (mMenuState == MENU_WIFI) closeWifiScreen();
                        else if (mMenuState == MENU_BT) closeBtScreen();
                        mXmbMode = !mXmbMode;
                        property_set("persist.gammaos.nano.xmb_mode",
                                     mXmbMode ? "1" : "0");
                        if (!mXmbMode) {
                            mMenuState = MENU_MAIN;
                            mSearchActive = false;
                            mOskActive = false;
                        }
                        mDisplayDirty = true;
                        ALOGD("XMB Mode: %s", mXmbMode ? "ON" : "OFF");
                        break;
                    case BTN_TR: case KEY_R:
                        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && (ndsGameInfoActive() || ndsDlgInfoPaged())) { ndsInfoPage(+1); break; }  // DSi/Minima: next info page
                        if (mOskActive) {
                            oskToggleSym();   // R1 toggles ABC <-> SYM inside the OSK
                            break;
                        }
                        if (mMpActive) { mpNext(); break; }   // R1: next track in Now Playing
                        if (mPvActive) { pvStep(+1); break; }   // R1: next photo in the viewer
                        // ES-DE home: R1 is quick system select.
                        if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) break;
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) { esdeQuickSystemSelect(+1); break; }
                        // Game Systems list: R1 moves the selected system down.
                        if (mPs3Xmb && ps3TopScreenKind() == GS_LIST) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_GS_SYSTEM_ROW)
                                gsReorderSystem(its[sel].a, +1);
                            break;
                        }
                        // Home Categories editor: R1 moves the selected category down.
                        if (mPs3Xmb && ps3TopScreenKind() == CAT_ORDER) {
                            auto& its = mPs3Stack.back().items; int sel = mPs3Stack.back().sel;
                            if (sel >= 0 && sel < (int)its.size() && its[sel].kind == PS3_CATORDER_ROW)
                                catOrderReorder(its[sel].a, +1);
                            break;
                        }
                        // DSi / Minima: R1 fast-skips a page forward through a drilled game / media list.
                        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && !mNdsAtRoot
                            && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive) {
                            ndsBumperSkip(+1); break;
                        }
                        // Pure PS3 XMB: R1 fast page-skips down through the focused item list.
                        if (mPs3Xmb && ps3TopScreenKind() == 0 && !mPs3OptActive && !mPs3DlgActive
                            && !mGSearchActive && !mMpActive && !mPvActive && !mVidActive) { ps3XmbBumperSkip(+1); break; }
                        mQuickResumeEnabled = !mQuickResumeEnabled;
                        property_set("persist.gammaos.nano.quick_resume",
                                     mQuickResumeEnabled ? "1" : "0");
                        mDisplayDirty = true;
                        ALOGD("Quick Resume: %s", mQuickResumeEnabled ? "ON" : "OFF");
                        break;
                    case BTN_TL2:
                        if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) break;
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) { esdeJumpToEdge(-1); break; }   // ES-DE: first game
                        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && !mNdsAtRoot && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive) { ndsBumperSkip(-1); break; }
                        if (mPs3Xmb && ps3TopScreenKind() == 0 && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive && !mMpActive && !mPvActive && !mVidActive) ps3XmbBumperSkip(-1);
                        break;
                    case BTN_TR2:
                        if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) break;
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) { esdeJumpToEdge(+1); break; }   // ES-DE: last game
                        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && !mNdsAtRoot && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive) { ndsBumperSkip(+1); break; }
                        if (mPs3Xmb && ps3TopScreenKind() == 0 && !mPs3OptActive && !mPs3DlgActive && !mGSearchActive && !mMpActive && !mPvActive && !mVidActive) ps3XmbBumperSkip(+1);
                        break;
                    case KEY_F10: case BTN_MODE:
                        // The Anbernic button (F10 on the RG Cube's pad, the mode button on others),
                        // acted on at its release. Over a running game PhoneWindowManager raises the
                        // overlay for it; nano reads it only while it is on screen. Android's HOME
                        // key as the button can't be given back to apps, and its hold goes Home.
                        mAnbernicArmed = mPs3Xmb && !(mOverlayMode && !mOverlayShown)
                            && devkeys::functionToSystem(homeFnKey,
                                   property_get_bool("persist.gammaos.nano.mode_quick_settings", true));
                        mAnbernicHomeDownMs = mAnbernicArmed && homeFnKey ? android::uptimeMillis() : 0;
                        break;
                    case BTN_THUMBL: case BTN_THUMBR:
                        if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeRandomGame();   // ES-DE: random game
                        break;
                    default: break;
                    }
                }
            }
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_HAT0X) {
                    // HAT axes only emit a release as value==0. We don't
                    // know from just the axis whether Left or Right was
                    // held, so pass NavDir::None to release whichever is
                    // currently active.
                    if (ev.value < 0)      navPress(NavDir::Left);
                    else if (ev.value > 0) navPress(NavDir::Right);
                    else                   navRelease(NavDir::None);
                } else if (ev.code == ABS_HAT0Y) {
                    if (ev.value < 0)      navPress(NavDir::Up);
                    else if (ev.value > 0) navPress(NavDir::Down);
                    else                   navRelease(NavDir::None);
                } else if (ev.code == ABS_X) {
                    // Left stick X: horizontal navigation
                    int threshold = 29490; // 90% of 32767
                    if (ev.value < -threshold && !mStickXTriggered) {
                        navPress(NavDir::Left);
                        mStickXTriggered = true;
                    } else if (ev.value > threshold && !mStickXTriggered) {
                        navPress(NavDir::Right);
                        mStickXTriggered = true;
                    } else if (ev.value > -threshold && ev.value < threshold) {
                        if (mStickXTriggered) navRelease(NavDir::None);
                        mStickXTriggered = false;
                    }
                } else if (ev.code == ABS_Y) {
                    // Left stick Y: signed range -32768..32767, 90% deadzone
                    // Threshold naturally filters touchscreen ABS_Y (max ~960)
                    int threshold = 29490; // 90% of 32767
                    if (ev.value < -threshold && !mStickYTriggered) {
                        navPress(NavDir::Up);
                        mStickYTriggered = true;
                    } else if (ev.value > threshold && !mStickYTriggered) {
                        navPress(NavDir::Down);
                        mStickYTriggered = true;
                    } else if (ev.value > -threshold && ev.value < threshold) {
                        if (mStickYTriggered) navRelease(NavDir::None);
                        mStickYTriggered = false;
                    }
                }
            }
        }
        // Its queue is empty now: a panel whose events were dropped is read from the device.
        auto rs = std::find(mTouchResyncFds.begin(), mTouchResyncFds.end(), fd);
        if (rs != mTouchResyncFds.end()) { mTouchResyncFds.erase(rs); touchResync(fd, true); }
    }
    // Watchdog: a contact on any panel or a multi-finger gesture with no touch event for 5 s is checked
    // against the device (every queue was just read empty), so a lift lost elsewhere cannot leave touch
    // dead, nor an ignored panel ignored.
    bool anyPanelDown = false;
    for (const auto& kv : mTouchPanels) anyPanelDown = anyPanelDown || kv.second.btn || kv.second.contacts() > 0;
    if ((mTouchMulti || anyPanelDown || (mTouchDown && !mTouchFromPointer))
        && (int64_t)uptimeMillis() - mTouchLastEvMs > 5000)
        touchResync();
    touchPressTick(false);
    launchStartNote();
    // Fire accelerating repeats while a direction remains held. Must run
    // every frame, not only when events arrive, because held axes stop
    // emitting events once settled.
    tickNavRepeat();
    tickAutoScroll();   // glide the Applications cursor to a freshly installed app
}

} // namespace android
