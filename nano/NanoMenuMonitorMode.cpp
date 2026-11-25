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

// Settings > Display Settings > External Display (and the Quick Menu > Display copy): the monitor's
// Output Resolution and Refresh Rate, and Forget Monitor Resolutions. system_server holds the
// monitor's mode (DisplayManagerService.gammaos*MonitorMode); Nano asks it through
// `cmd display gammaos-monitor-mode`, off the render thread, and reads what it holds from
// sys.gammaos.display.monitor_mode.
//
// A mode chosen in a chooser runs as a trial (`try`): system_server reverts it after 15 s unless
// Nano sends `keep`, so a monitor that shows nothing at the new mode comes back by itself, even if
// Nano dies. Meanwhile Nano asks Keep This Resolution?, the cursor on Revert, and takes no answer
// for its first kGuardMs, so the press that chose the mode can't keep it. B, closing the dialog and
// the countdown all revert. Auto (`auto`) and Forget Monitor Resolutions (`forget`) go back to the
// mode system_server picks, so they run at once.
//
// Over a running game the rows are dimmed and do nothing: a new size can restart an emulator, and
// any change blanks the monitor for a second or more.

#define LOG_TAG "NanoMenu"

#include "NanoMenu.h"
#include "NanoI18n.h"
#include "NanoMonitorMode.h"

#include <cutils/properties.h>
#include <log/log.h>
#include <stdio.h>
#include <sys/wait.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <thread>

namespace android {

namespace {

constexpr const char* kMonitorModeProp = "sys.gammaos.display.monitor_mode";   // "WxH@HZ;auto|user|trial"
constexpr long kGuardMs = 750;      // Keep This Resolution? takes no answer this long after it opens
constexpr int kTrialSecs = 15;      // GammaosScreenArrangement.TRIAL_MS
constexpr int kIcon = 16;           // the display icon, as the External Display rows have
constexpr int kRowResolution = 0, kRowRate = 1, kRowForget = 2;   // the rows' it.b
constexpr int kKeyResolution = 68, kKeyRate = 69, kKeyTrial = 70;  // the theme keys
constexpr int kRevert = 1;          // Keep This Resolution?'s options: 0 Keep, 1 Revert

// "60 Hz"
std::string rateWords(const monmode::Mode& m) {
    char buf[64];
    snprintf(buf, sizeof(buf), trDyn("%s Hz"), m.rate.c_str());
    return buf;
}

// "2560 x 1440 at 60 Hz"
std::string modeWords(const monmode::Mode& m) {
    char buf[160];
    snprintf(buf, sizeof(buf), trDyn("%s at %s"), monmode::sizeText(m).c_str(), rateWords(m).c_str());
    return buf;
}

// "Auto (1920 x 1080)"
std::string autoWords(const std::string& what) {
    char buf[160];
    snprintf(buf, sizeof(buf), trDyn("Auto (%s)"), what.c_str());
    return buf;
}

// Runs `cmd display gammaos-monitor-mode ARGS`: what it printed (stdout and stderr) goes to out,
// true when it exited 0. `try` waits up to 3.5 s for the mode; a wedged binder is killed at 10 s.
bool runMonitorModeCmd(const std::string& args, std::string* out) {
    const std::string cmd = "timeout -s KILL 10 cmd display gammaos-monitor-mode " + args + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out->append(buf, n);
    const int status = pclose(p);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

// A game runs: its monitor's mode is not changed under it.
bool NanoMenu::monitorModeLocked() const {
    return gameOnMonitor();
}

std::string NanoMenu::monitorModeSig() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kMonitorModeProp, v, "");
    return std::string(v) + (monitorModeLocked() ? "|game" : "");
}

void NanoMenu::monitorModeRows(Ps3Level& out, int qa, bool forget) {
    const bool locked = monitorModeLocked();
    auto row = [&](const char* label, const char* desc, int b, const std::string& value) {
        Ps3Item it;
        it.label = trDyn(label);
        it.desc = trDyn(desc);
        it.value = value;
        it.kind = PS3_QUICK; it.a = qa; it.b = b;
        it.iconTex = iconTexForIcon(kIcon); it.nmapTex = nmapForIcon(kIcon);
        it.iconR = it.iconG = it.iconB = locked ? 0.45f : 1.0f;   // dimmed over a game
        out.items.push_back(it);
    };
    if (forget) {
        row("Forget Monitor Resolutions",
            "Every monitor goes back to Auto. If a monitor shows no picture at the resolution it was given, "
            "unplug it, choose this, then plug it in again.",
            kRowForget, std::string());
        return;
    }
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kMonitorModeProp, v, "");
    monmode::Mode held;
    std::string why;
    if (!monmode::parseProp(v, &held, &why)) return;   // no monitor
    const bool autoMode = why == "auto";
    row("Output Resolution",
        "Sets the resolution the monitor runs at. Auto runs it at 1920 x 1080 at 60 Hz when it has that. A new "
        "resolution shows for 15 seconds and is kept only if you choose Keep.",
        kRowResolution, autoMode ? autoWords(monmode::sizeText(held)) : monmode::sizeText(held));
    row("Refresh Rate",
        "Sets how many times a second the monitor shows a new picture, at its resolution. A new rate shows for "
        "15 seconds and is kept only if you choose Keep.",
        kRowRate, autoMode ? autoWords(rateWords(held)) : rateWords(held));
}

