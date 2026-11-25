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

// The BIOS dialog and setup's microSD steps.
//
//   SETUP_SDCARD   with a microSD card in: Keep ES-DE / Keep emulator files on the card (On/Off).
//   SETUP_SDMOVE   the moves turned on, without questions (NanoSdMove.cpp, mSdMoveAuto).
//   SETUP_BIOS     the BIOS dialog as setup's page.
//
// The BIOS dialog: the files the emulators use (biosDialogRows, NanoBiosInstall.cpp) with their
// SHA-1, an Alternates page for a row with other versions, an All Supported Files page (biosAllFiles),
// Select Folder (the folder browser's entries, buildFolderBrowser, listed in the dialog), the install's
// progress and a report. Each page is wrapped lines scrolled a line at a time, its buttons under it.
// Game Settings shows it as a modal over any theme, coloured like the boxart progress modal.

#define LOG_TAG "NanoMenu"

#include "NanoMenu.h"
#include "NanoBidi.h"
#include "NanoI18n.h"
#include "NanoMenuPS3.h"
#include "NanoMenuShaders.h"
#include "NanoMenuStrings.h"

#include <cutils/properties.h>
#include <linux/input.h>
#include <log/log.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace android {

namespace {

// Line styles.
enum { kHead = 0, kSys, kSub, kFile, kSha, kPara, kGap };

// Palette slots (biosDrawPage's pal).
enum { kAccR = 0, kAccG, kAccB, kT1R, kT1G, kT1B, kT2R, kT2G, kT2B, kSelR, kSelG, kSelB, kSelA,
       kBtnR, kBtnG, kBtnB, kBtnA, kBtnInk, kPalN };

// One UTF-8 code point off the front / the end of s (never half a character).
void biosDropFirstCp(std::string& s) {
    size_t n = s.empty() ? 0 : 1;
    while (n < s.size() && ((unsigned char)s[n] & 0xC0) == 0x80) n++;
    s.erase(0, n);
}
void biosDropLastCp(std::string& s) {
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

// A path as the user knows it.
std::string biosShownPath(const std::string& p) {
    const std::string emu = "/storage/emulated/0";
    if (p.empty()) return trDyn("Storage");
    if (p.compare(0, emu.size(), emu) == 0) return trDyn("Internal storage") + p.substr(emu.size());
    if (p.compare(0, 9, "/storage/") == 0) {
        const size_t rest = p.find('/', 9);
        return trDyn("microSD card") + (rest == std::string::npos ? std::string() : p.substr(rest));
    }
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void NanoMenu::biosDialogOpen(bool setup) {
    // Over a running game or app the installer would close emulators under it.
    if (!setup && (property_get_bool("sys.gammaos.nano.app_launched", false))) {
        photoShowBanner(trDyn("Close the game first: installing BIOS files restarts emulators."));
        return;
    }
    mBiosRows = biosDialogRows();
    mBiosDlgSetup = setup;
    mBiosDlgActive = !setup;
    mBiosAltRow = -1;
    biosDlgSetMode(BIOS_LIST);
    mBiosSel = 0;
    // A running install shows its progress; a re-install after a move (no progress of its own)
    // "Finishing an earlier BIOS install" until it ends, then the list.
    bool busy;
    { std::lock_guard<std::mutex> lk(mBiosMutex); busy = mBiosBusy; }
    if (biosInstallProgress().running) biosDlgSetMode(BIOS_PROGRESS);
    else if (busy) { biosDlgSetMode(BIOS_PROGRESS); mBiosWaitEarlier = true; }
    mDisplayDirty = true;
}

// The folder browser's entries for path, as a folder pick (Select This Folder, no Use Default
// Folder) whatever picker was last open.
void NanoMenu::biosBuildPick(const std::string& path) {
    const int saved = mFolderPickTarget;
    mFolderPickTarget = 0;
    buildFolderBrowser(path, mBiosPick);
    mFolderPickTarget = saved;
}

void NanoMenu::biosDialogClose() {
    mBiosDlgActive = false;
    mBiosLines.clear();
    mBiosPick = Ps3Level();
    mDisplayDirty = true;
}

void NanoMenu::biosDlgSetMode(int mode) {
    mBiosDlgMode = mode;
    mBiosScroll = 0;
    mBiosLinesDirty = true;
    mBiosLines.clear();   // the old page's lines and rows mean nothing on the new one
    mBiosRowFirst.clear(); mBiosRowLast.clear();
    mBiosLinkFirst = mBiosLinkLast = -1;
    mBiosProgStartMs = 0;
    mBiosWaitEarlier = false;
    mBiosCancelAsked = false;
    mTouch.stopFling();
    if (mode == BIOS_PICK) mBiosSel = mBiosPick.sel;
    mDisplayDirty = true;
}

// A folder to install from (the picker's Select This Folder): the install starts, the dialog shows
// its progress.
void NanoMenu::biosFolderChosen(const std::string& path) {
    if (path.empty() || path == "@default") return;
    mBiosPick = Ps3Level();
    if (!biosInstallStart(path)) {   // a re-install after a move is still running
        ALOGI("bios: busy, not installing from %s", path.c_str());
        mBiosReport = BiosInstallProgress();
        mBiosReport.lines.push_back(trDyn("Another BIOS install is running. Try again in a moment."));
        biosDlgSetMode(BIOS_REPORT);
        return;
    }
    ALOGI("bios: install from %s", path.c_str());
    biosDlgSetMode(BIOS_PROGRESS);
}

// Render thread, each frame the dialog shows: the folder listing, the install's end.
void NanoMenu::biosDlgTick() {
    if (mBiosDlgMode == BIOS_PICK) {
        fbTick();
        const bool loading = mBiosPick.items.size() >= 1 && mBiosPick.items.back().kind == PS3_DATA_LEAF;
        if (loading && mFbCacheValid && mFbCachePath == mGsFolderPath && !mGsFolderPath.empty()) {
            const int keep = mBiosPick.sel;
            biosBuildPick(mGsFolderPath);
            mBiosPick.sel = std::min(keep, (int)mBiosPick.items.size() - 1);
            mBiosSel = mBiosPick.sel;
            mBiosLinesDirty = true;
        }
    } else if (mBiosDlgMode == BIOS_PROGRESS && mBiosWaitEarlier) {
        bool busy;
        { std::lock_guard<std::mutex> lk(mBiosMutex); busy = mBiosBusy; }
        if (!busy) biosDlgSetMode(BIOS_LIST);   // the earlier one ended: the list
        mDisplayDirty = true;
    } else if (mBiosDlgMode == BIOS_PROGRESS) {
        const BiosInstallProgress p = biosInstallProgress();
        const int64_t now = uptimeMillis();
        if (mBiosProgStartMs == 0) mBiosProgStartMs = now;
        // Ended, or never started (then the report says what there is after a moment).
        if (!p.running && (p.done || now - mBiosProgStartMs > 3000)) {
            mBiosProgStartMs = 0;
            mBiosReport = p;
            mBiosReport.done = true;
            biosDlgSetMode(BIOS_REPORT);
        }
        mDisplayDirty = true;   // the bar and the file move on their own
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// The focus in the list: a row (0..rows-1), the All Supported Files link (rows), then Select Folder
// (rows + 1) and Skip (rows + 2).
void NanoMenu::biosDlgNav(int dir) {
    const int rows = (int)mBiosRows.size();
    const int fLink = rows, fInstall = rows + 1;
    switch (mBiosDlgMode) {
    case BIOS_LIST: {
        if (biosDlgRepick(dir)) return;   // scrolled away by touch: a row in view first
        const bool laidOut = (int)mBiosRowFirst.size() == rows && (int)mBiosRowLast.size() == rows;
        if (mBiosSel >= 0 && mBiosSel < rows) {
            // A row taller than the page scrolls through before the focus moves on.
            if (laidOut) {
                const int first = mBiosRowFirst[mBiosSel], last = mBiosRowLast[mBiosSel];
                if (dir > 0 && last >= mBiosScroll + mBiosVisible) { biosDlgScroll(1); return; }
                if (dir < 0 && first < mBiosScroll) { biosDlgScroll(-1); return; }
            }
            mBiosSel += dir;
            if (mBiosSel < 0) { mBiosSel = 0; biosDlgScroll(-1); return; }
        } else if (mBiosSel == fLink) {
            if (dir > 0) mBiosSel = fInstall;
            else if (rows > 0) mBiosSel = rows - 1;
            else { biosDlgScroll(-1); return; }
        } else if (dir < 0) {
            mBiosSel = fLink;   // from the buttons back up to the link
        } else {
            biosDlgScroll(1);
            return;
        }
        biosDlgEnsureVisible();
        break;
    }
    case BIOS_PICK: {
        const int n = (int)mBiosPick.items.size();
        if (n == 0) return;
        if (biosDlgRepick(dir)) return;
        mBiosSel = std::clamp(mBiosSel + dir, 0, n - 1);
        mBiosPick.sel = mBiosSel;
        biosDlgEnsureVisible();
        break;
    }
    case BIOS_ALTS:
    case BIOS_ALL:
    case BIOS_REPORT:
        biosDlgScroll(dir);
        break;
    default:
        break;
    }
    mDisplayDirty = true;
}

// Left / Right pick the button (Select Folder on the left, Skip on the right).
void NanoMenu::biosDlgHoriz(int dir) {
    if (mBiosDlgMode != BIOS_LIST) return;
    const int rows = (int)mBiosRows.size();
    mBiosSel = rows + (dir < 0 ? 1 : 2);
    biosDlgEnsureVisible();
    mDisplayDirty = true;
}

void NanoMenu::biosDlgPage(int dir) {
    const int step = std::max(1, mBiosVisible - 1);
    if (mBiosDlgMode == BIOS_PICK) { for (int i = 0; i < step; i++) biosDlgNav(dir); return; }
    for (int i = 0; i < step; i++) if (!biosDlgScroll(dir)) break;
    biosDlgRepick(dir);   // the focus follows to what is in view
    mDisplayDirty = true;
}

// The focus is a row (or picker entry, or the link) the page no longer shows: take the first
// (dir > 0) or last (dir < 0) one in view instead. True when the focus moved. The buttons are
// always in view.
bool NanoMenu::biosDlgRepick(int dir) {
    const int end = mBiosScroll + mBiosVisible;
    auto inView = [&](int first, int last) { return first >= 0 && last >= mBiosScroll && first < end; };
    const int n = std::min(mBiosRowFirst.size(), mBiosRowLast.size());
    if (mBiosDlgMode == BIOS_PICK) {
        if (mBiosSel < 0 || mBiosSel >= n || inView(mBiosRowFirst[mBiosSel], mBiosRowLast[mBiosSel])) return false;
        int pick = -1;
        for (int r = 0; r < n; r++) if (inView(mBiosRowFirst[r], mBiosRowLast[r]) && (pick < 0 || dir < 0)) pick = r;
        if (pick < 0) return false;
        mBiosSel = mBiosPick.sel = pick;
        mDisplayDirty = true;
        return true;
    }
    if (mBiosDlgMode != BIOS_LIST) return false;
    const int rows = (int)mBiosRows.size();
    if (n != rows || mBiosSel < 0 || mBiosSel > rows) return false;   // buttons, or not laid out yet
    const bool linkIn = inView(mBiosLinkFirst, mBiosLinkLast);
    if (mBiosSel < rows ? inView(mBiosRowFirst[mBiosSel], mBiosRowLast[mBiosSel]) : linkIn) return false;
    int pick = -1;
    for (int r = 0; r < rows; r++) if (inView(mBiosRowFirst[r], mBiosRowLast[r]) && (pick < 0 || dir < 0)) pick = r;
    if (linkIn && (pick < 0 || dir < 0)) pick = rows;
    if (pick < 0) return false;
    mBiosSel = pick;
    biosDlgEnsureVisible();
    mDisplayDirty = true;
    return true;
}

bool NanoMenu::biosDlgScroll(int dir) {
    const int maxScroll = std::max(0, (int)mBiosLines.size() - mBiosVisible);
    const int to = std::clamp(mBiosScroll + dir, 0, maxScroll);
    if (to == mBiosScroll) return false;
    mBiosScroll = to;
    mDisplayDirty = true;
    return true;
}

void NanoMenu::biosDlgEnsureVisible() {
    int first = -1, last = -1;
    if (mBiosDlgMode == BIOS_LIST) {
        const int rows = (int)mBiosRows.size();
        if (mBiosSel == rows) { first = mBiosLinkFirst; last = mBiosLinkLast; }   // the All Supported Files link
        else if (mBiosSel > rows) { first = last = (int)mBiosLines.size() - 1; }   // the buttons: the page's end
        else if (mBiosSel >= 0 && mBiosSel < (int)mBiosRowFirst.size() && mBiosSel < (int)mBiosRowLast.size()) {
            first = mBiosRowFirst[mBiosSel]; last = mBiosRowLast[mBiosSel];
            // The first row of a section brings its heading along.
            while (first > 0 && first <= (int)mBiosLines.size()
                   && (mBiosLines[first - 1].style == kHead || mBiosLines[first - 1].style == kGap)
                   && mBiosLines[first - 1].row < 0 && last - first < mBiosVisible - 1)
                first--;
            // The top row, its start in view, keeps the page's opening lines (no BIOS included,
            // optional in setup) in view on a short screen too: the d-pad scrolls on to its end.
            bool topRow = mBiosScroll <= mBiosRowFirst[mBiosSel];
            for (int r = 0; r < rows && topRow; r++)
                if (mBiosRowFirst[r] < mBiosRowFirst[mBiosSel]) topRow = false;
            if (topRow) { first = 0; last = std::min(last, mBiosVisible - 1); }
        }
    } else if (mBiosDlgMode == BIOS_PICK && mBiosSel >= 0 && mBiosSel < (int)mBiosRowFirst.size()
               && mBiosSel < (int)mBiosRowLast.size()) {
        first = mBiosRowFirst[mBiosSel]; last = mBiosRowLast[mBiosSel];
    }
    if (first < 0) return;
    if (last >= mBiosScroll + mBiosVisible) mBiosScroll = last - mBiosVisible + 1;
    if (first < mBiosScroll) mBiosScroll = first;
    mBiosScroll = std::clamp(mBiosScroll, 0, std::max(0, (int)mBiosLines.size() - mBiosVisible));
}

void NanoMenu::biosDlgSelect() {
    // A focus scrolled out of view by touch: A first brings the focus to what is shown.
    if ((mBiosDlgMode == BIOS_LIST || mBiosDlgMode == BIOS_PICK) && biosDlgRepick(1)) return;
    switch (mBiosDlgMode) {
    case BIOS_LIST:    biosDlgActivate(mBiosSel); break;
    case BIOS_PICK:    biosDlgActivate(mBiosSel); break;
    case BIOS_ALTS:
    case BIOS_ALL:     biosDlgBack(); break;
    case BIOS_REPORT:  biosDlgActivate(0); break;
    case BIOS_PROGRESS: biosDlgCancel(); break;   // A presses the focused Cancel
    default: break;
    }
}

// What a focus does: a row opens its Alternates, the link All Supported Files, Select Folder picks
// the folder, Skip goes on.
void NanoMenu::biosDlgActivate(int focus) {
    const int rows = (int)mBiosRows.size();
    if (mBiosDlgMode == BIOS_LIST) {
        if (focus >= 0 && focus < rows) {
            mBiosSel = focus;
            if (!mBiosRows[focus].alternates.empty()) {
                mBiosAltRow = focus;
                biosDlgSetMode(BIOS_ALTS);
            }
        } else if (focus == rows) {   // the link
            mBiosSel = focus;
            biosDlgSetMode(BIOS_ALL);
        } else if (focus == rows + 1) {   // Select Folder
            // The folder browser's entries (storage roots, .., Select This Folder, the folders),
            // opened at the card's bios folder when there is one.
            const std::string def = biosDefaultFolder();
            biosBuildPick(def);
            mBiosPick.sel = (!def.empty() && mBiosPick.items.size() > 1) ? 1 : 0;   // Select This Folder
            biosDlgSetMode(BIOS_PICK);
        } else if (focus == rows + 2) {   // Skip: nothing is looked at
            if (mBiosDlgSetup) advanceSetupStep();
            else biosDialogClose();
        }
    } else if (mBiosDlgMode == BIOS_PICK) {
        if (focus < 0 || focus >= (int)mBiosPick.items.size()) return;
        mBiosSel = mBiosPick.sel = focus;
        const Ps3Item it = mBiosPick.items[focus];
        if (it.kind == PS3_GS_DIR) {
            biosBuildPick(it.payloadStr);
            mBiosPick.sel = 0;
            biosDlgSetMode(BIOS_PICK);
        } else if (it.kind == PS3_GS_SELFOLDER) {
            biosFolderChosen(it.payloadStr);
        }
    } else if (mBiosDlgMode == BIOS_ALTS || mBiosDlgMode == BIOS_ALL) {
        biosDlgBack();
    } else if (mBiosDlgMode == BIOS_REPORT) {
        if (mBiosDlgSetup) advanceSetupStep();
        else biosDialogClose();
    }
    mDisplayDirty = true;
}

void NanoMenu::biosDlgBack() {
    switch (mBiosDlgMode) {
    case BIOS_LIST:
        if (!mBiosDlgSetup) biosDialogClose();   // setup: Skip is the way on
        break;
    case BIOS_ALTS:   // back to the row
        biosDlgSetMode(BIOS_LIST);
        break;
    case BIOS_ALL:    // back to the link
        biosDlgSetMode(BIOS_LIST);
        mBiosSel = (int)mBiosRows.size();
        break;
    case BIOS_PICK:
        mBiosPick = Ps3Level();
        biosDlgSetMode(BIOS_LIST);
        mBiosSel = (int)mBiosRows.size() + 1;   // back on Select Folder
        break;
    case BIOS_REPORT:
        biosDlgActivate(0);
        break;
    case BIOS_PROGRESS:
        // An install runs to its end unless Cancel is pressed. Waiting for a re-install after a
        // move (no Cancel), Game Settings can be left.
        if (mBiosWaitEarlier && !mBiosDlgSetup) biosDialogClose();
        break;
    default:
        break;
    }
    mDisplayDirty = true;
}

// The progress page's Cancel: stops the install once; its report ("Install cancelled." and what
// went in) follows.
void NanoMenu::biosDlgCancel() {
    if (mBiosDlgMode != BIOS_PROGRESS || mBiosWaitEarlier || mBiosCancelAsked) return;
    mBiosCancelAsked = true;
    biosInstallCancel();
    mDisplayDirty = true;
}

// The buttons the dialog takes while it shows (the d-pad and stick go through handleUp/Down/
// Left/Right). Anything else does nothing behind it.
bool NanoMenu::biosDlgKey(uint16_t code) {
    if (!biosDlgShown()) return false;
    switch (code) {
    case BTN_SOUTH: case KEY_ENTER: case KEY_KPENTER: biosDlgSelect(); return true;
    case BTN_EAST: case KEY_BACK: case KEY_ESC:       biosDlgBack(); return true;
    case BTN_TL: biosDlgPage(-1); return true;
    case BTN_TR: biosDlgPage(1); return true;
    case BTN_START:
        if (mBiosDlgMode == BIOS_REPORT) biosDlgSelect();
        return true;
    case BTN_NORTH: case BTN_WEST: case BTN_SELECT: case BTN_TL2: case BTN_TR2:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

void NanoMenu::biosBuildLines(float fs, float maxW) {
    if (!mBiosLinesDirty && fs == mBiosLinesFs && maxW == mBiosLinesW && mBiosLinesMode == mBiosDlgMode) return;
    mBiosLinesDirty = false;
    mBiosLinesFs = fs; mBiosLinesW = maxW; mBiosLinesMode = mBiosDlgMode;
    mBiosLines.clear();
    mBiosRowFirst.clear(); mBiosRowLast.clear();
    mBiosLinkFirst = mBiosLinkLast = -1;
    const float ind = measureText("MM", fs);
    auto add = [&](const std::string& text, int style, int row, float indent, float width) {
        for (const std::string& l : wrapTextToWidth(text, fs, std::max(40.0f, width - indent))) {
            BiosLine bl; bl.text = l; bl.style = style; bl.row = row; bl.indent = indent;
            mBiosLines.push_back(bl);
        }
    };
    auto gap = [&]() { BiosLine bl; bl.style = kGap; mBiosLines.push_back(bl); };
    auto files = [&](const std::vector<BiosDialogFile>& v, int row) {
        for (const auto& f : v) {
            add(f.desc.empty() ? f.name : f.name + "  (" + trDyn(f.desc.c_str()) + ")", kFile, row, ind, maxW);
            if (!f.sha1.empty()) add("SHA-1 " + f.sha1, kSha, row, ind, maxW);
        }
    };
    const std::string altLabel = std::string(trDyn("Alternates")) + " \xE2\x80\xBA";
    const float linkW = measureText(altLabel.c_str(), fs) + ind;

    if (mBiosDlgMode == BIOS_LIST) {
        mBiosRowFirst.assign(mBiosRows.size(), 0);
        mBiosRowLast.assign(mBiosRows.size(), 0);
        add(trDyn("Thimsy doesn't include BIOS files. If you have BIOS files dumped from your own consoles, "
                  "Thimsy can copy them to the emulators that need them."), kPara, -1, 0.0f, maxW);
        if (mBiosDlgSetup) {
            const std::string where = std::string(trDyn("Game Settings")) + " > " + trDyn("Install Your Own BIOS Files");
            char later[600];
            snprintf(later, sizeof(later), trDyn("This step is optional: most systems work without BIOS files. "
                                                 "You can add them any time later from %s."), where.c_str());
            add(later, kHead, -1, 0.0f, maxW);
        }
        for (int pass = 0; pass < 2; pass++) {
            const bool req = pass == 0;
            bool any = false;
            for (const auto& r : mBiosRows) any = any || r.required == req;
            if (!any) continue;
            if (!mBiosLines.empty()) gap();
            add(trDyn(req ? "Some of the emulators in Thimsy require additional files to run:"
                          : "These emulators don't require BIOS files, but a BIOS can improve compatibility or add features:"),
                kHead, -1, 0.0f, maxW);
            for (int i = 0; i < (int)mBiosRows.size(); i++) {
                const BiosDialogRow& r = mBiosRows[i];
                if (r.required != req) continue;
                mBiosRowFirst[i] = (int)mBiosLines.size();
                std::string sys = trDyn(r.system.c_str());
                if (!r.emulators.empty()) sys += " \xC2\xB7 " + r.emulators;
                const size_t at = mBiosLines.size();
                add(sys, kSys, i, 0.0f, r.alternates.empty() ? maxW : maxW - linkW);
                if (!r.alternates.empty()) mBiosLines[at].link = true;
                if (!r.qualifier.empty()) {
                    char q[300];
                    snprintf(q, sizeof(q), trDyn("Needed for %s"), trDyn(r.qualifier.c_str()));
                    add(q, kSub, i, ind, maxW);
                }
                files(r.recommended, i);
                if (!r.note.empty()) add(trDyn(r.note.c_str()), kSub, i, ind, maxW);
                mBiosRowLast[i] = (int)mBiosLines.size() - 1;
            }
        }
        {   // the sentence with the All Supported Files link in it ("this list")
            const std::string fmt = trDyn("Many more files can be installed. Check %s for a complete set of supported files.");
            const std::string link = trDyn("this list");
            const size_t at = fmt.find("%s");
            const std::string text = at == std::string::npos ? fmt + " " + link : fmt.substr(0, at) + link + fmt.substr(at + 2);
            const size_t l0 = at == std::string::npos ? fmt.size() + 1 : at, l1 = l0 + link.size();
            gap();
            mBiosLinkFirst = (int)mBiosLines.size();
            size_t cur = 0;
            for (const std::string& l : wrapTextToWidth(text, fs, maxW)) {
                BiosLine bl; bl.text = l; bl.style = kPara;
                const size_t pos = text.find(l, cur);
                if (pos != std::string::npos) {
                    cur = pos + l.size();
                    const size_t a = std::max(pos, l0), b = std::min(cur, l1);
                    if (a < b) { bl.linkFrom = (int)(a - pos); bl.linkTo = (int)(b - pos); }
                }
                mBiosLines.push_back(bl);
            }
            mBiosLinkLast = (int)mBiosLines.size() - 1;
        }
        const std::string none = biosNeedNothingLine();
        if (!none.empty()) { gap(); add(trDyn(none.c_str()), kPara, -1, 0.0f, maxW); }
        gap();
        char how[400];
        snprintf(how, sizeof(how), trDyn("Have your own BIOS files? Choose %s and pick the folder they're in. If not, choose %s."),
                 trDyn("Select Folder"), trDyn("Skip"));
        add(how, kPara, -1, 0.0f, maxW);
    } else if (mBiosDlgMode == BIOS_ALTS && mBiosAltRow >= 0 && mBiosAltRow < (int)mBiosRows.size()) {
        const BiosDialogRow& r = mBiosRows[mBiosAltRow];
        std::string sys = trDyn(r.system.c_str());
        if (!r.emulators.empty()) sys += " \xC2\xB7 " + r.emulators;
        add(sys, kSys, -1, 0.0f, maxW);
        add(trDyn("Thimsy recognises and installs any of these versions too."), kSub, -1, 0.0f, maxW);
        for (int i = 0; i < (int)r.alternates.size(); i++) {
            gap();
            mBiosRowFirst.push_back((int)mBiosLines.size());
            files({r.alternates[i]}, i);
            mBiosRowLast.push_back((int)mBiosLines.size() - 1);
        }
    } else if (mBiosDlgMode == BIOS_ALL) {
        // Everything the installer takes, by system: name and size, the SHA-1 (or how it is
        // recognised), and how to put it in the folder.
        add(trDyn(biosAllFilesIntro().c_str()), kPara, -1, 0.0f, maxW);
        const std::vector<BiosAllGroup> groups = biosAllFiles();
        for (const auto& g : groups) {
            gap();
            std::string sys = trDyn(g.system.c_str());
            if (!g.emulators.empty()) sys += " \xC2\xB7 " + g.emulators;
            add(sys, kHead, -1, 0.0f, maxW);
            for (const auto& f : g.files) {
                add(f.size.empty() ? f.name : f.name + "  \xC2\xB7  " + trDyn(f.size.c_str()), kFile, -1, ind, maxW);
                bool hex = f.check.size() == 40;
                for (char c : f.check) hex = hex && isxdigit((unsigned char)c);
                if (!f.check.empty()) add(hex ? "SHA-1 " + f.check : std::string(trDyn(f.check.c_str())), kSha, -1, ind, maxW);
                if (!f.how.empty()) add(trDyn(f.how.c_str()), kSub, -1, ind, maxW);
            }
            if (!g.note.empty()) add(trDyn(g.note.c_str()), kSub, -1, 0.0f, maxW);
        }
    } else if (mBiosDlgMode == BIOS_PICK) {
        for (int i = 0; i < (int)mBiosPick.items.size(); i++) {
            const Ps3Item& it = mBiosPick.items[i];
            std::string label = it.label;
            if (it.kind == PS3_GS_SELFOLDER || label == "Internal storage") label = trDyn(label.c_str());
            // One line an entry, cut to fit.
            const float w = maxW - ind;
            if (measureText(label.c_str(), fs) > w) {
                while (label.size() > 1 && measureText((label + "...").c_str(), fs) > w) {
                    biosDropLastCp(label);
                }
                label += "...";
            }
            BiosLine bl; bl.text = label; bl.style = it.kind == PS3_GS_SELFOLDER ? kSys : kFile; bl.row = i;
            bl.indent = ind * 0.5f;
            mBiosRowFirst.push_back((int)mBiosLines.size());
            mBiosRowLast.push_back((int)mBiosLines.size());
            mBiosLines.push_back(bl);
        }
    } else if (mBiosDlgMode == BIOS_REPORT) {
        const BiosInstallProgress& p = mBiosReport;
        char b[200];
        if (!p.done) {   // it did not run: only why
            for (const auto& l : p.lines) add(l, kPara, -1, 0.0f, maxW);
            biosDlgEnsureVisible();
            return;
        }
        if (p.noneFound) {
            add(trDyn("No BIOS files that Thimsy recognises were found in this folder. Thimsy doesn't include BIOS "
                      "files: you need to provide your own, dumped from your consoles."), kPara, -1, 0.0f, maxW);
            gap();
        }
        snprintf(b, sizeof(b), trDyn("Installed: %d"), p.installed);                          add(b, kPara, -1, 0.0f, maxW);
        snprintf(b, sizeof(b), trDyn("Already installed: %d"), p.already);                    add(b, kPara, -1, 0.0f, maxW);
        snprintf(b, sizeof(b), trDyn("Kept, a different file is already there: %d"), p.conflicts); add(b, kPara, -1, 0.0f, maxW);
        snprintf(b, sizeof(b), trDyn("Not recognised: %d"), p.unknown);                       add(b, kPara, -1, 0.0f, maxW);
        if (!p.lines.empty()) gap();
        for (const auto& l : p.lines) add(l, kSub, -1, 0.0f, maxW);   // translated by the installer
    }
    if (mBiosDlgMode == BIOS_LIST || mBiosDlgMode == BIOS_PICK) biosDlgEnsureVisible();
}

// The smallest touch target: about 44 px on a 640x480 screen, a little more on bigger ones.
static float biosMinTap(int w, int h) {
    const float unit = (float)std::min(w, h);
    return 44.0f * std::max(1.0f, unit / 480.0f * 0.8f);
}

// The lines in (x, y, w, h), the selected row, the links, then the buttons centred on btnCy. Touch
// zones for all of it on the primary panel.
void NanoMenu::biosDrawPage(float x, float y, float w, float h, float fs, float btnCy, const float* pal, float a) {
    biosBuildLines(fs, w);
    const int mode = mBiosDlgMode;
    const float minTap = biosMinTap(mWidth, mHeight);
    float lineH = (float)FONT_CHAR_H * fs * 1.32f;
    if (mode == BIOS_PICK) lineH = std::max(lineH, minTap);   // one entry a line: a fingertip tall
    const float textOff = (lineH - (float)FONT_CHAR_H * fs) * 0.5f;
    mBiosVisible = std::max(1, (int)(h / lineH));
    const int n = (int)mBiosLines.size();
    const int maxScroll = std::max(0, n - mBiosVisible);
    if (mBiosScroll > maxScroll) mBiosScroll = maxScroll;
    const bool touch = mRenderingPanel == 0 && !(mBiosDlgSetup && mSetupTransitioning);
    const int rows = (int)mBiosRows.size();
    const bool listMode = mode == BIOS_LIST || mode == BIOS_PICK;
    auto tapGrown = [&](const TouchRect& r, TouchZones::Action act) {
        mTouch.tapMargin(r.grownTo(minTap, minTap), act);
        mTouch.tap(r, act);
    };

    if (touch) {
        mTouch.scroll({x, y, w, h}, TouchZones::kUpDown, lineH, [this, mode](int dir) {
            return biosDlgShown() && mBiosDlgMode == mode && biosDlgScroll(dir);
        });
    }
    const std::string altLabel = std::string(trDyn("Alternates")) + " \xE2\x80\xBA";
    const int last = std::min(n, mBiosScroll + mBiosVisible);
    // The selected row (list, picker).
    const int selRow = listMode && mBiosSel < (int)mBiosRowFirst.size() ? mBiosSel : -1;
    if (selRow >= 0) {
        const int f = std::max(mBiosRowFirst[selRow], mBiosScroll), l = std::min(mBiosRowLast[selRow], last - 1);
        if (l >= f) {
            const float ry = y + (float)(f - mBiosScroll) * lineH;
            drawRoundedRect(x - fs * 6.0f, ry, w + fs * 12.0f, (float)(l - f + 1) * lineH, fs * 4.0f,
                            pal[kSelR], pal[kSelG], pal[kSelB], pal[kSelA] * a);
        }
    }
    // A tap on a row selects it (a picker entry opens). Before the links, which sit on top.
    if (touch && listMode) {
        const int count = (int)mBiosRowFirst.size();
        for (int rI = 0; rI < count; rI++) {
            const int f = std::max(mBiosRowFirst[rI], mBiosScroll), l = std::min(mBiosRowLast[rI], last - 1);
            if (l < f) continue;
            const TouchRect rr{x, y + (float)(f - mBiosScroll) * lineH, w, (float)(l - f + 1) * lineH};
            mTouch.tap(rr, [this, rI, mode]() {
                if (!biosDlgShown() || mBiosDlgMode != mode) return;
                if (mode == BIOS_PICK) biosDlgActivate(rI);
                else if (mBiosSel == rI && rI < (int)mBiosRows.size() && !mBiosRows[rI].alternates.empty())
                    biosDlgActivate(rI);   // a second tap on the row opens its Alternates
                else { mBiosSel = rI; mDisplayDirty = true; }
            });
        }
    }
    const bool linkFocused = mode == BIOS_LIST && mBiosSel == rows;
    for (int i = mBiosScroll; i < last; i++) {
        const BiosLine& ln = mBiosLines[i];
        const float ly = y + (float)(i - mBiosScroll) * lineH;
        const float ty = ly + textOff;
        if (ln.style == kGap) continue;
        // A thin rule above each list row.
        if (mode == BIOS_LIST && ln.row >= 0 && ln.row < (int)mBiosRowFirst.size() && mBiosRowFirst[ln.row] == i
            && i > mBiosScroll && mBiosLines[i - 1].style != kHead && mBiosLines[i - 1].style != kGap)
            drawQuad(x, ly, w, std::max(1.0f, fs), pal[kT2R], pal[kT2G], pal[kT2B], 0.25f * a);
        float r = pal[kT1R], g = pal[kT1G], b = pal[kT1B];
        if (ln.style == kHead) { r = pal[kAccR]; g = pal[kAccG]; b = pal[kAccB]; }
        else if (ln.style == kSub || ln.style == kSha) { r = pal[kT2R]; g = pal[kT2G]; b = pal[kT2B]; }
        if (ln.linkFrom >= 0) {
            // The sentence with the All Supported Files link: before, the link (accent, underlined), after.
            const std::string pre = ln.text.substr(0, ln.linkFrom);
            const std::string mid = ln.text.substr(ln.linkFrom, ln.linkTo - ln.linkFrom);
            const std::string post = ln.text.substr(ln.linkTo);
            // Left to right, or in Arabic and Hebrew right to left (the sentence's start on the right).
            const float x0 = x + ln.indent;
            const float pw = measureText(pre.c_str(), fs), mw = measureText(mid.c_str(), fs);
            const float qw = measureText(post.c_str(), fs);
            const bool rtl = nanoTextIsRtl(ln.text.c_str());
            const float xPre = rtl ? x0 + qw + mw : x0, x1 = rtl ? x0 + qw : x0 + pw, xPost = rtl ? x0 : x0 + pw + mw;
            if (linkFocused)
                drawRoundedRect(x1 - fs * 3.0f, ly, mw + fs * 6.0f, lineH, fs * 4.0f,
                                pal[kSelR], pal[kSelG], pal[kSelB], pal[kSelA] * 1.6f * a);
            drawText(pre.c_str(), xPre, ty, fs, r, g, b, a);
            drawText(mid.c_str(), x1, ty, fs, pal[kAccR], pal[kAccG], pal[kAccB], a);
            drawQuad(x1, ty + (float)FONT_CHAR_H * fs, mw, std::max(1.0f, fs * 0.9f), pal[kAccR], pal[kAccG], pal[kAccB], 0.7f * a);
            drawText(post.c_str(), xPost, ty, fs, r, g, b, a);
            if (touch) {
                const int focus = rows;
                tapGrown({x1, ly, mw, lineH}, [this, focus]() {
                    if (!biosDlgShown() || mBiosDlgMode != BIOS_LIST) return;
                    biosDlgActivate(focus);
                });
            }
            continue;
        }
        drawText(ln.text.c_str(), x + ln.indent, ty, fs, r, g, b, a);
        if (ln.link) {   // Alternates, at the right of the row's first line; framed when the row has the focus
            const float lw = measureText(altLabel.c_str(), fs);
            const float lx = x + w - lw;
            if (mode == BIOS_LIST && mBiosSel == ln.row)
                drawRoundedRect(lx - fs * 4.0f, ly, lw + fs * 8.0f, lineH, lineH * 0.5f,
                                pal[kAccR], pal[kAccG], pal[kAccB], 0.22f * a);
            drawText(altLabel.c_str(), lx, ty, fs, pal[kAccR], pal[kAccG], pal[kAccB], a);
            drawQuad(lx, ty + (float)FONT_CHAR_H * fs, lw, std::max(1.0f, fs * 0.9f),
                     pal[kAccR], pal[kAccG], pal[kAccB], 0.7f * a);
            if (touch && ln.row >= 0 && ln.row < (int)mBiosRowFirst.size()) {
                // A fingertip-sized target, kept inside its row; over the row's own zone.
                const int row = ln.row;
                const int f = std::max(mBiosRowFirst[row], mBiosScroll), l = std::min(mBiosRowLast[row], last - 1);
                const TouchRect rowRect{x, y + (float)(f - mBiosScroll) * lineH, w, (float)(l - f + 1) * lineH};
                mTouch.tap(TouchRect{lx, ly, lw, lineH}.grownTo(minTap, minTap).clippedTo(rowRect), [this, row]() {
                    if (!biosDlgShown() || mBiosDlgMode != BIOS_LIST) return;
                    biosDlgActivate(row);
                });
            }
        }
    }
    // Scrollbar: where the page is.
    if (n > mBiosVisible) {
        const float trackH = (float)mBiosVisible * lineH;
        const float barW = std::max(2.0f, fs * 3.0f);
        const float thumbH = std::max(lineH, trackH * (float)mBiosVisible / (float)n);
        const float thumbY = y + (trackH - thumbH) * (float)mBiosScroll / (float)std::max(1, maxScroll);
        const float bx = x + w + fs * 8.0f;
        drawRoundedRect(bx, y, barW, trackH, barW * 0.5f, pal[kT2R], pal[kT2G], pal[kT2B], 0.18f * a);
        drawRoundedRect(bx, thumbY, barW, thumbH, barW * 0.5f, pal[kAccR], pal[kAccG], pal[kAccB], 0.85f * a);
    }

    biosDrawButtons(x, w, btnCy, fs, pal, a);
}

// The page's buttons, centred in (x, w) on btnCy: always in view. Progress has Cancel.
void NanoMenu::biosDrawButtons(float x, float w, float btnCy, float fs, const float* pal, float a) {
    const int mode = mBiosDlgMode;
    const int rows = (int)mBiosRows.size();
    const float minTap = biosMinTap(mWidth, mHeight);
    const bool touch = mRenderingPanel == 0 && !(mBiosDlgSetup && mSetupTransitioning);
    std::vector<std::string> btns;
    if (mode == BIOS_LIST) { btns.push_back(trDyn("Select Folder")); btns.push_back(trDyn("Skip")); }
    else if (mode == BIOS_ALTS || mode == BIOS_ALL || mode == BIOS_PICK) btns.push_back(trDyn("Back"));
    else if (mode == BIOS_REPORT) btns.push_back(trDyn(mBiosDlgSetup ? "Continue" : "OK"));
    else if (mode == BIOS_PROGRESS && !mBiosWaitEarlier) btns.push_back(trDyn(mBiosCancelAsked ? "Cancelling..." : "Cancel"));
    if (btns.empty()) return;
    const float bfs = fs * 1.05f;
    const float bh = std::max((float)FONT_CHAR_H * bfs * 1.9f, minTap);
    float bw = 0.0f;
    for (const auto& s : btns) bw = std::max(bw, measureText(s.c_str(), bfs) + bh * 1.2f);
    bw = std::max(bw, minTap * 2.2f);
    const float gapW = bh * 0.5f;
    const float total = bw * (float)btns.size() + gapW * (float)(btns.size() - 1);
    float bx = x + (w - total) * 0.5f;
    for (int i = 0; i < (int)btns.size(); i++, bx += bw + gapW) {
        // The list's buttons take the focus in turn; the single button of the other pages is the
        // one A presses (the picker's A opens its entry, so its Back is not framed; Cancelling...
        // is pressed already).
        const bool focused = mode == BIOS_LIST ? mBiosSel == rows + 1 + i
                           : mode == BIOS_PROGRESS ? !mBiosCancelAsked : mode != BIOS_PICK;
        const float by = btnCy - bh * 0.5f;
        if (focused) drawRoundedRect(bx, by, bw, bh, bh * 0.5f, pal[kAccR], pal[kAccG], pal[kAccB], 0.95f * a);
        else drawRoundedRect(bx, by, bw, bh, bh * 0.5f, pal[kBtnR], pal[kBtnG], pal[kBtnB], pal[kBtnA] * a);
        const float tw = measureText(btns[i].c_str(), bfs);
        const float ink = pal[kBtnInk];
        const float tx = bx + (bw - tw) * 0.5f, tyB = btnCy - (float)FONT_CHAR_H * bfs * 0.5f;
        if (focused) drawText(btns[i].c_str(), tx, tyB, bfs, ink, ink, ink, a);
        else drawText(btns[i].c_str(), tx, tyB, bfs, pal[kT1R], pal[kT1G], pal[kT1B], a);
        if (touch) {
            const int focus = mode == BIOS_LIST ? rows + 1 + i : 0;
            mTouch.press(TouchRect{bx, by, bw, bh}, [this, mode, focus]() {
                if (!biosDlgShown() || mBiosDlgMode != mode) return;
                if (mode == BIOS_PROGRESS) biosDlgCancel();
                else if (mode == BIOS_PICK || mode == BIOS_ALTS || mode == BIOS_ALL) biosDlgBack();
                else biosDlgActivate(focus);
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Game Settings modal
// ---------------------------------------------------------------------------

void NanoMenu::renderBiosDialog() {
    if (!mBiosDlgActive) return;
    biosDlgTick();
    if (!mBiosDlgActive) return;
    const float W = (float)mWidth, H = (float)mHeight;
    const float unit = std::min(W, H);
    const float fs = unit / 720.0f * 1.05f * ps3DlgFontBoost();

    // The boxart progress modal's colours: XMB (and ES-DE) dark panel, DSi message box, Minima card.
    float pal[kPalN];
    float panR, panG, panB, panA, dimA, rad;
    if (mNdsTheme) {
        const NdsPal np = ndsPal();
        ndsAccentRGB(pal[kAccR], pal[kAccG], pal[kAccB]);
        panR = panG = panB = np.dlgBg; panA = 1.0f;
        pal[kT1R] = pal[kT1G] = pal[kT1B] = np.dlgInk;
        const float sub = mNdsDark ? 0.70f : 0.40f;
        pal[kT2R] = pal[kT2G] = pal[kT2B] = sub;
        pal[kSelR] = pal[kAccR]; pal[kSelG] = pal[kAccG]; pal[kSelB] = pal[kAccB]; pal[kSelA] = 0.18f;
        pal[kBtnR] = pal[kBtnG] = pal[kBtnB] = mNdsDark ? 1.0f : 0.0f; pal[kBtnA] = 0.10f;
        dimA = 0.42f; rad = 8.0f * fs;
    } else if (mMinimaTheme) {
        minimaAccent(pal[kAccR], pal[kAccG], pal[kAccB]);
        panR = 0.06f; panG = 0.07f; panB = 0.09f; panA = 0.97f;
        pal[kT1R] = pal[kT1G] = pal[kT1B] = 1.0f;
        pal[kT2R] = 0.72f; pal[kT2G] = 0.75f; pal[kT2B] = 0.80f;
        pal[kSelR] = pal[kSelG] = pal[kSelB] = 1.0f; pal[kSelA] = 0.10f;
        pal[kBtnR] = pal[kBtnG] = pal[kBtnB] = 1.0f; pal[kBtnA] = 0.10f;
        dimA = 0.55f; rad = 10.0f * fs;
    } else {
        pal[kAccR] = 0.47f; pal[kAccG] = 0.78f; pal[kAccB] = 1.0f;
        panR = 0.07f; panG = 0.08f; panB = 0.10f; panA = 0.94f;
        pal[kT1R] = pal[kT1G] = pal[kT1B] = 1.0f;
        pal[kT2R] = 0.75f; pal[kT2G] = 0.78f; pal[kT2B] = 0.82f;
        pal[kSelR] = pal[kSelG] = pal[kSelB] = 1.0f; pal[kSelA] = 0.12f;
        pal[kBtnR] = pal[kBtnG] = pal[kBtnB] = 1.0f; pal[kBtnA] = 0.10f;
        dimA = 0.55f; rad = 0.0f;
    }
    // Text on the accent: dark on a light accent.
    const float lum = 0.299f * pal[kAccR] + 0.587f * pal[kAccG] + 0.114f * pal[kAccB];
    pal[kBtnInk] = lum > 0.6f ? 0.08f : 1.0f;

    const bool touch = mRenderingPanel == 0;
    drawQuad(0, 0, W, H, 0.0f, 0.0f, 0.0f, dimA);
    const float pw = std::min(W * 0.92f, unit * 1.5f), ph = H * 0.92f;
    const float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
    if (touch) { mTouch.block({0.0f, 0.0f, W, H}); }
    if (rad > 0.0f) {
        drawRoundedRect(px, py + 3.0f * fs, pw, ph, rad, 0.0f, 0.0f, 0.0f, 0.35f);
        drawRoundedRect(px, py, pw, ph, rad, panR, panG, panB, panA);
        drawRoundedRect(px, py, pw, 3.0f * fs, rad, pal[kAccR], pal[kAccG], pal[kAccB], 0.95f);
    } else {
        drawQuad(px, py, pw, ph, panR, panG, panB, panA);
        drawQuad(px, py, pw, 3.0f * fs, pal[kAccR], pal[kAccG], pal[kAccB], 0.9f);
    }
    const float pad = unit * 0.035f;
    float tfs = fs * 1.35f;
    const char* title = mBiosDlgMode == BIOS_ALTS ? trDyn("Alternates")
                      : mBiosDlgMode == BIOS_ALL ? trDyn("All Supported Files")
                      : mBiosDlgMode == BIOS_PICK ? trDyn("Choose the Folder With Your BIOS Files")
                      : mBiosDlgMode == BIOS_LIST ? trDyn("BIOS Files") : trDyn("Installing BIOS Files");
    float ty = py + pad;
    if (measureText(title, tfs) > pw - pad * 2.0f) tfs *= (pw - pad * 2.0f) / measureText(title, tfs);   // a long translation
    drawText(title, px + (pw - measureText(title, tfs)) * 0.5f, ty, tfs, pal[kAccR], pal[kAccG], pal[kAccB], 1.0f);
    ty += (float)FONT_CHAR_H * tfs * 1.5f;
    if (mBiosDlgMode == BIOS_PICK) {   // where the list is
        std::string where = biosShownPath(mGsFolderPath);
        const float sfs = fs * 0.9f;
        while (where.size() > 4 && measureText(where.c_str(), sfs) > pw - pad * 2.0f) biosDropFirstCp(where);
        drawText(where.c_str(), px + (pw - measureText(where.c_str(), sfs)) * 0.5f, ty, sfs,
                 pal[kT2R], pal[kT2G], pal[kT2B], 1.0f);
        ty += (float)FONT_CHAR_H * sfs * 1.5f;
    }
    // Footer: the hint line under the buttons.
    const float hfs = fs * 0.85f;
    const float hintY = py + ph - pad * 0.6f - (float)FONT_CHAR_H * hfs;
    const float btnH = std::max((float)FONT_CHAR_H * fs * 1.05f * 1.9f, biosMinTap(mWidth, mHeight));
    const float btnCy = hintY - pad * 0.4f - btnH * 0.5f;
    const float bodyBot = btnCy - btnH * 0.5f - pad * 0.6f;
    const float bodyX = px + pad, bodyW = pw - pad * 2.0f - fs * 12.0f;   // room for the scrollbar

    if (mBiosDlgMode == BIOS_PROGRESS) {
        BiosInstallProgress p = biosInstallProgress();
        if (mBiosWaitEarlier) p = BiosInstallProgress();   // a re-install has no progress of its own
        const float cx = W * 0.5f, cy = (ty + bodyBot) * 0.5f;
        char line[160];
        if (mBiosWaitEarlier) snprintf(line, sizeof(line), "%s", trDyn("Finishing an earlier BIOS install..."));
        else if (p.total > 0) snprintf(line, sizeof(line), trDyn("%d / %d files"), p.scanned, p.total);
        else snprintf(line, sizeof(line), "%s", trDyn("Looking for files..."));
        drawText(line, cx - measureText(line, fs * 1.2f) * 0.5f, cy - (float)FONT_CHAR_H * fs * 2.2f, fs * 1.2f,
                 pal[kT1R], pal[kT1G], pal[kT1B], 1.0f);
        const float barW = pw * 0.7f, barH = 6.0f * fs;
        drawRoundedRect(cx - barW * 0.5f, cy, barW, barH, barH * 0.5f, pal[kT2R], pal[kT2G], pal[kT2B], 0.28f);
        float frac = p.total > 0 ? (float)p.scanned / (float)p.total : 0.0f;
        if (p.total <= 0) {   // still counting: a block that sweeps
            const float ph2 = fmodf((float)elapsedRealtime(), 1300.0f) / 1300.0f;
            const float bw = barW * 0.18f, bx = cx - barW * 0.5f + (barW - bw) * ph2;
            drawRoundedRect(bx, cy, bw, barH, barH * 0.5f, pal[kAccR], pal[kAccG], pal[kAccB], 1.0f);
        } else if (frac > 0.0f) {
            drawRoundedRect(cx - barW * 0.5f, cy, barW * std::min(1.0f, frac), barH, barH * 0.5f,
                            pal[kAccR], pal[kAccG], pal[kAccB], 1.0f);
        }
        std::string cur = p.current;
        const float cfs = fs * 0.9f;
        if (measureText(cur.c_str(), cfs) > pw * 0.9f) {
            while (cur.size() > 4 && measureText(("..." + cur).c_str(), cfs) > pw * 0.9f) biosDropFirstCp(cur);
            cur = "..." + cur;
        }
        drawText(cur.c_str(), cx - measureText(cur.c_str(), cfs) * 0.5f, cy + barH + (float)FONT_CHAR_H * fs, cfs,
                 pal[kT2R], pal[kT2G], pal[kT2B], 1.0f);
        biosDrawButtons(bodyX, bodyW, btnCy, fs, pal, 1.0f);   // Cancel
        const char* wait = trDyn("Please wait...");
        drawText(wait, cx - measureText(wait, hfs) * 0.5f, hintY, hfs, pal[kT2R], pal[kT2G], pal[kT2B], 0.9f);
        return;
    }
    biosDrawPage(bodyX, ty, bodyW, bodyBot - ty, fs, btnCy, pal, 1.0f);
    std::string hint;
    if (mBiosDlgMode == BIOS_LIST || mBiosDlgMode == BIOS_PICK) hint = themeButtonText(trDyn("Cross: Select | Circle: Back"));
    else if (mBiosDlgMode == BIOS_ALTS || mBiosDlgMode == BIOS_ALL) hint = themeButtonText(trDyn("Press Cross or Circle to close"));
    if (!hint.empty()) {
        const float hx = W * 0.5f - measureText(hint.c_str(), hfs) * 0.5f;
        drawText(hint.c_str(), hx, hintY, hfs, pal[kT2R], pal[kT2G], pal[kT2B], 0.9f);
        if (touch) {
            if (mBiosDlgMode == BIOS_ALTS || mBiosDlgMode == BIOS_ALL) touchHintText(hint, hx, hintY, hfs, {BTN_EAST});
            else touchHintText(hint, hx, hintY, hfs, {BTN_SOUTH, BTN_EAST});
        }
    }
}

// ---------------------------------------------------------------------------
// Setup pages: the PS3 dialog layout of the other steps (title, dividers, footer).
// ---------------------------------------------------------------------------

namespace {
struct SetupFrame {
    float S, offX, offY, fb;
    float X(float vx) const { return S * vx + offX; }
    float XC(float vx) const { return S * ps3::XCF(vx) + offX; }
    float Y(float vy) const { return S * vy + offY; }
    float DS(float v) const { return S * v; }
    float FS(float px) const { return S * px * fb / 16.0f; }
};
}  // namespace

#define BIOS_SETUP_FRAME(fr)                                                                         \
    { ps3::LayoutParams lp; lp.panelW = mWidth; lp.panelH = mHeight;                                \
      lp.uiScale = mPs3UiScale; ps3::layoutCompute(lp); }                                           \
    float ui_ = mPs3UiScale; if (ui_ < 0.5f) ui_ = 0.5f; if (ui_ > 2.0f) ui_ = 2.0f;                \
    SetupFrame fr;                                                                                   \
    fr.S = ps3::gScale / ui_;                                                                        \
    fr.offX = ps3::gFrameX + (ps3::gFrameW - fr.S * ps3::XCF(ps3::VW)) * 0.5f + mSetupSlideOffset;   \
    fr.offY = ps3::gFrameY + ps3::gFrameH * 0.5f - fr.S * (ps3::VH * 0.5f);                         \
    fr.fb = ps3DlgFontBoost();

static const float kSetupInnerTop = 199.0f, kSetupInnerBot = 880.0f;

void NanoMenu::renderSetupBios() {
    if (!mBiosDlgSetup) biosDialogOpen(true);
    biosDlgTick();
    const float alpha = mSetupTransitionAlpha;
    BIOS_SETUP_FRAME(fr);
    const float VW = ps3::VW;
    const int savedMode = mTextOutlineMode;
    const float savedRatio = mTextOutlineRatio;
    mTextOutlineMode = 1; mTextOutlineRatio = 0.5f;

    const char* title = mBiosDlgMode == BIOS_ALTS ? "Alternates"
                      : mBiosDlgMode == BIOS_ALL ? "All Supported Files"
                      : mBiosDlgMode == BIOS_PICK ? "Choose the Folder With Your BIOS Files"
                      : mBiosDlgMode == BIOS_LIST ? "BIOS Files (Optional)" : "Installing BIOS Files";
    ps3DlgText(title, fr.X(160.0f), fr.Y(187.0f), fr.FS(28.0f), 1, 1, 1, alpha, 0);
    const float divLw = fmaxf(1.0f, fr.DS(1.0f));
    drawQuad(ps3::gFrameX, fr.Y(kSetupInnerTop), ps3::gFrameW, divLw, 1, 1, 1, 0.55f * alpha);
    drawQuad(ps3::gFrameX, fr.Y(kSetupInnerBot), ps3::gFrameW, divLw, 1, 1, 1, 0.55f * alpha);

    // The XMB dialog colours; the modal's text size, so the page reads the same at 640x480.
    float pal[kPalN] = {0.47f, 0.78f, 1.0f, 1, 1, 1, 0.75f, 0.78f, 0.82f, 1, 1, 1, 0.14f, 1, 1, 1, 0.12f, 0.08f};
    const float unit = (float)std::min(mWidth, mHeight);
    const float fs = unit / 720.0f * 1.05f * fr.fb;
    const float left = fr.X(160.0f), right = fr.XC(VW - 160.0f);
    const float width = right - left - fs * 12.0f;   // room for the scrollbar
    const float pad = unit * 0.015f;
    float top = fr.Y(kSetupInnerTop) + pad * 2.0f;
    const bool touch = mRenderingPanel == 0 && !mSetupTransitioning;
    if (mBiosDlgMode == BIOS_PICK) {   // where the list is
        std::string where = biosShownPath(mGsFolderPath);
        const float sfs = fs * 0.9f;
        while (where.size() > 4 && measureText(where.c_str(), sfs) > right - left) biosDropFirstCp(where);
        drawText(where.c_str(), left, top, sfs, 0.75f, 0.78f, 0.82f, alpha);
        top += (float)FONT_CHAR_H * sfs * 1.6f;
    }
    const float bh = std::max((float)FONT_CHAR_H * fs * 1.05f * 1.9f, biosMinTap(mWidth, mHeight));
    const float btnCy = fr.Y(kSetupInnerBot) - pad - bh * 0.5f;
    const float bottom = btnCy - bh * 0.5f - pad;
    if (mBiosDlgMode == BIOS_PROGRESS) {
        BiosInstallProgress p = biosInstallProgress();
        if (mBiosWaitEarlier) p = BiosInstallProgress();   // a re-install has no progress of its own
        char line[160];
        if (mBiosWaitEarlier) snprintf(line, sizeof(line), "%s", trDyn("Finishing an earlier BIOS install..."));
        else if (p.total > 0) snprintf(line, sizeof(line), trDyn("%d / %d files"), p.scanned, p.total);
        else snprintf(line, sizeof(line), "%s", trDyn("Looking for files..."));
        ps3DlgText(line, fr.XC(VW * 0.5f), fr.Y(470.0f), fr.FS(24.0f), 1, 1, 1, alpha, 1);
        const float barW = (right - left) * 0.7f, barX = fr.XC(VW * 0.5f) - barW * 0.5f, barY = fr.Y(510.0f);
        const float barH = fr.DS(9.0f);
        drawQuad(barX, barY, barW, barH, 1, 1, 1, 0.10f * alpha);
        if (p.total > 0) {
            drawQuad(barX, barY, barW * std::min(1.0f, (float)p.scanned / (float)p.total), barH, 0.35f, 0.75f, 1.0f, 0.95f * alpha);
        } else {
            const float ph = fmodf((float)elapsedRealtime(), 1300.0f) / 1300.0f;
            const float bw = barW * 0.18f;
            drawQuad(barX + (barW - bw) * ph, barY, bw, barH, 0.35f, 0.75f, 1.0f, 0.95f * alpha);
        }
        std::string cur = p.current;
        const float cfs = fs * 0.9f;
        if (measureText(cur.c_str(), cfs) > right - left) {
            while (cur.size() > 4 && measureText(("..." + cur).c_str(), cfs) > right - left) biosDropFirstCp(cur);
            cur = "..." + cur;
        }
        drawText(cur.c_str(), fr.XC(VW * 0.5f) - measureText(cur.c_str(), cfs) * 0.5f, fr.Y(560.0f), cfs,
                 0.75f, 0.78f, 0.82f, alpha);
        biosDrawButtons(left, width, btnCy, fs, pal, alpha);   // Cancel
        ps3DlgText(tr(STR_SETUP_INSTALL_WAIT), fr.XC(VW * 0.5f), fr.Y(916.0f), fr.FS(20.0f), 0.9f, 0.9f, 0.95f, 0.9f * alpha, 1);
    } else {
        biosDrawPage(left, top, width, bottom - top, fs, btnCy, pal, alpha);
        std::string hint;
        if (mBiosDlgMode == BIOS_PICK) hint = themeButtonText(trDyn("Cross: Select | Circle: Back"));
        else if (mBiosDlgMode == BIOS_LIST) hint = themeButtonText(trDyn("Cross: Select"));
        else if (mBiosDlgMode != BIOS_REPORT) hint = themeButtonText(trDyn("Press Cross or Circle to close"));
        if (!hint.empty()) {
            const float hfs = fs * 0.85f;
            const float hx = fr.XC(VW * 0.5f) - measureText(hint.c_str(), hfs) * 0.5f, hy = fr.Y(kSetupInnerBot) + pad * 1.5f;
            drawText(hint.c_str(), hx, hy, hfs, 0.85f, 0.87f, 0.92f, 0.8f * alpha);
            if (touch) {
                if (mBiosDlgMode == BIOS_PICK) touchHintText(hint, hx, hy, hfs, {BTN_SOUTH, BTN_EAST});
                else if (mBiosDlgMode == BIOS_LIST) touchHintText(hint, hx, hy, hfs, {BTN_SOUTH});
                else touchHintText(hint, hx, hy, hfs, {BTN_EAST});
            }
        }
    }
    mTextOutlineMode = savedMode;
    mTextOutlineRatio = savedRatio;
}

// SETUP_SDCARD: two On/Off rows and Continue, laid out like the Date and Time step (rows at least a
// fingertip tall).
void NanoMenu::renderSetupSdCard() {
    const float alpha = mSetupTransitionAlpha;
    BIOS_SETUP_FRAME(fr);
    const float VW = ps3::VW;
    const int savedMode = mTextOutlineMode;
    const float savedRatio = mTextOutlineRatio;
    mTextOutlineMode = 1; mTextOutlineRatio = 0.5f;

    ps3DlgText("microSD Card", fr.X(160.0f), fr.Y(187.0f), fr.FS(28.0f), 1, 1, 1, alpha, 0);
    const float divLw = fmaxf(1.0f, fr.DS(1.0f));
    drawQuad(ps3::gFrameX, fr.Y(kSetupInnerTop), ps3::gFrameW, divLw, 1, 1, 1, 0.55f * alpha);
    drawQuad(ps3::gFrameX, fr.Y(kSetupInnerBot), ps3::gFrameW, divLw, 1, 1, 1, 0.55f * alpha);
    const float textW = fr.XC(VW - 220.0f) - fr.XC(220.0f);
    float y = fr.Y(kSetupInnerTop + 50.0f);
    {
        const float fs = fr.FS(20.0f);
        const char* intro = mSetupSdHasEsde
            ? "This microSD card has an ES-DE folder. Thimsy can use it and keep these files on the card:"
            : "A microSD card is inserted. Thimsy can keep these files on it instead of internal storage:";
        for (const auto& l : wrapTextToWidth(trDyn(intro), fs, textW)) {
            drawText(l.c_str(), fr.XC(VW * 0.5f) - measureText(l.c_str(), fs) * 0.5f, y, fs, 0.85f, 0.87f, 0.92f, alpha);
            y += (float)FONT_CHAR_H * fs * 1.3f;
        }
    }
    const char* labels[2] = {"Keep ES-DE on the microSD card", "Keep emulator files on the microSD card"};
    const bool on[2] = {mSetupSdEsde, mSetupSdEmu};
    const float rowFs = fr.FS(22.0f);
    const float step = std::max(fr.DS(80.0f), biosMinTap(mWidth, mHeight));
    const float lx = fr.XC(220.0f), rx = fr.XC(VW - 220.0f);
    const float hlX = fr.XC(180.0f), hlW = fr.XC(VW - 180.0f) - hlX;
    const bool touch = mRenderingPanel == 0 && !mSetupTransitioning;
    y += step * 0.4f;
    for (int i = 0; i < 3; i++) {
        if (i == 2) y += step * 0.25f;
        const float cy = y + step * 0.5f;   // the row's middle
        const bool sel = i == mSetupSdSel;
        const TouchRect rr{hlX, y, hlW, step};
        if (sel) drawRoundedRect(rr.x, rr.y + step * 0.06f, rr.w, step * 0.88f, step * 0.12f, 1, 1, 1, 0.14f * alpha);
        const float a = alpha * (sel ? 1.0f : 0.8f);
        if (touch) mTouch.tap(rr, [this, i]() {   // a tap toggles (or continues)
            if (!mSetupWizardActive || mSetupStep != SETUP_SDCARD || mSetupTransitioning) return;
            mSetupSdSel = i;
            setupSdCardSelect();
        });
        const float base = cy + (float)FONT_CHAR_H * rowFs * 0.3f;    // a baseline that centres the text
        if (i == 2) {
            ps3DlgText("Continue", fr.XC(VW * 0.5f), base, rowFs, 1, 1, 1, a, 1);
        } else {
            ps3DlgText(labels[i], lx, base, rowFs, 0.78f, 0.78f, 0.82f, a, 0);
            std::string v = trDyn(on[i] ? "On" : "Off");
            if (sel) v = "< " + v + " >";
            ps3DlgText(v.c_str(), rx, base, rowFs, on[i] ? 0.55f : 1.0f, on[i] ? 0.85f : 1.0f, 1.0f, a, 2);
        }
        y += step;
    }
    if (mSetupSdSel < 2) {   // what the selected option does
        const char* what = mSetupSdSel == 1
            ? (mSetupSdHasEmu
                   ? "Uses the emulator settings, button maps, saves and states on the card. Emulators the card has "
                     "nothing for move theirs there."
                   : "Moves emulator settings, saves and states to the microSD card, with Vita3K's installed games. "
                     "Saves kept beside the games stay there. Nothing is overwritten.")
            : mSetupSdHasEsde
            ? "Uses the ES-DE folder on the card: its games, collections, favourites, emulators and settings. "
              "Thimsy's own are added to it; the card's entries are kept."
            : "Moves the ES-DE folder, gamelists, collections, favourites and game media to the microSD card. "
              "The card then holds the only copy. Nothing on it is overwritten.";
        const float fs = fr.FS(17.0f);
        y += step * 0.2f;
        for (const auto& l : wrapTextToWidth(trDyn(what), fs, textW)) {
            if (y + (float)FONT_CHAR_H * fs > fr.Y(kSetupInnerBot)) break;
            drawText(l.c_str(), fr.XC(VW * 0.5f) - measureText(l.c_str(), fs) * 0.5f, y, fs, 0.75f, 0.78f, 0.82f, alpha);
            y += (float)FONT_CHAR_H * fs * 1.3f;
        }
    }
    const TouchRect h = ps3DlgHint(fr.XC(VW * 0.5f), true, mSetupSdSel == 2 ? "OK" : "Select", fr.Y(909.0f), fr.S, alpha);
    if (touch) touchButtonZone(BTN_SOUTH, h);
    mTextOutlineMode = savedMode;
    mTextOutlineRatio = savedRatio;
}

void NanoMenu::setupSdCardSelect() {
    if (mSetupSdSel == 0) mSetupSdEsde = !mSetupSdEsde;
    else if (mSetupSdSel == 1) mSetupSdEmu = !mSetupSdEmu;
    else advanceSetupStep();
    mDisplayDirty = true;
}

// SETUP_SDMOVE: ES-DE first, then the emulator files, one job at a time, then on to the BIOS step.
void NanoMenu::setupSdMoveTick() {
    if (mSetupTransitioning || mSetupStep != SETUP_SDMOVE || mSetupTransitionAlpha < 1.0f) return;
    fileJobTick();   // a finished job's end (sdMoveFinish) before the next starts
    bool running;
    { std::lock_guard<std::mutex> lk(mScrapeMutex); running = mScrapeRunning; }
    // A job, or the deletion of the internal copies the last one left, still going.
    const bool idle = mFileJobKind == 0 && !running && !sdAutoDeleting();
    switch (mSetupMovePhase) {
    case 0:
        mSdMoveAuto = true;
        mSetupMovePhase = (mSetupSdEsde && esdeMoveAuto()) ? 1 : 2;
        break;
    case 1: if (idle) mSetupMovePhase = 2; break;
    case 2:   // the card's emulator settings first (a reinstall gets its own back), then the move
        if (!idle) break;
        if (!mSetupSdEmu) { property_set("persist.gammaos.nano.emu_sd", "0"); mSetupMovePhase = 4; break; }
        if (emuSdSlotCard().empty()) { mSetupMovePhase = 4; break; }   // emulator files go on the microSD card only
        mEmuSyncRestored = 0;
        emuSettingsSyncAsync(emuSdSlotCard(), false, true);
        mSetupMovePhase = 5;
        break;
    case 5:
        if (mEmuSyncRunning.load() > 0) break;
        mSetupMovePhase = emuMoveAuto() ? 3 : 4;
        break;
    case 3: if (idle) mSetupMovePhase = 4; break;
    case 4: {
        mSdMoveAuto = false;
        const int64_t now = elapsedRealtime();
        if (mSetupMoveDoneMs == 0) {
            mSetupMoveDoneMs = now;
            if (mSdMoveAutoNote.empty() && mEmuSyncRestored.load() > 0)
                mSdMoveAutoNote = trDyn("Emulator settings were restored from the microSD card.");
            if (sdCardRoot().empty()) {   // taken out meanwhile: said instead of "no files"
                mSdMoveAutoNote = std::string(trDyn("The microSD card was removed.")) +
                                  (mSdMoveAutoNote.empty() ? "" : "\n" + mSdMoveAutoNote);
            }
        } else if (now - mSetupMoveDoneMs >= 10000) {
            mSetupMoveDoneMs = 0;
            advanceSetupStep();
        }
        break;
    }
    }
    mDisplayDirty = true;
}

void NanoMenu::renderSetupSdMove() {
    setupSdMoveTick();
    const float alpha = mSetupTransitionAlpha;
    BIOS_SETUP_FRAME(fr);
    const float VW = ps3::VW;
    const int savedMode = mTextOutlineMode;
    const float savedRatio = mTextOutlineRatio;
    mTextOutlineMode = 1; mTextOutlineRatio = 0.5f;

    ps3DlgText("Moving Files to the microSD Card", fr.X(160.0f), fr.Y(187.0f), fr.FS(28.0f), 1, 1, 1, alpha, 0);
    const float divLw = fmaxf(1.0f, fr.DS(1.0f));
    drawQuad(ps3::gFrameX, fr.Y(kSetupInnerTop), ps3::gFrameW, divLw, 1, 1, 1, 0.55f * alpha);
    drawQuad(ps3::gFrameX, fr.Y(kSetupInnerBot), ps3::gFrameW, divLw, 1, 1, 1, 0.55f * alpha);

    const bool done = mSetupMovePhase == 4;   // 5: the settings sync before the emulator move
    if (!done) {
        int d, t; std::string status;
        { std::lock_guard<std::mutex> lk(mScrapeMutex); d = mScrapeDone; t = mScrapeTotal; status = mScrapeStatus; }
        const char* what = mSetupMovePhase <= 1 ? "Moving ES-DE..." : "Moving emulator files...";
        if (mSetupMovePhase == 2 || mSetupMovePhase == 5) {   // the settings sync: no file job's counts yet
            what = "Getting emulator settings from the microSD card...";
            t = 0;
            status.clear();
        }
        ps3DlgText(what, fr.XC(VW * 0.5f), fr.Y(420.0f), fr.FS(24.0f), 1, 1, 1, alpha, 1);
        const float barW = fr.S * VW * 0.5f, barX = fr.XC(VW * 0.5f) - barW * 0.5f, barY = fr.Y(470.0f);
        const float barH = fr.DS(9.0f);
        drawQuad(barX, barY, barW, barH, 1, 1, 1, 0.10f * alpha);
        if (t > 0) {
            drawQuad(barX, barY, barW * std::min(1.0f, (float)d / (float)t), barH, 0.35f, 0.75f, 1.0f, 0.95f * alpha);
            char line[96];
            snprintf(line, sizeof(line), trDyn("%d / %d files"), d, t);
            ps3DlgText(line, fr.XC(VW * 0.5f), fr.Y(530.0f), fr.FS(18.0f), 0.85f, 0.87f, 0.92f, alpha, 1);
        } else {
            const float ph = fmodf((float)elapsedRealtime(), 1300.0f) / 1300.0f;
            const float bw = barW * 0.18f;
            drawQuad(barX + (barW - bw) * ph, barY, bw, barH, 0.35f, 0.75f, 1.0f, 0.95f * alpha);
        }
        const float cfs = fr.FS(16.0f);
        while (status.size() > 4 && measureText(status.c_str(), cfs) > barW * 1.6f) biosDropFirstCp(status);
        drawText(status.c_str(), fr.XC(VW * 0.5f) - measureText(status.c_str(), cfs) * 0.5f, fr.Y(560.0f), cfs,
                 0.75f, 0.78f, 0.82f, alpha);
        ps3DlgText(tr(STR_SETUP_INSTALL_WAIT), fr.XC(VW * 0.5f), fr.Y(916.0f), fr.FS(20.0f), 0.9f, 0.9f, 0.95f, 0.9f * alpha, 1);
    } else {
        // What the moves said (or that there was nothing to move), then on by itself.
        std::string text = mSdMoveAutoNote.empty() ? std::string(trDyn("There were no files to move.")) : mSdMoveAutoNote;
        const float fs = fr.FS(20.0f);
        float y = fr.Y(kSetupInnerTop + 90.0f);
        for (const auto& l : wrapTextToWidth(text, fs, fr.S * (VW - 400.0f))) {
            drawText(l.c_str(), fr.XC(VW * 0.5f) - measureText(l.c_str(), fs) * 0.5f, y, fs, 0.9f, 0.9f, 0.95f, alpha);
            y += (float)FONT_CHAR_H * fs * 1.35f;
        }
        const TouchRect h = ps3DlgHintG(fr.XC(VW * 0.5f), 2, "Continue", fr.Y(909.0f), fr.S, alpha);
        if (mRenderingPanel == 0 && !mSetupTransitioning) touchButtonZone(BTN_START, h);
        if (mSetupMoveDoneMs > 0) {
            int left = (int)((10000 - (elapsedRealtime() - mSetupMoveDoneMs) + 999) / 1000);
            left = std::clamp(left, 0, 10);
            char buf[64]; snprintf(buf, sizeof(buf), tr(STR_SETUP_INSTALL_AUTO), left);
            ps3DlgText(buf, fr.XC(VW * 0.5f), fr.Y(946.0f), fr.FS(18.0f), 0.9f, 0.9f, 0.95f, 0.75f * alpha, 1);
        }
    }
    mTextOutlineMode = savedMode;
    mTextOutlineRatio = savedRatio;
}

}  // namespace android
