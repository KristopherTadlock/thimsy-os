/*
 * Copyright (C) 2026 GammaOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

// Game Systems editor: the native raw-path folder picker (scan-source
// selection) and the add / remove custom-system flow. All native XMB screens
// (pushed Ps3Levels) - no Activities, no SAF picker. nano reads /storage,
// /storage/<UUID> and /mnt/media_rw/<UUID> directly (perms already relaxed).

#define LOG_TAG "GammaOSNano"

#include "NanoMenu.h"
#include "NanoI18n.h"      // trDyn() runtime translation of hardcoded UI strings
#include "NanoMenuPS3.h"   // ps3:: layout for the XMB Y prompt
#include "NanoAppFiles.h"  // sdCanon, sdUnder
#include "NanoLaunch.h"    // shellQuote
#include "NanoRaConfig.h"
#include "NanoScummvmList.h"
#include "NanoSdMove.h"       // sdStopIfRunning

#include <dirent.h>
#include <linux/input.h>   // BTN_WEST
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <stdlib.h>   // strtol, for the octal escapes in /proc/self/mountinfo
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <utils/Log.h>
#include <cutils/properties.h>   // property_set for the DraStic data-folder override (#90)

namespace android {

// One translated format with a single %s, e.g. folderFmt(trDyn("SD: %s"), name).
static std::string folderFmt(const char* fmt, const std::string& a) {
    char b[512];
    snprintf(b, sizeof(b), fmt, a.c_str());
    return b;
}

// Slugify a display name into a stable lowercase id ([a-z0-9_]).
static std::string slugify(const std::string& name) {
    std::string s;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s += c;
        else if (c >= 'A' && c <= 'Z') s += (char)(c - 'A' + 'a');
        else if (!s.empty() && s.back() != '_') s += '_';
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    if (s.empty()) s = "system";
    return s;
}

// A short label from a platform/display name: prefer the part after the last
// " - " (the actual system), else the whole thing, capped to ~8 chars.
static std::string shortFromName(const std::string& name) {
    std::string base = name;
    size_t d = base.rfind(" - ");
    if (d != std::string::npos) base = base.substr(d + 3);
    if (base.size() > 10) base = base.substr(0, 10);
    return base;
}

// ---- Add / remove custom systems ----

void NanoMenu::gsAddSystem() {
    // Open the emulator picker in "add" mode: picking a Daijishou platform/player
    // (or Custom) creates a brand-new system. mGsEditIdx is irrelevant here.
    mGsAddMode = true;
    mGsEditIdx = -1;
    mEmuPickFilter.clear();
    loadEmuCatalog();

    std::vector<Ps3Item> parentSnap = mPs3Stack.empty() ? std::vector<Ps3Item>() : mPs3Stack.back().items;
    int parentSel = mPs3Stack.empty() ? 0 : mPs3Stack.back().sel;
    Ps3Level lvl;
    buildEmulatorPicker(lvl);
    mPs3Stack.push_back(lvl);
    // Collapse-rail slide-in, matching every other submenu push.
    mPs3SubParentItems = std::move(parentSnap);
    mPs3SubParentIdx   = parentSel;
    mPs3SubChildItems  = mPs3Stack.back().items;
    mPs3SubDir         = 1;
    mPs3SubAnimStart   = mEffectTime;
    mPs3SubAnim        = 0.0f;
    mPs3AnimItem       = 0.0f;
    mPs3ItemAnimStart  = -1.0f;
}

// Create a fresh system, slug deduped against existing ids. preferredId (the
// Daijishou platform uniqueId, e.g. "ps2") becomes the rom-folder id when given,
// so the default scan paths land on ROMs/<platformId>. Returns its index.
int nano_makeUniqueSystem(std::vector<NanoMenu::XmbSystem>& systems, const std::string& name,
                          const std::string& preferredId) {
    NanoMenu::XmbSystem s;
    s.builtin = false; s.enabled = true;
    s.name = name.empty() ? "New System" : name;
    s.shortname = shortFromName(s.name);
    std::string id = preferredId.empty() ? slugify(s.name) : slugify(preferredId);
    std::string uid = id; int k = 2;
    auto exists = [&](const std::string& q) {
        for (auto& x : systems) if (x.id == q) return true; return false;
    };
    while (exists(uid)) { uid = id + "_" + std::to_string(k++); }
    s.id = uid; s.romDir = uid;
    int maxOrd = -1; for (auto& x : systems) if (x.order > maxOrd) maxOrd = x.order;
    s.order = maxOrd + 1;
    s.launchType = NanoMenu::XLT_LIBRETRO_CORE;
    s.iconR = s.iconG = s.iconB = 1.0f;
    systems.push_back(std::move(s));
    return (int)systems.size() - 1;
}

// Create a blank custom system and open its editor (the "Custom..." path of the
// add flow). The emulator picker was already popped by the dispatch.
void NanoMenu::gsAddBlankSystem() {
    int idx = nano_makeUniqueSystem(mXmbSystems, "New System", "");
    mGsEditIdx = idx;
    mGsAddMode = false;
    saveSystemsConfig();
    gsRefreshStackLevels();
    Ps3Level lvl; buildGameSystemEditor(idx, lvl); mPs3Stack.push_back(lvl);
    buildPs3Cats();
}

void NanoMenu::gsRemoveSystem(int sysIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    XmbSystem& s = mXmbSystems[sysIdx];
    if (s.builtin) return;   // built-ins can only be disabled, never removed
    ALOGI("ps3menu: removing custom system %s", s.id.c_str());
    unlink(xmbCachePath(s).c_str());
    std::string userIcon = "/data/system/nano_user_icons/" + s.id + ".png";
    unlink(userIcon.c_str());
    mXmbSystems.erase(mXmbSystems.begin() + sysIdx);
    for (size_t k = 0; k < mXmbSystems.size(); k++) mXmbSystems[k].order = (int)k;
    mGsEditIdx = -1;
    // If we were removing from this system's editor, pop back to the list.
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_EDITOR) mPs3Stack.pop_back();
    saveSystemsConfig();
    gsRefreshStackLevels();
    buildPs3Cats();
}

void NanoMenu::gsOpenRemoveConfirm(int sysIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    if (mXmbSystems[sysIdx].builtin) return;   // no remove for built-ins
    mGsEditIdx = sysIdx;   // case 23 reads mGsEditIdx
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 1; mPs3DlgThemeKey = 23; mPs3DlgTitle = "Remove System"; mPs3DlgBody.clear();
    mPs3DlgOptions.push_back("Cancel");          mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Remove System");   mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(22);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgOrigSel = 0;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgBlurValid = false;
}

// Confirm before dropping a scan-source folder (Cancel / Remove Folder), so removing an
// accidentally-added ROM source is a clear, deliberate action instead of a silent one-press
// delete. The folder path is shown in the body. case 45 removes on commit.
void NanoMenu::gsOpenRemoveScanSourceConfirm(int srcIdx) {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size()) return;
    const XmbSystem& s = mXmbSystems[mGsEditIdx];
    if (srcIdx < 0 || srcIdx >= (int)s.scanSources.size()) return;
    mGsRemoveSrcIdx = srcIdx;   // case 45 reads this
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 1; mPs3DlgThemeKey = 45; mPs3DlgTitle = "Remove Folder";
    mPs3DlgBody = s.scanSources[srcIdx].value;
    mPs3DlgOptions.push_back("Cancel");         mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Remove Folder");  mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(22);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgBlurValid = false;
}

// What Y does on the focused row of the settings screen on top (the BTN_WEST handler in
// NanoMenuInput.cpp), for its button prompt; null where Y does nothing there.
const char* NanoMenu::menuYAction() const {
    if (!mPs3Xmb || mPs3Stack.empty() || ndsInModal()) return nullptr;
    const Ps3Level& lvl = mPs3Stack.back();
    if (lvl.sel < 0 || lvl.sel >= (int)lvl.items.size()) return nullptr;
    const Ps3Item& it = lvl.items[lvl.sel];
    switch (lvl.screenKind) {
        case GS_LIST:
            return it.kind == PS3_GS_SYSTEM_ROW && it.a >= 0 && it.a < (int)mXmbSystems.size() &&
                           !mXmbSystems[it.a].builtin
                       ? "Remove"
                       : nullptr;
        case GS_FOLDER:
            if (it.kind == PS3_GS_SCANSRC || it.kind == PS3_GS_DEFFOLDER) return "Remove";
            return it.kind == PS3_GS_DEFFOLDER_OFF ? "Restore" : nullptr;
        case MUSIC_FOLDER: return it.kind == PS3_MUSIC_FOLDER_ROW ? "Remove" : nullptr;
        case PHOTO_FOLDER: return it.kind == PS3_PHOTO_FOLDER_ROW ? "Remove" : nullptr;
        case VIDEO_FOLDER: return it.kind == PS3_VIDEO_FOLDER_ROW ? "Remove" : nullptr;
        default: return nullptr;
    }
}

// The XMB theme's prompt for menuYAction: the Y glyph and its label on a dark pill, centred at the
// foot of the visible frame, over the list rows there. Not a touch target: rows under it keep their
// taps.
void NanoMenu::drawXmbYPrompt() {
    const char* label = xmbHomeTouch() ? menuYAction() : nullptr;
    if (!label) return;
    const float ap = mPs3ChromeLabelReveal * pspClockTextFade();
    if (ap <= 0.01f) return;
    float ui = mPs3UiScale; if (ui < 0.5f) ui = 0.5f; if (ui > 2.0f) ui = 2.0f;
    const float S = ps3::gScale / ui;
    label = trDyn(label);
    const float sb = S * ps3DlgFontBoost();
    const float fg = mUserFontScale > 0.05f ? mUserFontScale : 1.0f;   // drawText scales the text by it
    const float fs = sb * 22.0f / 16.0f, glyphR = sb * 12.0f, gap = sb * 12.0f, pad = sb * 14.0f;
    const float lw = fmaxf(sb * 2.0f, 1.5f), tw = measureText(label, fs);
    const float textH = 16.0f * fs * fg;
    const float groupW = glyphR * 2.0f + gap + tw, h = fmaxf(glyphR * 2.0f, textH) + pad;
    const float left = ps3::gFrameX + ps3::gFrameW * 0.5f - groupW * 0.5f;
    const float y = ps3::gFrameY + ps3::gFrameH - h * 0.5f - sb * 18.0f;
    drawRoundedRect(left - pad, y - h * 0.5f, groupW + pad * 2.0f, h, h * 0.5f, 0.0f, 0.0f, 0.0f, 0.55f * ap);
    refreshFaceButtonPrefs();
    drawFaceGlyph(2, left + glyphR, y, glyphR, lw, ap);
    drawText(label, left + glyphR * 2.0f + gap, y - 0.45f * textH, fs, 1.0f, 1.0f, 1.0f, 0.95f * ap);
}

// ---- Native folder picker ----

void NanoMenu::buildScanFoldersScreen(Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.title = "Scan Folders"; out.screenKind = GS_FOLDER;
    { Ps3Item it; it.label = "Add Folder..."; it.kind = PS3_GS_ADDFOLDER;
      it.iconTex = iconTexForIcon(50); it.nmapTex = nmapForIcon(50);   // folder+ glyph
      it.iconR = it.iconG = it.iconB = 1.0f; out.items.push_back(it); }
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size()) return;
    const XmbSystem& sys = mXmbSystems[mGsEditIdx];
    if (nanoscummvm::listsGames(sys.romDir, sys.launchPkg)) {   // its games are ScummVM's own list: no folders
        out.items.clear();
        for (const char* line : {"Games come from ScummVM's game list",
                                 "Add them in ScummVM: Add Game, or Mass Add for a whole folder"}) {
            Ps3Item it; it.label = trDyn(line);
            it.kind = PS3_GS_FIELD; it.a = -1;
            it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.72f;
            out.items.push_back(it);
        }
        return;
    }

    // User-added scan folders (Y removes). When empty, the system uses the
    // default ROMs/<romDir> candidates across internal + every SD/USB mount.
    for (size_t i = 0; i < sys.scanSources.size(); i++) {
        Ps3Item it; it.label = sys.scanSources[i].value;
        if (sys.scanSources[i].subfolders != 0) it.value = "Subfolders";
        it.kind = PS3_GS_SCANSRC; it.a = (int)i;
        it.iconTex = iconTexForIcon(62); it.nmapTex = nmapForIcon(62);   // folder glyph
        it.iconR = it.iconG = it.iconB = 1.0f; out.items.push_back(it);
    }

    // Normalise a path to one stable identity for de-duping AND display. /storage/emulated/0,
    // /data/media/0 and /sdcard are three separate mount VIEWS of the same internal storage
    // (realpath does NOT collapse the two FUSE mounts), so fold them all to the familiar
    // /storage/emulated/0 form users recognise; realpath first resolves the /sdcard symlink and
    // any case variants of an existing dir.
    auto norm = [](const std::string& p) -> std::string {
        char rp[4096]; std::string q = realpath(p.c_str(), rp) ? std::string(rp) : p;
        if (q.rfind("/data/media/0/", 0) == 0)      q = "/storage/emulated/0/" + q.substr(14);
        else if (q.rfind("/sdcard/", 0) == 0)       q = "/storage/emulated/0/" + q.substr(8);
        else if (q == "/data/media/0")              q = "/storage/emulated/0";
        return q;
    };

    // Read-only: the DEFAULT folders (built-in ROMs/<romDir> candidates across internal +
    // SD/USB) that are ALREADY being scanned, so the user can see which default locations
    // are in use and where to drop ROMs - even before any are found. Each is marked
    // "Default" (with the game count when populated), inert (not removable; the user removes
    // the folder itself or adds their own above). De-duped by normalised path against the
    // user-added folders and each other. (Requested: show which default folders are in use.)
    std::vector<std::string> shownNorm;
    for (const auto& src : sys.scanSources) shownNorm.push_back(norm(src.value));
    // Count ROMs found under each active scan path, keyed by normalised dir.
    std::vector<std::pair<std::string, int>> activeCount;   // (normalised activePath, count)
    for (const auto& ap : sys.activePaths) {
        int cnt = 0;
        for (const auto& r : sys.roms) {
            size_t sl = r.rfind('/');
            if (sl != std::string::npos && r.compare(0, sl, ap) == 0) cnt++;
        }
        activeCount.push_back({norm(ap), cnt});
    }
    int defaultShown = 0;
    std::vector<std::string> cands = buildScanCandidates(sys);
    for (const auto& cp : cands) {
        struct stat st;
        if (stat(cp.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;   // only real, existing dirs
        std::string c = norm(cp);
        bool dup = false;
        for (const auto& s : shownNorm) if (s == c) { dup = true; break; }
        if (dup) continue;
        shownNorm.push_back(c);
        int cnt = 0;
        for (const auto& ac : activeCount) if (ac.first == c) { cnt = ac.second; break; }
        Ps3Item it; it.label = c; it.kind = PS3_GS_DEFFOLDER; it.a = -1;   // default folder; Y removes it
        // Alias = last path component (ROMs/<alias> or /<alias>), lowercased -- the key stored in
        // disabledDefaultFolders so removing here drops that alias from every mount it maps to.
        { size_t sl = c.rfind('/'); std::string al = (sl == std::string::npos) ? c : c.substr(sl + 1);
          for (char& ch : al) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch); it.payloadStr = al; }
        char v[96];
        if (cnt > 0) snprintf(v, sizeof(v), trDyn("Default, %d"), cnt);   // "Default, 12"
        else         snprintf(v, sizeof(v), "%s", trDyn("Default (empty)"));
        it.value = v;
        it.iconTex = iconTexForIcon(62); it.nmapTex = nmapForIcon(62);
        float m = cnt > 0 ? 0.72f : 0.5f;   // brighter when populated, dimmer when empty
        it.iconR = it.iconG = it.iconB = m;
        out.items.push_back(it);
        defaultShown++;
    }

    // Removed default folders (Y restores). Listed straight from disabledDefaultFolders because a
    // removed alias is no longer in buildScanCandidates above, so it would otherwise vanish with no
    // way to bring it back.
    for (const auto& alias : sys.disabledDefaultFolders) {
        Ps3Item it; it.label = std::string("ROMs/") + alias;
        it.kind = PS3_GS_DEFFOLDER_OFF; it.a = -1; it.payloadStr = alias;
        it.value = trDyn("Removed");
        it.iconTex = iconTexForIcon(62); it.nmapTex = nmapForIcon(62);
        it.iconR = it.iconG = it.iconB = 0.42f;   // dim: not scanned
        out.items.push_back(it);
    }

    // Nothing at all to scan (no user folders, and not even a default dir exists yet):
    // point the user at the default location so they know where to create it.
    if (sys.scanSources.empty() && defaultShown == 0) {
        char nl[256]; snprintf(nl, sizeof(nl), trDyn("No ROMs found in ROMs/%s"), sys.romDir.c_str());
        Ps3Item it; it.label = nl;
        it.kind = PS3_GS_FIELD; it.a = -1;
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.55f;
        out.items.push_back(it);
    }
}

void NanoMenu::gsOpenScanFolders() {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size()) return;
    mFolderPickTarget = 0;   // route the folder browser's "Select" to the Game system

    std::vector<Ps3Item> parentSnap = mPs3Stack.empty() ? std::vector<Ps3Item>() : mPs3Stack.back().items;
    int parentSel = mPs3Stack.empty() ? 0 : mPs3Stack.back().sel;
    Ps3Level lvl; buildScanFoldersScreen(lvl);
    mPs3Stack.push_back(lvl);
    mPs3SubParentItems = std::move(parentSnap);
    mPs3SubParentIdx = parentSel; mPs3SubChildItems = mPs3Stack.back().items;
    mPs3SubDir = 1; mPs3SubAnimStart = mEffectTime; mPs3SubAnim = 0.0f;
    mPs3AnimItem = 0.0f; mPs3ItemAnimStart = -1.0f;
}

void NanoMenu::gsRemoveScanSource(int srcIdx) {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size()) return;
    XmbSystem& s = mXmbSystems[mGsEditIdx];
    if (srcIdx < 0 || srcIdx >= (int)s.scanSources.size()) return;
    s.scanSources.erase(s.scanSources.begin() + srcIdx);
    invalidateSystemScan(s);
    forceRescanAllSystems();
    saveSystemsConfig();
    gsRefreshStackLevels();
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDER)
        buildScanFoldersScreen(mPs3Stack.back());
    buildPs3Cats();
}

// Re-derive + persist after a scan-folder change, then rebuild the open screen. Shared by the
// default-folder remove/restore below (mirrors gsRemoveScanSource's refresh tail).
void NanoMenu::gsAfterScanFolderChange(XmbSystem& s) {
    invalidateSystemScan(s);
    forceRescanAllSystems();
    saveSystemsConfig();
    gsRefreshStackLevels();
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDER)
        buildScanFoldersScreen(mPs3Stack.back());
    buildPs3Cats();
}

// Remove a default (built-in alias) scan folder from the edited system so it is no longer scanned.
void NanoMenu::gsDisableDefaultFolder(const std::string& alias) {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size() || alias.empty()) return;
    XmbSystem& s = mXmbSystems[mGsEditIdx];
    std::string la; for (char c : alias) la += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    for (const auto& d : s.disabledDefaultFolders) if (d == la) return;   // already removed
    s.disabledDefaultFolders.push_back(la);
    gsAfterScanFolderChange(s);
}

// Restore a previously-removed default scan folder.
void NanoMenu::gsEnableDefaultFolder(const std::string& alias) {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size() || alias.empty()) return;
    XmbSystem& s = mXmbSystems[mGsEditIdx];
    std::string la; for (char c : alias) la += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    for (size_t i = 0; i < s.disabledDefaultFolders.size(); i++) {
        if (s.disabledDefaultFolders[i] == la) { s.disabledDefaultFolders.erase(s.disabledDefaultFolders.begin() + i); break; }
    }
    gsAfterScanFolderChange(s);
}

// Names of the network shares that are actually mounted right now.
//
// This reads the kernel's mount table rather than looking in /mnt/shares, for two reasons: the
// share daemon creates its mount point before it connects, so an empty directory there does not
// mean a usable share, and any stat of a mount point whose server has gone away can block until
// FUSE times out. Reading /proc/self/mountinfo never touches the network.
std::vector<std::string> NanoMenu::mountedShareNames() const {
    std::vector<std::string> out;
    FILE* f = fopen("/proc/self/mountinfo", "re");
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Field 5 is the mount point; it is the field after the "major:minor root" pair.
        int n = 0;
        const char* p = line;
        const char* mp = nullptr;
        while (*p) {
            if (n == 4) { mp = p; break; }
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            n++;
        }
        if (!mp) continue;
        const char* end = strchr(mp, ' ');
        if (!end) continue;
        std::string path(mp, end - mp);
        if (path.compare(0, 12, "/mnt/shares/") != 0) continue;
        std::string name = path.substr(12);
        // Only the share's own mount point, not anything nested below it.
        if (name.empty() || name.find('/') != std::string::npos) continue;
        // mountinfo escapes spaces and friends as octal; undo that so the name matches the
        // directory and the label reads properly.
        std::string unesc;
        for (size_t i = 0; i < name.size(); i++) {
            if (name[i] == '\\' && i + 3 < name.size()) {
                unesc += (char)strtol(name.substr(i + 1, 3).c_str(), nullptr, 8);
                i += 3;
            } else {
                unesc += name[i];
            }
        }
        out.push_back(unesc);
    }
    fclose(f);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Is this path one of the storage roots (its parent should be the roots list)?
static bool isStorageRoot(const std::string& p) {
    if (p == "/storage/emulated/0") return true;
    if (p.compare(0, 9, "/storage/") == 0 && p.find('/', 9) == std::string::npos) return true;
    if (p.compare(0, 14, "/mnt/media_rw/") == 0 && p.find('/', 14) == std::string::npos) return true;
    // A mounted network share is a root of its own: there is nothing above it to browse.
    if (p.compare(0, 12, "/mnt/shares/") == 0 && p.find('/', 12) == std::string::npos) return true;
    return false;
}

// A folder as the browser names it: a storage root as in the roots list, else its own name.
static std::string folderBrowserName(const std::string& p) {
    if (p.empty()) return trDyn("Storage");
    if (p == "/storage/emulated/0") return trDyn("Internal storage");
    if (isStorageRoot(p)) {
        const size_t sl = p.find_last_of('/');
        const char* fmt = p.compare(0, 12, "/mnt/shares/") == 0 ? trDyn("Share: %s")
                          : p.compare(0, 14, "/mnt/media_rw/") == 0 ? trDyn("Removable: %s") : trDyn("SD: %s");
        return folderFmt(fmt, p.substr(sl + 1));
    }
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

void NanoMenu::buildFolderBrowser(const std::string& path, Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.screenKind = GS_FOLDERBROWSE;
    mGsFolderPath = path;
    GLuint folderNmap = nmapForIcon(62);
    auto addDir = [&](const std::string& label, const std::string& target) {
        Ps3Item it; it.label = label; it.kind = PS3_GS_DIR; it.payloadStr = target;
        it.iconTex = iconTexForIcon(62); it.nmapTex = folderNmap; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    };
    if (path.empty()) {
        // Storage roots.
        out.title = "Storage";
        // #90 DraStic data-folder picker (target 5): offer a reset-to-default at the top of the roots,
        // since the roots pseudo-folder has no "Select This Folder". Selecting it clears the override.
        // Same for the Boxart Folder picker (target 7): "Use Default Folder" moves the art back to internal.
        if (mFolderPickTarget == 5 || mFolderPickTarget == 7 || mFolderPickTarget == 8 || mFolderPickTarget == 10) {
            Ps3Item it; it.label = "Use Default Folder"; it.kind = PS3_GS_SELFOLDER; it.payloadStr = "@default";
            it.iconTex = iconTexForIcon(22); it.nmapTex = nmapForIcon(22); it.iconR = it.iconG = it.iconB = 1.0f;
            out.items.push_back(it);
        }
        addDir("Internal storage", "/storage/emulated/0");
        // Network shares are bind-mounted into /storage so apps can open them by path, which means
        // they turn up in this scan too and would otherwise be listed twice: once here as "SD: name"
        // and again below as "Share: name". Skip them here and let the share pass label them, since
        // calling a NAS an SD card is worse than the duplication.
        const std::vector<std::string> shareNames = mountedShareNames();
        auto isShare = [&](const char* n) {
            return std::find(shareNames.begin(), shareNames.end(), std::string(n)) != shareNames.end();
        };
        DIR* d = opendir("/storage");
        if (d) { struct dirent* e; while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            if (!strcmp(e->d_name, "emulated") || !strcmp(e->d_name, "self")) continue;
            if (isShare(e->d_name)) continue;
            addDir(folderFmt(trDyn("SD: %s"), e->d_name), std::string("/storage/") + e->d_name);
        } closedir(d); }
        d = opendir("/mnt/media_rw");
        if (d) { struct dirent* e; while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            addDir(folderFmt(trDyn("Removable: %s"), e->d_name), std::string("/mnt/media_rw/") + e->d_name);
        } closedir(d); }
        // Mounted network shares, so a media folder can live on a NAS. Listing them here is what
        // puts them behind "Search for Media Servers" in Photos/Music/Video as well, since all of
        // those open this same browser.
        for (const std::string& s : shareNames)
            addDir(folderFmt(trDyn("Share: %s"), s), std::string("/mnt/shares/") + s);
        return;
    }
    out.title = path;
    // First the parent (".. <its name>"; a storage root goes back to the roots list, ""), then
    // "Select This Folder", then the subfolders.
    std::string parent;
    if (!isStorageRoot(path)) { size_t sl = path.rfind('/'); parent = sl == std::string::npos ? "" : path.substr(0, sl); }
    addDir(".. " + folderBrowserName(parent), parent);
    // Icon-import picker (target 9) also lists selectable image files, so the user can browse to
    // their own PNG; it selects a file, so it has no "Select This Folder". It used to share target
    // 6 with the Syncthing folder picker, which therefore had no such row: a Syncthing folder could
    // not be chosen at all (reported 2026-09-28).
    const bool iconPick = (mFolderPickTarget == 9 || mFolderPickTarget == 12);   // 12: Set Boxart
    // "Select This Folder" - only when picking a folder. The icon-import picker selects a file
    // instead, so it has no whole-folder action.
    if (!iconPick) {
        Ps3Item it; it.label = "Select This Folder"; it.kind = PS3_GS_SELFOLDER; it.payloadStr = path;
        it.iconTex = iconTexForIcon(22); it.nmapTex = nmapForIcon(22); it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }

    // Subdirectories, from the worker rather than from a blocking opendir+stat here.
    //
    // This is the screen a user picks a media folder with, so it is routinely pointed at a network
    // share. Listing one inline cost a round trip per entry with the render thread held throughout,
    // which is what let the watchdog abort nano when a folder on an FTP share was opened. The
    // listing is requested once and cached; until it arrives the screen shows "Loading..." and
    // stays fully responsive, so Back still works if the server never answers.
    auto isImageName = [](const std::string& n) {
        size_t dot = n.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = n.substr(dot + 1);
        return strcasecmp(ext.c_str(), "png") == 0 || strcasecmp(ext.c_str(), "jpg") == 0 ||
               strcasecmp(ext.c_str(), "jpeg") == 0;
    };
    GLuint pngNmap = nmapForIcon(25);   // document/page glyph for a file row
    if (mFbCacheValid && mFbCachePath == path) {
        for (const auto& e : mFbCacheEntries)
            if (e.isDir) addDir(e.name, path + "/" + e.name);
        if (iconPick) {
            for (const auto& e : mFbCacheEntries) {
                if (e.isDir || !isImageName(e.name)) continue;
                Ps3Item it; it.label = e.name; it.kind = PS3_GS_PICKFILE; it.payloadStr = path + "/" + e.name;
                it.iconTex = iconTexForIcon(25); it.nmapTex = pngNmap; it.iconR = it.iconG = it.iconB = 1.0f;
                out.items.push_back(it);
            }
        }
    } else {
        fbRequestListing(path);
        Ps3Item it; it.kind = PS3_DATA_LEAF; it.label = trDyn("Loading...");
        it.desc = "Reading the folder.";
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 0.55f;
        out.items.push_back(it);
    }
}

// Queue a directory listing for the folder browser. Render thread; never blocks.
void NanoMenu::fbRequestListing(const std::string& path) {
    if (!mFbStarted) {
        mFbStarted = true;
        mFbThread = std::thread([this]() {
            for (;;) {
                std::string p;
                {
                    std::unique_lock<std::mutex> lk(mFbLock);
                    mFbCv.wait(lk, [this]{
                        return mFbQuit.load(std::memory_order_relaxed) || !mFbQueue.empty();
                    });
                    if (mFbQuit.load(std::memory_order_relaxed)) return;
                    p = mFbQueue.front();
                    mFbQueue.pop_front();
                }
                FbResult r;
                r.path = p;
                if (DIR* d = opendir(p.c_str())) {
                    while (struct dirent* e = readdir(d)) {
                        if (e->d_name[0] == '.') continue;
                        const std::string child = p + "/" + e->d_name;
                        struct stat st;
                        if (stat(child.c_str(), &st) != 0) continue;
                        FbEntry fe;
                        fe.name = e->d_name;
                        fe.isDir = S_ISDIR(st.st_mode);
                        fe.size = (long long)st.st_size;
                        // Directories and regular files only: a socket or a device node in a share
                        // is not something either browser can do anything with.
                        if (fe.isDir || S_ISREG(st.st_mode)) r.entries.push_back(std::move(fe));
                    }
                    closedir(d);
                    r.ok = true;
                }
                // Directories first, then files, each case-insensitively by name - the order both
                // browsers present.
                std::sort(r.entries.begin(), r.entries.end(),
                          [](const FbEntry& a, const FbEntry& b) {
                              if (a.isDir != b.isDir) return a.isDir;
                              return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
                          });
                {
                    std::lock_guard<std::mutex> lk(mFbLock);
                    mFbDone.push_back(std::move(r));
                }
                FrameFlag::wake();   // fbTick adopts it on the next frame
            }
        });
    }
    {
        std::lock_guard<std::mutex> lk(mFbLock);
        if (!mFbPending.insert(path).second) return;   // already queued or in flight
        mFbQueue.push_back(path);
    }
    mFbCv.notify_one();
}

// Render thread: adopt a finished listing and rebuild the screen showing it.
void NanoMenu::fbTick() {
    std::vector<FbResult> done;
    {
        std::lock_guard<std::mutex> lk(mFbLock);
        if (mFbDone.empty()) return;
        done.swap(mFbDone);
        for (const auto& r : done) mFbPending.erase(r.path);
    }
    for (auto& r : done) {
        // Cache even a failed listing, so an unreachable folder shows as empty instead of
        // re-requesting forever; leaving the screen and coming back retries it.
        mFbCachePath = r.path;
        mFbCacheEntries = std::move(r.entries);
        mFbCacheValid = true;
        // Only the screen currently showing this path needs rebuilding.
        const int kind = mPs3Stack.empty() ? -1 : mPs3Stack.back().screenKind;
        const bool isFolder = (kind == GS_FOLDERBROWSE && mGsFolderPath == r.path);
        const bool isFile   = (kind == FE_BROWSE      && mFeBrowsePath  == r.path);
        if (isFolder || isFile) {
            int keep = mPs3Stack.back().sel;
            if (isFolder) buildFolderBrowser(r.path, mPs3Stack.back());
            else          buildFileBrowser(r.path, mPs3Stack.back());
            int n = (int)mPs3Stack.back().items.size();
            if (keep >= n) keep = n - 1;
            mPs3Stack.back().sel = keep < 0 ? 0 : keep;
            mDisplayDirty = true;
        }
    }
}

void NanoMenu::fbStopWorker() {
    if (!mFbStarted) return;
    mFbQuit.store(true, std::memory_order_relaxed);
    mFbCv.notify_all();
    if (mFbThread.joinable()) mFbThread.join();
    mFbStarted = false;
}

// A folder chosen for the system: ask whether its subfolders are scanned too (case 61 adds it).
void NanoMenu::gsFolderSelect(const std::string& path) {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size() || path.empty()) return;
    const XmbSystem& s = mXmbSystems[mGsEditIdx];
    // Skip duplicates.
    for (const auto& src : s.scanSources) if (src.value == path) {
        if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDERBROWSE) mPs3Stack.pop_back();
        return;
    }
    mGsAddFolderPath = path;
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 1; mPs3DlgThemeKey = 61; mPs3DlgTitle = "Add Folder";
    mPs3DlgBody = path;
    mPs3DlgOptions.push_back("This Folder Only");    mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Include Subfolders");  mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 1;
    mPs3DlgOrigSel = mPs3DlgSel;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(50);   // folder+ glyph
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgBlurValid = false;
}

void NanoMenu::gsFolderAdd(const std::string& path, bool subfolders) {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size() || path.empty()) return;
    XmbSystem& s = mXmbSystems[mGsEditIdx];
    ScanSource src; src.type = 0; src.value = path; src.subfolders = subfolders ? 1 : 0;
    s.scanSources.push_back(src);
    ALOGI("ps3menu: added scan folder %s%s to %s", path.c_str(),
          subfolders ? " (with subfolders)" : "", s.id.c_str());
    invalidateSystemScan(s);
    forceRescanAllSystems();
    saveSystemsConfig();
    // Pop the browser, refresh the scan-folders screen + the Game category.
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDERBROWSE) mPs3Stack.pop_back();
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDER)
        buildScanFoldersScreen(mPs3Stack.back());
    gsRefreshStackLevels();
    buildPs3Cats();
}

// The DraStic USER data folder (saves, save states, shader overrides). Writes
// persist.gammaos.drastic.data_dir, which drastic-nano reads at each DS launch: its private root
// (BIOS, config, cheats) never moves and is always seeded, but its backup/ and savestates/ links and
// its shader merge point into this folder (drastic_assets::userDir). Any browsable folder is allowed
// (internal storage, SD card, removable media, a network share); the folder is created if missing
// and must be writable. Empty / "@default" (the "Use Default Folder" row) resets to
// /sdcard/drastic-nano. Existing files are not moved. Pops the browser back to Game Settings.
void NanoMenu::drasticDataFolderSelect(const std::string& path) {
    drasticFolderSelect("persist.gammaos.drastic.data_dir", "DraStic data folder", path);
}

// The DraStic cheats folder (the user's usrcheat.dat files), persist.gammaos.drastic.cheats_dir.
// Default is <data folder>/cheats. Same rules as the data folder; folder-picker target 8.
void NanoMenu::drasticCheatsFolderSelect(const std::string& path) {
    drasticFolderSelect("persist.gammaos.drastic.cheats_dir", "DraStic cheats folder", path);
}

static void nanoMkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if ((i == dir.size() || dir[i] == '/') && !nanoscraper::isVolumeRoot(dir.substr(0, i)))
            ::mkdir(dir.substr(0, i).c_str(), 0775);
}

// The ES-DE theme's game media folder (ES-DE's MediaDirectory), persist.gammaos.nano.esde.mediadir:
// the folder holding the per-system media folders, read before the default
// /sdcard/ES-DE/downloaded_media. Folder-picker target 10; "@default" clears it. When there is
// media in the current folder, asks whether to copy it to the new one first (theme key 60).
void NanoMenu::esdeMediaFolderSelect(const std::string& path) {
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDERBROWSE) mPs3Stack.pop_back();
    mDisplayDirty = true;
    if (esdeStoreMode()) {   // the ES-DE folder on the card holds the media
        photoShowBanner(trDyn("Game media is kept in the ES-DE folder on the microSD card."));
        return;
    }
    std::string p = (path == "@default") ? std::string() : path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    struct stat st = {};
    if (!p.empty() && (stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || access(p.c_str(), R_OK) != 0)) {
        photoShowBanner(trDyn("ES-DE media folder: not readable"));
        ALOGW("ps3menu: ES-DE media folder %s rejected: %s", p.c_str(), strerror(errno));
        return;
    }
    char cur[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.esde.mediadir", cur, "");
    const std::string from = cur[0] ? std::string(cur) : std::string("/storage/emulated/0/ES-DE/downloaded_media");
    const std::string to = p.empty() ? std::string("/storage/emulated/0/ES-DE/downloaded_media") : p;
    bool hasMedia = false;
    if (from != to) nanoMkdirs(to);
    if (from != to && access(to.c_str(), W_OK) == 0) {
        if (DIR* d = opendir(from.c_str())) {
            while (dirent* e = readdir(d)) if (e->d_name[0] != '.') { hasMedia = true; break; }
            closedir(d);
        }
    }
    if (!hasMedia) { esdeMediaFolderApply(p); return; }
    mPendingEsdeMediaDir = p.empty() ? std::string("@default") : p;
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 1; mPs3DlgThemeKey = 60; mPs3DlgTitle = "ES-DE Media Folder";
    mPs3DlgBody = "Copy your game media (covers, screenshots, marquees, videos and manuals) to the new folder? "
                  "Files already in the new folder are kept, and nothing is deleted from the old one.";
    mPs3DlgOptions.push_back("Don't Copy");  mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Copy Media");  mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 1; mPs3DlgOrigSel = 1;
    mPs3DlgBinding = nullptr; mPs3DlgAppInfo = false; mPs3DlgRomInfo = false;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(62);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
}

void NanoMenu::esdeMediaFolderApply(const std::string& p) {
    property_set("persist.gammaos.nano.esde.mediadir", p.c_str());
    mEsdeMediaPath.clear();   // re-resolve every game's media from the new folder
    mEsdeLoadedSysIdx = -1;   // and the focused system's noMedia/noVideos variant with it
    photoShowBanner(p.empty() ? trDyn("ES-DE media folder: Default") : trDyn("ES-DE media folder set"));
    ALOGI("ps3menu: ES-DE media folder -> %s", p.empty() ? "default" : p.c_str());
    mDisplayDirty = true;
}

// ---------------------------------------------------------------------------
// Folder copy: every file under each source copied to the same place under its destination,
// skipping files that already exist there (nothing is overwritten or deleted). Runs on a worker
// behind the scraper's progress modal; B cancels between files.
// ---------------------------------------------------------------------------

bool NanoMenu::fileCopyStart(int kind, const char* title, std::vector<FileCopyPair> pairs) {
    if (mScrapeRunning || !mScrapeMoveDst.empty() || mFileJobKind != 0) {
        sdMoveNotice(trDyn("Another copy or scrape is running"));   // setup: a line on its page
        return false;
    }
    // Compared as the same spelling (the raw mount, links resolved): /storage/X and /mnt/media_rw/X
    // are one folder. A move never copies a folder onto or into itself, which would delete the files.
    std::vector<FileCopyPair> kept;
    for (auto& pr : pairs) {
        const std::string cs = sdCanon(pr.src), cd = sdCanon(pr.dst);
        if (pr.move) {
            if (cs == cd) { ALOGI("ps3menu: move: %s is already there", pr.src.c_str()); continue; }
            if (sdUnder(cd, cs) || sdUnder(cs, cd)) {
                sdMoveNotice(trDyn("A folder to move is inside where it is going. Nothing was moved."));
                ALOGW("ps3menu: move: %s and %s overlap", pr.src.c_str(), pr.dst.c_str());
                return false;
            }
        } else if (cs != cd && sdUnder(cd, cs)) {   // into itself: never ends
            sdMoveNotice(trDyn("Pick a folder outside the current one"));
            return false;
        }
        kept.push_back(std::move(pr));
    }
    pairs = std::move(kept);
    mFileJobKind = kind;
    mFileJobCommitted = false;
    mFileJobIncomplete = false;
    mScrapeJobTitle = title;
    mScrapeCopyMode = true;
    mScrapeMoveMode = false;
    mScrapeProgActive = true;
    mScrapeDoneFlag = false;
    mScrapeCancel = false;
    mScrapeError.clear();
    mScrapeStatus = trDyn("Counting files...");
    mScrapeDone = 0; mScrapeHits = 0; mScrapeFail = 0; mScrapeSkip = 0;
    mScrapeTotal = 0;
    mScrapeRunning = true;
    std::thread(&NanoMenu::fileCopyThreadFunc, this, std::move(pairs)).detach();
    mDisplayDirty = true;
    return true;
}

void NanoMenu::fileCopyThreadFunc(std::vector<FileCopyPair> pairs) {
    nanoThreadNormalPriority();
    std::vector<std::string> stopPkgs;
    stopPkgs.swap(mFileJobStopPkgs);   // this job's only
    nanofilejob::Hooks h;
    h.cancelled = [this]() { std::lock_guard<std::mutex> lk(mScrapeMutex); return (bool)mScrapeCancel; };
    h.progress = [this](const nanofilejob::Counters& c) {
        std::lock_guard<std::mutex> lk(mScrapeMutex);
        mScrapeTotal = c.total; mScrapeDone = c.done; mScrapeHits = c.hits; mScrapeSkip = c.skip; mScrapeFail = c.fail;
        mScrapeError = c.error;
        if (!c.status.empty()) mScrapeStatus = c.status;
    };
    // The programs whose files move are closed first (they write their settings when they close).
    h.stopPrograms = [this, &stopPkgs]() {
        const bool stopRa = std::find(stopPkgs.begin(), stopPkgs.end(), "com.retroarch.aarch64") != stopPkgs.end();
        for (const auto& pkg : stopPkgs) {
            if (pkg != "com.retroarch.aarch64") sdStopIfRunning(pkg);
        }
        if (stopRa && isRetroArchRunning()) {
            // RetroArch rewrites its config on exit: ask it to save and quit first (as a power-off
            // does), and stop it only if it does not.
            property_set("sys.gammaos.nano.drop_input", "0");
            property_set("sys.gammaos.nano.overlay_esc", "1");
            for (int i = 0; i < 50 && isRetroArchRunning(); i++) {
                if (i == 10 || i == 25) property_set("sys.gammaos.nano.overlay_esc", "1");
                usleep(100000);
            }
            if (isRetroArchRunning()) system("am force-stop com.retroarch.aarch64 >/dev/null 2>&1");
        }
        return !stopPkgs.empty();
    };
    h.committed = [this]() { std::lock_guard<std::mutex> lk(mScrapeMutex); mFileJobCommitted = true; };
    h.relocate = [this](int group) { return fileJobRelocate(group); };
    h.restorecon = [](const std::string& path) {
        system(("restorecon " + nanolaunch::shellQuote(path) + " >/dev/null 2>&1").c_str());
    };
    h.rawPath = [](const std::string& p) { return rawStoragePath(p); };
    nanofilejob::Result r = nanofilejob::run(std::move(pairs), h);
    std::lock_guard<std::mutex> lk(mScrapeMutex);
    const nanofilejob::Counters& c = r.counters;
    mScrapeTotal = c.total; mScrapeDone = c.done; mScrapeHits = c.hits; mScrapeSkip = c.skip; mScrapeFail = c.fail;
    mScrapeError = c.error;
    mFileJobConflicts = std::move(r.conflicts);
    mFileJobFailedGroups = std::move(r.failedGroups);
    mFileJobIncomplete = r.incomplete;
    mScrapeRunning = false;
    mScrapeDoneFlag = true;
}

// Test 26's move rewrote RetroArch's default config as root (owner root, mode 0600), which keeps
// RetroArch on its defaults whenever it is given that config: given back once, when storage is up.
static void raRepairDefaultConfigOnce() {
    static std::atomic<bool> done{false};
    if (done.load(std::memory_order_relaxed)) return;
    struct stat st;
    if (stat("/data/media/0/Android", &st) != 0) return;   // not unlocked yet
    if (done.exchange(true)) return;
    if (nanora::raRepairOwner(nanora::kRaDefCfgRaw)) ALOGI("ps3menu: RetroArch's config given back to RetroArch");
}

// Render thread, every frame: once a copy has finished, switch the folder it was copying for.
// A cancelled copy leaves the setting as it was.
void NanoMenu::fileJobTick() {
    raRepairDefaultConfigOnce();
    if (mFileJobKind == 0) return;
    bool done, cancelled; int failed;
    {
        std::lock_guard<std::mutex> lk(mScrapeMutex);
        done = mScrapeDoneFlag && !mScrapeRunning;
        cancelled = mScrapeCancel && !mFileJobCommitted;   // a move past its copy finished anyway
        failed = mScrapeFail;
    }
    if (!done) return;
    const int kind = mFileJobKind;
    mFileJobKind = 0;
    if (kind >= 3) {   // a move to the microSD card (NanoSdMove.cpp)
        sdMoveFinish(kind, cancelled, failed);
        return;
    }
    if (cancelled) {
        photoShowBanner(trDyn("Copy cancelled. The folder was not changed."));
        return;
    }
    if (failed > 0) {   // switching would leave the files that failed behind
        photoShowBanner(trDyn("Some files could not be copied. The folder was not changed."));
        return;
    }
    if (kind == 1) esdeMediaFolderApply(mFileJobTo);
}

// ---------------------------------------------------------------------------
// RetroArch Folder. RetroArch's settings file and the folders of the user's own setup live
// either in RetroArch's defaults (its RetroArch folder on internal storage, its config in
// Android/data) or together in one chosen folder T: T/retroarch.cfg, T/system, T/saves, ....
// Its shaders, overlays, filters and controller profiles stay in its app folder wherever T is
// (a T made by an earlier version may hold its own copies, which move with it). Moving copies
// the files (keeping any already at the destination), then rewrites the folder settings in
// RetroArch's config files to the new place.
// ---------------------------------------------------------------------------

using nanora::kRaDefCfgDir;
using nanora::raCfgDir;
using nanora::raPart;
using nanora::raPrefixMap;
using nanora::raRewriteFile;
using nanora::raRewriteTree;

std::string NanoMenu::rawStoragePath(const std::string& p) { return nanora::raRawPath(p); }

std::string NanoMenu::retroarchDirRaw() {
    char d[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.retroarch.dir", d, "");
    return d[0] == '/' ? rawStoragePath(d) : std::string();
}

std::string NanoMenu::retroarchConfigFile() {
    // The watcher and a move's switch-over run this at once: one that read the folder setting
    // before the switch must not publish its stale answer after the other's. The home and overlay
    // processes each have a watcher, hence the second look at the setting.
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    char d[PROPERTY_VALUE_MAX] = {};
    property_get(nanora::kRaDirProp, d, "");
    std::string verified;
    const std::string cfg = nanora::raLaunchConfigNow(d, &verified);
    char now[PROPERTY_VALUE_MAX] = {}, cur[PROPERTY_VALUE_MAX] = {};
    property_get(nanora::kRaDirProp, now, "");
    property_get(nanora::kRaCfgDirProp, cur, "");
    if (strcmp(now, d) == 0 && verified != cur) property_set(nanora::kRaCfgDirProp, verified.c_str());
    return cfg;
}

// Keeps sys.gammaos.retroarch.cfg_dir current for RootWindowContainer's launches: at once, on every
// mount or unmount (a card in or out, storage coming up at boot), and every half minute for the
// file itself.
void NanoMenu::retroarchConfigWatchStart() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([]() {
        nanoThreadNormalPriority();
        const int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
        bool driftChecked = false;
        for (;;) {
            const std::string cfg = retroarchConfigFile();
            if (!driftChecked && cfg.compare(0, strlen(kRaDefCfgDir), kRaDefCfgDir) != 0) {
                driftChecked = true;
                // Through test 26 home-screen launches used the default config while the folder was
                // on the card. Its later settings stay where they are: which file holds the ones
                // the user wants cannot be told, so neither is overwritten.
                struct stat def, card;
                if (stat(nanora::kRaDefCfgRaw, &def) == 0 && stat(rawStoragePath(cfg).c_str(), &card) == 0
                    && def.st_mtime > card.st_mtime)
                    ALOGW("ps3menu: RetroArch's default config (%s) was saved %llds after %s, which every launch "
                          "now uses; nothing copied", nanora::kRaDefCfgRaw, (long long)(def.st_mtime - card.st_mtime),
                          cfg.c_str());
            }
            pollfd p = {fd, POLLPRI, 0};
            if (fd < 0 || poll(&p, 1, 30000) < 0) sleep(30);
        }
    }).detach();
}

// Move Emulator Files: RetroArch's folder moves to `to`; its default config is copied (RetroArch
// keeps its own, the rewrite points it at the new folder).
void NanoMenu::retroarchMovePairs(const std::string& to, const std::string& backup, int group,
                                  std::vector<FileCopyPair>* pairs) {
    char cur[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.retroarch.dir", cur, "");
    const std::string from = cur[0] == '/' ? std::string(cur) : std::string();
    mFileJobFrom = from;
    mFileJobTo = to;
    FileCopyPair main{raPart(from, ""), raPart(to, ""), false, !from.empty()};
    main.move = true;
    main.backup = backup;
    main.group = group;
    if (from.empty()) main.skipDirs = {"thumbnails"};   // box art: many files, and RetroArch downloads it again
    pairs->push_back(main);
    if (from.empty()) {
        FileCopyPair cfg{raCfgDir(from) + "/retroarch.cfg", raCfgDir(to) + "/retroarch.cfg"};
        cfg.group = group;
        pairs->push_back(cfg);
    }
}

namespace {

std::string raJoin(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size() && i < 8; i++) out += (i ? ", " : "") + v[i];
    if (v.size() > 8) out += ", ...";
    return out;
}

// RetroArch has a process (running, or still closing). Read from /proc, no program run.
bool raProcessRunning() {
    DIR* d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (!found) {
        const dirent* e = readdir(d);
        if (!e) break;
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char cmd[64] = {};
        const ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0) continue;
        static const char kPkg[] = "com.retroarch.aarch64";   // "<package>" or "<package>:<process>"
        found = strncmp(cmd, kPkg, sizeof(kPkg) - 1) == 0 && (cmd[sizeof(kPkg) - 1] == '\0' || cmd[sizeof(kPkg) - 1] == ':');
    }
    closedir(d);
    return found;
}

// The newest change to either of RetroArch's configs, seconds; 0 when neither is there.
long long raConfigsMtime(const std::string& dir) {
    long long newest = 0;
    for (const std::string& f : {std::string(kRaDefCfgDir) + "/retroarch.cfg", dir + "/retroarch.cfg"}) {
        struct stat st;
        if (stat(f.c_str(), &st) == 0) newest = std::max<long long>(newest, (long long)st.st_mtime);
    }
    return newest;
}

// Internal storage, and the volume the folder is on, are there to apps.
bool raStorageUp(const std::string& dir) {
    struct stat st;
    if (stat("/storage/emulated/0/Android", &st) != 0) return false;
    const std::string root = nanora::raVolumeRoot(dir);
    if (root.empty() || root == "/storage/emulated") return true;
    std::string mounts;
    const int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) mounts.append(buf, (size_t)n);
    close(fd);
    return nanora::raMountListed(mounts, root);
}

void raHealNow() {
    char d[PROPERTY_VALUE_MAX] = {};
    bool up = false;
    for (int i = 0; i < 120 && !up; i++) {   // storage and the card come up after Nano at boot
        if (i) sleep(1);
        property_get(nanora::kRaDirProp, d, "");
        up = d[0] != '/' || raStorageUp(d);
    }
    property_set("sys.gammaos.retroarch.healed", "1");
    if (d[0] != '/' || !up) return;
    // A RetroArch still open writes its own settings back when it closes.
    for (int i = 0; i < 20 && raProcessRunning(); i++) usleep(250000);
    if (raProcessRunning()) return;
    const std::string dir = d;
    nanora::raRepairOwner(nanora::kRaDefCfgRaw);
    bool any = false;
    for (const std::string& f : {std::string(kRaDefCfgDir) + "/retroarch.cfg", dir + "/retroarch.cfg"}) {
        std::vector<std::string> fixed;
        const int err = nanora::raHealConfig(f, dir, &fixed);
        if (err && err != ENOENT) {
            ALOGW("ps3menu: RetroArch config %s: %s", f.c_str(), strerror(err));
        } else if (!fixed.empty()) {
            any = true;
            ALOGW("ps3menu: RetroArch config %s pointed at %s again (%s)", f.c_str(), dir.c_str(), raJoin(fixed).c_str());
        }
    }
    if (any || !property_get_bool(nanora::kRaHealPendingProp, false)) return;
    // Both right after a RetroArch session (a config saved since the move): nothing to watch for now.
    char was[PROPERTY_VALUE_MAX] = {};
    property_get(nanora::kRaHealMtimeProp, was, "0");
    if (raConfigsMtime(dir) > strtoll(was, nullptr, 10)) {
        property_set(nanora::kRaHealPendingProp, "");
        property_set(nanora::kRaHealMtimeProp, "");
        ALOGI("ps3menu: RetroArch kept its folders on the card after the move");
    }
}

}  // namespace

void NanoMenu::retroarchHealStart(bool force) {
    char d[PROPERTY_VALUE_MAX] = {};
    property_get(nanora::kRaDirProp, d, "");
    if (d[0] != '/') return;   // on its defaults: no folder to point the configs at
    if (!force && property_get_bool("sys.gammaos.retroarch.healed", false)
        && !property_get_bool(nanora::kRaHealPendingProp, false))
        return;
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
    std::thread([]() {
        nanoThreadNormalPriority();
        raHealNow();
        busy = false;
    }).detach();
}

bool NanoMenu::retroarchFolderRewrite() {
    const std::string& from = mFileJobFrom;
    const std::string& to = mFileJobTo;
    const auto map = raPrefixMap(from, to);
    const auto keys = nanora::raMoveKeys(from, to);
    const std::string cfg = raCfgDir(to) + "/retroarch.cfg";
    const std::string def = std::string(kRaDefCfgDir) + "/retroarch.cfg";
    nanora::raRepairOwner(nanora::kRaDefCfgRaw);
    auto stale = [&](const std::string& f) {
        std::vector<std::string> keysLeft;
        const int err = nanora::raStaleKeysIn(f, map, &keys, &keysLeft);
        if (err) ALOGW("ps3menu: RetroArch folder: reading %s back: %s", f.c_str(), strerror(err));
        else if (!keysLeft.empty()) ALOGW("ps3menu: RetroArch folder: %s still names the old folders (%s)", f.c_str(), raJoin(keysLeft).c_str());
        return err || !keysLeft.empty();
    };
    // The new location's config (made from the folder settings when RetroArch never had one), read
    // back before anything else changes.
    int err = nanora::raRewriteFileEx(cfg, map, &keys, true);
    if (err) {
        ALOGW("ps3menu: RetroArch folder: rewriting %s: %s", cfg.c_str(), strerror(err));
        return false;   // the other configs keep the old folders too
    }
    if (stale(cfg)) return false;
    // The default one, which RetroArch opens when started from its icon and other frontends pass. None
    // yet is fine: RetroArch makes it on its first start and the heal (retroarchHealStart) points it.
    if (!to.empty()) {
        err = nanora::raRewriteFileEx(def, map, &keys);
        if (err && err != ENOENT) {
            ALOGW("ps3menu: RetroArch folder: rewriting %s: %s", def.c_str(), strerror(err));
            return false;
        }
        if (!err && stale(def)) {
            nanora::raRewriteFileEx(def, raPrefixMap(to, from), nullptr);   // the move is called off: back to the old folders
            return false;
        }
    }
    raRewriteTree(raPart(to, "") + "/config", map);      // per-core overrides, core options, shader presets
    raRewriteTree(raPart(to, "") + "/playlists", map);   // content kept in RetroArch's folder
    return true;
}

void NanoMenu::retroarchFolderFinish() {
    const std::string& from = mFileJobFrom;
    const std::string& to = mFileJobTo;
    property_set("persist.gammaos.retroarch.dir", to.c_str());
    retroarchConfigFile();   // published for the next home-screen launch
    // Folders outside internal storage need RetroArch's all-files access.
    if (!to.empty()) system("appops set --uid com.retroarch.aarch64 MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1");
    // RetroArch's next sessions are checked: one that cannot read its config saves its internal defaults.
    if (!to.empty()) {
        property_set(nanora::kRaHealMtimeProp, std::to_string(raConfigsMtime(to)).c_str());
        property_set(nanora::kRaHealPendingProp, "1");
    } else {
        property_set(nanora::kRaHealPendingProp, "");
    }
    loadRecentPlaylist();
    photoShowBanner(to.empty() ? trDyn("RetroArch folder: Default") : trDyn("RetroArch folder set"));
    ALOGI("ps3menu: RetroArch folder %s -> %s", from.empty() ? "default" : from.c_str(), to.empty() ? "default" : to.c_str());
    mDisplayDirty = true;
}

void NanoMenu::drasticFolderSelect(const char* prop, const char* what, const std::string& path) {
    if (path.empty() || path == "@default") {
        property_set(prop, "");
        photoShowBanner(folderFmt(trDyn("%s: Default"), trDyn(what)));
        ALOGI("ps3menu: %s reset to default", what);
    } else {
        std::string p = path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        struct stat st = {};
        if (stat(p.c_str(), &st) != 0) ::mkdir(p.c_str(), 0775);
        if (stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || access(p.c_str(), W_OK) != 0) {
            photoShowBanner(folderFmt(trDyn("%s: not writable"), trDyn(what)));
            ALOGW("ps3menu: %s %s rejected: %s", what, p.c_str(), strerror(errno));
        } else {
            property_set(prop, p.c_str());
            photoShowBanner(folderFmt(trDyn("%s set (next game launch)"), trDyn(what)));
            ALOGI("ps3menu: %s -> %s", what, p.c_str());
        }
    }
    if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDERBROWSE) mPs3Stack.pop_back();
    mDisplayDirty = true;
}

} // namespace android