void NanoMenu::monitorModeOpen(int row) {
    if (monitorModeLocked()) {
        showXmbMessage(trDyn("Close the game to change the resolution"));
        return;
    }
    if (mMonModeCall != MMC_NONE || mMonTrialOpen) return;   // one change at a time
    if (row == kRowForget) {
        monitorModeCall(MMC_FORGET, "forget");
    } else {
        monitorModeCall(row == kRowRate ? MMC_LIST_RATES : MMC_LIST_SIZES, "list");
    }
}

bool NanoMenu::monitorModeCall(int call, const std::string& args) {
    if (mMonModeCall != MMC_NONE) return false;
    mMonModeCall = call;
    {
        std::lock_guard<std::mutex> lk(mMonModeMutex);
        mMonModeDone = false;
        mMonModeOut.clear();
    }
    ALOGI("monitor mode: %s", args.c_str());
    std::thread([this, args] {
        nanoThreadNormalPriority();
        std::string out;
        const bool ok = runMonitorModeCmd(args, &out);
        {
            std::lock_guard<std::mutex> lk(mMonModeMutex);
            mMonModeOut = out;
            mMonModeOk = ok;
            mMonModeDone = true;
        }
        FrameFlag::wake();   // monitorModeTick takes it on the next frame
    }).detach();
    return true;
}

void NanoMenu::monitorModeTick() {
    if (mMonModeCall != MMC_NONE) {
        bool done = false, ok = false;
        std::string out;
        {
            std::lock_guard<std::mutex> lk(mMonModeMutex);
            if (mMonModeDone) {
                done = true;
                ok = mMonModeOk;
                out.swap(mMonModeOut);
                mMonModeDone = false;
            }
        }
        if (done) {
            const int call = mMonModeCall;
            mMonModeCall = MMC_NONE;
            monitorModeResult(call, ok, out);
        }
    }
    if (mMonModeRevertDue && mMonModeCall == MMC_NONE) {
        mMonModeRevertDue = false;
        monitorModeCall(MMC_REVERT, "revert");
    }
    if (!mMonTrialOpen) return;
    if (!mPs3DlgActive || mPs3DlgThemeKey != kKeyTrial) {   // another dialog took its place: not kept
        mMonTrialOpen = false;
        monitorModeRevert();
        return;
    }
    const long now = (long)android::uptimeMillis();
    if (now - mMonTrialShownMs < kGuardMs) mPs3DlgSel = kRevert;
    if (now >= mMonTrialEndMs) {
        closePs3Dialog(false);   // not kept in time: Revert
        return;
    }
    monitorModeBody(now);
}

void NanoMenu::monitorModeResult(int call, bool ok, const std::string& out) {
    mExtDisplaySig.clear();   // the rows show what the monitor runs at now
    mDisplayDirty = true;
    if (!ok) ALOGW("monitor mode: call %d failed: %s", call, out.c_str());
    switch (call) {
        case MMC_LIST_SIZES:
        case MMC_LIST_RATES: {
            monmode::List list;
            if (!ok || !monmode::parseList(out, &list)) {
                showXmbMessage(trDyn("No monitor is connected"));
                return;
            }
            // Only over the list it was opened from, with nothing else up.
            if (mPs3DlgActive || mPs3Stack.empty() || mPs3Stack.back().screenKind != EXT_DISPLAY) return;
            monitorModeChooser(call, list);
            return;
        }
        case MMC_TRY: {
            monmode::Trial trial;
            if (ok && monmode::parseTrial(out, &trial)) {
                if (mPs3DlgActive) {   // something else is up: the trial is not kept
                    monitorModeRevert();
                    return;
                }
                monitorModeAsk(trial);
                return;
            }
            char buf[256];
            snprintf(buf, sizeof(buf), trDyn("The monitor could not switch to %s"), modeWords(mMonModeTried).c_str());
            showXmbMessage(buf, std::string(), 300);
            return;
        }
        case MMC_KEEP:
            if (!ok) showXmbMessage(trDyn("The resolution could not be changed"), std::string(), 300);
            else if (out.find("until unplugged") != std::string::npos)   // a monitor without product info
                photoShowBanner(trDyn("Kept until the monitor is unplugged"));
            else photoShowBanner(trDyn("Setting changed"));
            return;
        case MMC_AUTO:
            if (ok) photoShowBanner(trDyn("Setting changed"));
            else showXmbMessage(trDyn("The resolution could not be changed"), std::string(), 300);
            return;
        case MMC_FORGET:
            if (ok) photoShowBanner(trDyn("Monitor resolutions forgotten"));
            else showXmbMessage(trDyn("The resolution could not be changed"), std::string(), 300);
            return;
        default:   // MMC_REVERT: system_server reverts at the end of the trial anyway
            return;
    }
}

// Output Resolution (theme key 68): Auto, then one mode per size, largest first. Refresh Rate (69):
// Auto, then the rates at the monitor's size. The cursor starts on what the monitor runs at; moving
// it changes nothing (a switch blanks the monitor for a second or more).
void NanoMenu::monitorModeChooser(int call, const monmode::List& list) {
    const bool rates = call == MMC_LIST_RATES;
    const std::vector<monmode::Mode> modes = monmode::offered(list.modes, externalDisplayMode() == "dual");
    const monmode::Mode cur = list.held.valid() ? list.held : list.active;
    mMonModeChoices = rates ? monmode::rateChoices(modes, cur) : monmode::sizeChoices(modes, cur);
    mMonModeHeld = cur;
    mMonModeHeldWhy = list.why;
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 1; mPs3DlgType = 0; mPs3DlgThemeKey = rates ? kKeyRate : kKeyResolution;
    mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    // A Dual-Screen App prompt closed without an answer leaves its flag set; X would answer it.
    mPs3DlgDualScreen = false; mPs3DlgDualScreenPkg.clear();
    mPs3DlgTitle = rates ? "Refresh Rate" : "Output Resolution";
    mPs3DlgBody.clear();
    if (list.autoMode.valid())
        mPs3DlgOptions.push_back(autoWords(rates ? rateWords(list.autoMode) : monmode::sizeText(list.autoMode)));
    else
        mPs3DlgOptions.push_back("Auto");
    mPs3DlgSwatch.push_back(-1);
    for (const monmode::Mode& m : mMonModeChoices) {
        mPs3DlgOptions.push_back(rates ? rateWords(m) : monmode::sizeText(m));
        mPs3DlgSwatch.push_back(-1);
    }
    int sel = 0;
    if (list.why != "auto") {
        const int i = monmode::indexOf(mMonModeChoices, cur, !rates);
        if (i >= 0) sel = i + 1;
    }
    mPs3DlgSel = sel; mPs3DlgOrigSel = sel;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(kIcon);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
    mDisplayDirty = true;
}

void NanoMenu::monitorModeChosen(int themeKey, int sel) {
    (void)themeKey;   // the choices hold the modes either chooser offered
    if (sel < 0 || sel > (int)mMonModeChoices.size()) return;
    if (monitorModeLocked()) {
        showXmbMessage(trDyn("Close the game to change the resolution"));
        return;
    }
    if (sel == 0) {   // Auto: what system_server picks, so no trial
        if (mMonModeHeldWhy != "auto") monitorModeCall(MMC_AUTO, "auto");
        return;
    }
    const monmode::Mode m = mMonModeChoices[sel - 1];
    if (monmode::sameMode(m, mMonModeHeld)) return;   // what the monitor runs at already
    mMonModeTried = m;
    // parseMode let only digits and a dot into the rate.
    char args[64];
    snprintf(args, sizeof(args), "try %d %d %s", m.width, m.height, m.rate.c_str());
    monitorModeCall(MMC_TRY, args);
}

// Keep This Resolution? (theme key 70), once the mode is active. It ends a second before
// system_server's own revert, so Keep never comes too late.
void NanoMenu::monitorModeAsk(const monmode::Trial& trial) {
    const long now = (long)android::uptimeMillis();
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 1; mPs3DlgThemeKey = kKeyTrial; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    mPs3DlgDualScreen = false; mPs3DlgDualScreenPkg.clear();   // as in monitorModeChooser
    mPs3DlgTitle = "Keep This Resolution?";
    mPs3DlgOptions.push_back("Keep");   mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Revert"); mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = kRevert; mPs3DlgOrigSel = kRevert;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(kIcon);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
    mMonTrialOpen = true;
    mMonTrialShownMs = now;
    mMonTrialEndMs = now + 1000L * std::min(std::max(trial.seconds - 1, 1), kTrialSecs);
    mMonTrialNow = modeWords(trial.mode);
    mMonTrialBack = modeWords(trial.revert);
    monitorModeBody(now);
    ALOGI("monitor mode: trial of %s, back to %s unless kept", trial.mode.text().c_str(), trial.revert.text().c_str());
}

void NanoMenu::monitorModeBody(long nowMs) {
    const long left = mMonTrialEndMs - nowMs;
    const int secs = left <= 0 ? 0 : (int)((left + 999) / 1000);
    char buf[512];
    snprintf(buf, sizeof(buf), trDyn("The monitor now shows %s. It goes back to %s in %d s."), mMonTrialNow.c_str(),
             mMonTrialBack.c_str(), secs);
    if (mPs3DlgBody != buf) {
        mPs3DlgBody = buf;
        mDisplayDirty = true;
    }
}

void NanoMenu::monitorModeAnswer(bool keep) {
    if (!mMonTrialOpen) return;
    mMonTrialOpen = false;
    if (keep && (long)android::uptimeMillis() < mMonTrialEndMs && monitorModeCall(MMC_KEEP, "keep")) return;
    monitorModeRevert();
}

void NanoMenu::monitorModeRevert() {
    if (!monitorModeCall(MMC_REVERT, "revert")) mMonModeRevertDue = true;   // sent once the running call ends
}

// While Keep This Resolution? is new, an answer is ignored and the cursor goes back to Revert: true
// then, for the caller to drop the input.
bool NanoMenu::monitorModeDlgGuard() {
    if (!mMonTrialOpen || !mPs3DlgActive || mPs3DlgThemeKey != kKeyTrial) return false;
    if ((long)android::uptimeMillis() - mMonTrialShownMs >= kGuardMs) return false;
    mPs3DlgSel = kRevert;
    return true;
}

}  // namespace android
