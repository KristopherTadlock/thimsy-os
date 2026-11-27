/*
 * Copyright (C) 2026 GammaOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

// Game Systems editor: the Emulator chooser, backed by the bundled Daijishou
// community platform configs (119 platforms, ~917 players). Covers RetroArch
// libretro cores AND standalone emulators (AetherSX2, Dolphin, azahar, ...).
// Parsed once at runtime with the embedded NanoJson reader; picking an entry
// auto-fills the system's launch type, core/package, intent template and
// accepted extensions per the Daijishou amStartArguments template.

#define LOG_TAG "GammaOSNano"

#include "NanoMenu.h"
#include "NanoEsdeSystemName.h"
#include "NanoJson.h"
#include "NanoSystemsHeal.h"
#include "NanoI18n.h"      // trDyn() runtime translation of hardcoded UI strings

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include <set>
#include <utils/Log.h>

namespace android {

// Read a whole file into a string (bounded). Returns "" on failure.
static std::string readWholeFile(const std::string& path, size_t maxSz) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    std::string out;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size < maxSz) {
        out.resize(st.st_size);
        ssize_t rd = read(fd, &out[0], st.st_size);
        if (rd > 0) out.resize(rd); else out.clear();
    }
    close(fd);
    return out;
}

// Whitespace-normalize an am string to single spaces (newlines/tabs -> space).
static std::string normalizeWs(const std::string& in) {
    std::string s; bool sp = false;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = true; }
        else { if (sp && !s.empty()) s += ' '; sp = false; s += c; }
    }
    return s;
}

// Extract a dotted, comma-separated extension list from a Daijishou
// acceptedFilenameRegex by reading its first (?:a|b|c) alternation group.
static std::string extsFromRegex(const std::string& rx) {
    size_t a = rx.find("(?:");
    if (a == std::string::npos) return "";
    size_t b = rx.find(')', a);
    if (b == std::string::npos) return "";
    std::string grp = rx.substr(a + 3, b - a - 3);
    std::string out, tok;
    auto flush = [&]() {
        if (!tok.empty() && tok.find('\\') == std::string::npos) {
            if (!out.empty()) out += ",";
            out += "." + tok;
        }
        tok.clear();
    };
    for (char c : grp) { if (c == '|') flush(); else tok += c; }
    flush();
    return out;
}

void NanoMenu::loadEmuCatalog() {
    if (!mEmuCatalog.empty()) return;
    const char* dirs[2] = {
        "/data/system/nano_xmb/daijishou",
        "/system/etc/nano_xmb/daijishou",
    };
    std::vector<std::string> files;
    std::string base;
    for (const char* d : dirs) {
        DIR* dp = opendir(d);
        if (!dp) continue;
        struct dirent* e;
        while ((e = readdir(dp)) != nullptr) {
            const char* n = e->d_name;
            if (n[0] == '.') continue;
            size_t len = strlen(n);
            if (len < 6 || strcasecmp(n + len - 5, ".json") != 0) continue;
            files.push_back(std::string(d) + "/" + n);
        }
        closedir(dp);
        if (!files.empty()) { base = d; break; }
    }
    for (const auto& f : files) {
        std::string text = readWholeFile(f, 2 * 1024 * 1024);
        if (text.empty()) continue;
        njson::Value root;
        if (!njson::parse(text, &root) || !root.isObject()) continue;
        const njson::Value* plat = root.find("platform");
        std::string platName = plat ? plat->getString("name") : "";
        std::string platRegex = plat ? plat->getString("acceptedFilenameRegex") : "";
        std::string platId = plat ? plat->getString("uniqueId") : "";
        if (platName.empty()) platName = trDyn("Platform");
        const njson::Value* players = root.find("playerList");
        if (!players || !players->isArray()) continue;
        for (const auto& pv : players->arr) {
            if (!pv.isObject()) continue;
            EmuCatEntry e;
            e.platform = platName;
            e.platformId = platId;
            e.player = pv.getString("name");
            e.amArgs = normalizeWs(pv.getString("amStartArguments"));
            e.playerRegex = pv.getString("acceptedFilenameRegex");
            e.platformRegex = platRegex;
            if (e.player.empty() || e.amArgs.empty()) continue;
            // The catalog lists every libretro core up to three times (the
            // RetroArch / RetroArch 32 / RetroArch 64 player variants). Keep
            // ONLY the 64-bit build (com.retroarch.aarch64): nano's native
            // LibretroRunner dlopen()s aarch64 cores, so the universal and the
            // 32-bit variants are pure duplicates here (the single 32-bit-only
            // core in the whole set could never load anyway).
            {
                std::string pkg;
                size_t np = e.amArgs.find("-n ");
                if (np != std::string::npos) {
                    size_t s = np + 3;
                    size_t epos = e.amArgs.find_first_of(" /", s);
                    pkg = e.amArgs.substr(s, epos == std::string::npos
                                                 ? std::string::npos : epos - s);
                }
                if (pkg.rfind("com.retroarch", 0) == 0
                    && pkg != "com.retroarch.aarch64") continue;
            }
            // Players are named "<uid> - <player>" in the catalog; the picker
            // label already leads with the platform display name, so drop the
            // redundant uid prefix ("ps2 - Aethersx2" -> "Aethersx2").
            if (!platId.empty() && e.player.size() > platId.size() + 3
                && strncasecmp(e.player.c_str(), platId.c_str(), platId.size()) == 0
                && e.player.compare(platId.size(), 3, " - ") == 0)
                e.player = e.player.substr(platId.size() + 3);
            mEmuCatalog.push_back(std::move(e));
        }
    }
    std::sort(mEmuCatalog.begin(), mEmuCatalog.end(),
              [](const EmuCatEntry& a, const EmuCatEntry& b) {
                  int c = strcasecmp(a.platform.c_str(), b.platform.c_str());
                  if (c != 0) return c < 0;
                  return strcasecmp(a.player.c_str(), b.player.c_str()) < 0;
              });
    ALOGI("emucatalog: loaded %zu players from %s", mEmuCatalog.size(), base.c_str());
}

// The catalog platform (uniqueId) a system plays: its ROM folder, or the folder's catalog name where
// they differ, else its short name; "" when the catalog has no such platform.
std::string NanoMenu::emuPlatformFor(const XmbSystem& sys) {
    static const std::pair<const char*, const char*> kAlias[] = {
        {"mastersystem", "master"}, {"sms", "master"}, {"n3ds", "3ds"}, {"pcengine", "tg16"}, {"pce", "tg16"},
        {"pcenginecd", "tgcd"}, {"megadrive", "genesis"}, {"md", "genesis"}, {"ps1", "psx"}, {"ps", "psx"},
        {"psvita", "vita"}, {"gamecube", "gc"}, {"segacd", "segacd"}, {"megacd", "segacd"}, {"32x", "sega32x"},
        {"wonderswan", "ws"}, {"wonderswancolor", "wsc"}, {"atarilynx", "lynx"}, {"colecovision", "coleco"},
    };
    auto known = [this](const std::string& uid) {
        for (const auto& e : mEmuCatalog) if (!strcasecmp(e.platformId.c_str(), uid.c_str())) return true;
        return false;
    };
    for (const std::string& raw : {sys.romDir, sys.shortname}) {
        std::string low = raw;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low.empty()) continue;
        if (known(low)) return low;
        for (const auto& a : kAlias)
            if (low == a.first && known(a.second)) return a.second;
    }
    return std::string();
}


static std::set<std::string> installedPackages() {
    std::set<std::string> out;
    std::string text = readWholeFile("/data/system/packages.list", 4 * 1024 * 1024);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        size_t sp = text.find(' ', pos);
        if (sp != std::string::npos && sp < eol) out.insert(text.substr(pos, sp - pos));
        pos = eol + 1;
    }
    return out;
}

std::set<std::string> NanoMenu::emuInstalledPackages() { return installedPackages(); }

static std::string emuPlayerLabel(const std::string& player) { return nanosys::playerLabel(player); }

std::string NanoMenu::emuEntryLabel(int catIdx) const {
    return catIdx >= 0 && catIdx < (int)mEmuCatalog.size() ? emuPlayerLabel(mEmuCatalog[catIdx].player) : std::string();
}

// An emulator can run here when its RetroArch core is shipped or its app is installed.
bool NanoMenu::emuEntryUsable(const XmbSystem& launch, const std::set<std::string>& installed) {
    return launch.launchType == XLT_LIBRETRO_CORE ? coreSoExists(launch.coreSo)
                                                  : installed.count(launch.packageName) > 0;
}

// A platform's emulators that can run here, RetroArch cores first, then apps, each by name; all of
// the platform's emulators (noneReady set) when none can.
std::vector<int> NanoMenu::emuPlatformChoices(const std::string& uid, bool& noneReady) {
    const std::set<std::string> installed = installedPackages();
    std::vector<int> idx;
    const std::vector<nanosys::Player> players = emuPlatformPlayers(uid, &idx);
    const auto order = nanosys::playerChoices(players, [&](const nanosys::Player& p) {
        return p.libretro ? coreSoExists(p.coreSo) : installed.count(p.package) > 0;
    }, &noneReady);
    std::vector<int> out;
    for (size_t i : order) out.push_back(idx[i]);
    return out;
}

// A platform's catalog players as they launch; *catIdx gets their mEmuCatalog indexes.
std::vector<nanosys::Player> NanoMenu::emuPlatformPlayers(const std::string& uid, std::vector<int>* catIdx) {
    std::vector<nanosys::Player> players;
    catIdx->clear();
    for (int i = 0; i < (int)mEmuCatalog.size(); i++) {
        if (strcasecmp(mEmuCatalog[i].platformId.c_str(), uid.c_str()) != 0) continue;
        XmbSystem launch; applyEmuEntryToSystem(launch, mEmuCatalog[i]);
        nanosys::Player p;
        p.name = mEmuCatalog[i].player;
        p.libretro = launch.launchType == XLT_LIBRETRO_CORE;
        p.coreSo = launch.coreSo;
        p.package = launch.packageName;
        players.push_back(std::move(p));
        catIdx->push_back(i);
    }
    return players;
}

// The emulator picker (screenKind GS_EMUPICK), in one of three views:
//  - out.emuPlatform set: that platform's emulators that can run here (RetroArch cores that are
//    shipped, apps that are installed), the system's own marked Active;
//  - adding a system: the platforms that have such an emulator, each opening its list;
//  - a search (mEmuPickFilter): every catalog emulator whose platform or name matches.
// Search All Emulators and Custom are at the bottom of each.
void NanoMenu::buildEmulatorPicker(Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.screenKind = GS_EMUPICK;
    out.title = "Emulator";
    const std::set<std::string> installed = installedPackages();
    auto usable = [&](const XmbSystem& launch) { return emuEntryUsable(launch, installed); };
    auto row = [&](const std::string& label, int kind, int a) -> Ps3Item& {
        Ps3Item it; it.label = label; it.kind = kind; it.a = a;
        it.iconTex = iconTexForIcon(22); it.nmapTex = nmapForIcon(22);
        it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
        return out.items.back();
    };
    const XmbSystem* cur = (!mGsAddMode && mGsEditIdx >= 0 && mGsEditIdx < (int)mXmbSystems.size())
                               ? &mXmbSystems[mGsEditIdx] : nullptr;
    if (!mEmuPickFilter.empty()) {
        { char t[256]; snprintf(t, sizeof(t), trDyn("Search: %s"), mEmuPickFilter.c_str()); out.title = t; }
        for (int i = 0; i < (int)mEmuCatalog.size(); i++) {
            const EmuCatEntry& e = mEmuCatalog[i];
            if (!strcasestr(e.platform.c_str(), mEmuPickFilter.c_str()) &&
                !strcasestr(e.player.c_str(), mEmuPickFilter.c_str()))
                continue;
            XmbSystem launch; applyEmuEntryToSystem(launch, e);
            Ps3Item& it = row(e.platform + " - " + emuPlayerLabel(e.player), PS3_GS_EMUROW, i);
            if (!usable(launch)) it.value = "Not installed";
        }
    } else if (!out.emuPlatform.empty()) {
        bool noneReady = false;
        for (int i : emuPlatformChoices(out.emuPlatform, noneReady)) {
            const EmuCatEntry& e = mEmuCatalog[i];
            out.title = e.platform;
            XmbSystem launch; applyEmuEntryToSystem(launch, e);
            Ps3Item& it = row(emuEntryLabel(i), PS3_GS_EMUROW, i);
            std::string exts = launch.acceptExts;
            for (auto& c : exts) if (c == ',') c = ' ';
            if (!exts.empty()) { char d[512]; snprintf(d, sizeof(d), trDyn("Opens %s files."), exts.c_str()); it.desc = d; }
            if (noneReady) it.value = "Not installed";
            if (cur && cur->launchType == launch.launchType
                && (launch.launchType == XLT_LIBRETRO_CORE ? cur->coreSo == launch.coreSo
                                                           : cur->packageName == launch.packageName)) {
                it.value = "Active";
                out.sel = (int)out.items.size() - 1;
            }
        }
    } else {
        // Adding a system: one row per platform that has an emulator ready.
        out.title = "Platform";
        std::map<std::string, std::pair<std::string, int>> platforms;   // name -> (uid, ready count)
        for (const auto& e : mEmuCatalog) {
            XmbSystem launch; applyEmuEntryToSystem(launch, e);
            auto& p = platforms[e.platform];
            p.first = e.platformId;
            if (usable(launch)) p.second++;
        }
        for (const auto& kv : platforms) {
            if (kv.second.second == 0 || kv.second.first.empty()) continue;
            Ps3Item& it = row(kv.first, PS3_GS_EMU_PLATFORM, 0);
            it.payloadStr = kv.second.first;
            char v[64];
            snprintf(v, sizeof(v), kv.second.second == 1 ? trDyn("%d emulator") : trDyn("%d emulators"), kv.second.second);
            it.value = v;
        }
    }
    { Ps3Item& it = row("Search All Emulators...", PS3_GS_EMU_SEARCH, 0);
      it.desc = "Search every emulator for every platform, installed or not."; }
    row(mGsAddMode ? "Custom System..." : "Custom (type core / package)...", PS3_GS_EMU_CUSTOM, 0);
}

// The picker's search: the OSK filters the whole catalog; an empty search goes back to the list.
void NanoMenu::emuPickerSearch() {
    openOskForPassword("Search Emulators", [this](const std::string& v) {
        mEmuPickFilter = v;   // strcasestr makes it case-insensitive
        if (!mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_EMUPICK) {
            buildEmulatorPicker(mPs3Stack.back());
            if (!v.empty()) mPs3Stack.back().sel = 0;
        }
        mDisplayDirty = true;
    });
    mOskPasswordMode = false; mOskPlaintext = true;
    mOskQuery = mEmuPickFilter; mOsk.caret = (int)mOskQuery.size();
}

// Adding a system: a platform row opens that platform's emulators.
void NanoMenu::emuPickerOpenPlatform(const std::string& uid) {
    mEmuPickFilter.clear();
    std::vector<Ps3Item> parentSnap = mPs3Stack.empty() ? std::vector<Ps3Item>() : mPs3Stack.back().items;
    int parentSel = mPs3Stack.empty() ? 0 : mPs3Stack.back().sel;
    Ps3Level lvl; lvl.emuPlatform = uid;
    buildEmulatorPicker(lvl);
    mPs3Stack.push_back(lvl);
    mPs3SubParentItems = std::move(parentSnap);
    mPs3SubParentIdx   = parentSel;
    mPs3SubChildItems  = mPs3Stack.back().items;
    mPs3SubDir = 1; mPs3SubAnimStart = mEffectTime; mPs3SubAnim = 0.0f;
    mPs3AnimItem = 0.0f; mPs3ItemAnimStart = -1.0f;
    mNdsCamera = (float)ndsFocusSel(); mDisplayDirty = true;
}

void NanoMenu::gsOpenEmulatorPicker() {
    if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size()) return;
    loadEmuCatalog();
    const XmbSystem& sys = mXmbSystems[mGsEditIdx];
    // The system's own platform; one the catalog doesn't know opens as a search for its name.
    Ps3Level lvl;
    lvl.emuPlatform = emuPlatformFor(sys);
    mEmuPickFilter = lvl.emuPlatform.empty() ? (sys.shortname.empty() ? sys.name : sys.shortname) : std::string();
    // Snapshot the parent (editor) for the collapse-rail animation, since this
    // push bypasses ps3XmbSelect's switch (which normally arms it).
    std::vector<Ps3Item> parentSnap = mPs3Stack.empty() ? std::vector<Ps3Item>() : mPs3Stack.back().items;
    int parentSel = mPs3Stack.empty() ? 0 : mPs3Stack.back().sel;

    buildEmulatorPicker(lvl);
    // If the pre-filter matched nothing, fall back to the full list.
    if (!mEmuPickFilter.empty() && lvl.items.size() <= 2) { mEmuPickFilter.clear(); buildEmulatorPicker(lvl); }
    mPs3Stack.push_back(lvl);

    // Animate the editor collapsing into the breadcrumb + the picker sliding in
    // (250ms easeOutCubic, dir +1) - same language as every other submenu push.
    mPs3SubParentItems = std::move(parentSnap);
    mPs3SubParentIdx   = parentSel;
    mPs3SubChildItems  = mPs3Stack.back().items;
    mPs3SubDir         = 1;
    mPs3SubAnimStart   = mEffectTime;
    mPs3SubAnim        = 0.0f;
    mPs3AnimItem       = 0.0f;
    mPs3ItemAnimStart  = -1.0f;
}

// Daijishou platform uniqueId -> LibretroDB monochrome icon basename (the
// bundled /system/etc/nano_xmb/icons_retroarch set). The catalog platform
// display names ("Sony PlayStation 2") do NOT match the LibretroDB icon
// naming ("Sony - PlayStation 2"), so new systems created from the catalog
// resolve their icon through this table. Verified complete against the
// bundled icon set; platforms with no sensible icon (pico8/lowresnx/wasm4)
// are absent and fall back to the sanitized-name guess -> generic cartridge.
static const struct { const char* uid; const char* icon; } kPlatformIconMap[] = {
    { "3do", "The_3DO_Company_-_3DO" },   // 3DO
    { "3ds", "Nintendo_-_Nintendo_3DS" },   // Nintendo 3DS
    { "adam", "Coleco_-_ColecoVision_ADAM" },   // Coleco Adam
    { "advision", "Entex_-_Adventure_Vision" },   // Entex Adventure Vision
    { "ags", "ScummVM" },   // Adventure Game Studio
    { "amiga", "Commodore_-_Amiga" },   // Commodore Amiga
    { "amigacd32", "Commodore_-_CD32" },   // Commodore Amiga CD32
    { "apf", "APF_-_MP-1000" },   // APF M-1000
    { "apple2gs", "Apple_-_IIGS" },   // Apple IIGS
    { "appleii", "Apple_-_II" },   // Apple II
    { "arcadia", "Emerson_-_Arcadia_2001" },   // Arcadia 2001
    { "archimedes", "Acorn_-_Archimedes" },   // Acorn Archimedes
    { "arduboy", "Arduboy_Inc_-_Arduboy" },   // Arduboy
    { "astrocade", "Bally_-_Astrocade" },   // Bally Astrocade
    { "atari2600", "Atari_-_2600" },   // Atari 2600
    { "atari5200", "Atari_-_5200" },   // Atari 5200
    { "atari7800", "Atari_-_7800" },   // Atari 7800
    { "atari800", "Atari_-_8-bit_Family" },   // Atari 8-bit
    { "atarist", "Atari_-_ST" },   // Atari ST
    { "atom", "Acorn_-_Atom" },   // Acorn Atom
    { "atomiswave", "Sega_-_Dreamcast" },   // Atomiswave
    { "bbcmicro", "Acorn_-_BBC_Micro" },   // Acorn Computers BBC Micro
    { "c128", "Commodore_-_128" },   // Commodore 128
    { "c64", "Commodore_-_64" },   // Commodore 64
    { "cannonball", "Cannonball" },   // Cannonball OutRun Engine
    { "cavestory", "Cave_Story" },   // Cave Story Game Engine
    { "cdi", "Philips_-_CD-i" },   // Philips CD-i
    { "cdtv", "Commodore_-_CDTV" },   // Commodore CDTV
    { "chailove", "ChaiLove" },   // ChaiLove
    { "channelf", "Fairchild_-_Channel_F" },   // Fairchild Channel F
    { "chip8", "CHIP-8" },   // CHIP-8
    { "coleco", "Coleco_-_ColecoVision" },   // ColecoVision
    { "cpc", "Amstrad_-_CPC" },   // Amstrad CPC
    { "cps1", "Capcom_-_CP_System_I" },   // CP System I
    { "cps2", "Capcom_-_CP_System_II" },   // CP System II
    { "cps3", "Capcom_-_CP_System_III" },   // CP System III
    { "crvision", "VTech_-_CreatiVision" },   // VTech CreatiVision
    { "doom", "DOOM" },   // DOOM Game Engine
    { "dos", "DOS" },   // DOS
    { "dreamcast", "Sega_-_Dreamcast" },   // Dreamcast
    { "ebook", "Non_Redump_-_Sony_Electronic_Book" },   // Book Reader
    { "elektor", "1292_Advanced_Programmable_Video_System" },   // Elektor TV Games Computer
    { "fbneo", "FBNeo_-_Arcade_Games" },   // Arcade (FinalBurn Neo)
    { "fds", "Nintendo_-_Family_Computer_Disk_System" },   // Famicom Disk System
    { "flashback", "Flashback" },   // Flashback Game Engine
    { "fm7", "Fujitsu_-_FM-7" },   // Fujitsu FM-7
    { "fmtowns", "Fujitsu_-_FM_Towns" },   // Fujitsu FM Towns
    { "g7400", "Philips_-_Videopacplus" },   // Philips Videopac+ G7400
    { "gamate", "Bit_Corporation_-_Gamate" },   // Bit Corporation Gamate
    { "gamecom", "Tiger_-_Game.com" },   // Tiger Game.com
    { "gamegear", "Sega_-_Game_Gear" },   // Sega Game Gear
    { "gb", "Nintendo_-_Game_Boy" },   // Nintendo - Game Boy
    { "gba", "Nintendo_-_Game_Boy_Advance" },   // Nintendo - Game Boy Advance
    { "gbc", "Nintendo_-_Game_Boy_Color" },   // Nintendo - Game Boy Color
    { "gc", "Nintendo_-_GameCube" },   // Nintendo - GameCube
    { "genesis", "Sega_-_Mega_Drive_-_Genesis" },   // Sega Genesis
    { "genesismsu", "Sega_-_Mega_Drive_-_Genesis" },   // Sega Genesis - MSU
    { "gmaster", "Hartung_-_Game_Master" },   // Hartung Game Master
    { "gw", "Nintendo_-_Game_and_Watch" },   // Nintendo - Game & Watch
    { "gx4000", "Amstrad_-_GX4000" },   // Amstrad GX4000
    { "idtech", "DOOM" },   // idTech4A++
    { "intellivision", "Mattel_-_Intellivision" },   // Intellivision
    { "j2me", "Mobile_-_J2ME" },   // Java Me
    { "jaguar", "Atari_-_Jaguar" },   // Atari Jaguar
    { "jaguarcd", "Atari_-_Jaguar_CD" },   // Atari Jaguar CD
    { "lutro", "Lutro" },   // Lutro
    { "lynx", "Atari_-_Lynx" },   // Atari Lynx
    { "macintosh", "Apple_-_Macintosh" },   // Apple Macintosh
    { "mame", "MAME" },   // Arcade (MAME)
    { "master", "Sega_-_Master_System_-_Mark_III" },   // Sega Master System
    { "megaduck", "Welback_-_Mega-Duck" },   // Mega Duck
    { "model2", "MAME" },   // Sega Model 2
    { "model3", "MAME" },   // Sega Model 3
    { "moonlight", "menu_stream" },   // Moonlight Streaming
    { "moto", "Thomson_-_MOTO" },   // Thomson MO/TO
    { "msx", "Microsoft_-_MSX" },   // MSX
    { "msxturbor", "Microsoft_-_MSX2" },   // MSX turbo R
    { "n64", "Nintendo_-_Nintendo_64" },   // Nintendo 64
    { "n64dd", "Nintendo_-_Nintendo_64DD" },   // Nintendo 64DD
    { "naomi", "Sega_-_Dreamcast" },   // Sega Naomi
    { "naomi2", "Sega_-_Dreamcast" },   // Sega Naomi 2
    { "nds", "Nintendo_-_Nintendo_DS" },   // Nintendo DS
    { "ndsi", "Nintendo_-_Nintendo_DSi" },   // Nintendo DSi
    { "neogeo", "SNK_-_Neo_Geo" },   // Neo Geo
    { "neogeocd", "SNK_-_Neo_Geo_CD" },   // Neo Geo CD
    { "nes", "Nintendo_-_Nintendo_Entertainment_System" },   // Nintendo Entertainment System
    { "ngage", "Nokia_-_N-Gage" },   // Nokia N-Gage
    { "ngp", "SNK_-_Neo_Geo_Pocket" },   // NeoGeo Pocket
    { "ngpc", "SNK_-_Neo_Geo_Pocket_Color" },   // NeoGeo Pocket Color
    { "odyssey2", "Magnavox_-_Odyssey2" },   // Magnavox Odyssey 2
    { "oric", "Tangerine_Computer_Systems_-_Oric" },   // Tangerine Oric
    { "palm", "Mobile_-_Palm_OS" },   // Palm OS
    { "pc88", "NEC_-_PC-8001_-_PC-8801" },   // NEC PC-88
    { "pc98", "NEC_-_PC-98" },   // NEC PC-98
    { "pcfx", "NEC_-_PC-FX" },   // NEC PC-FX
    { "pet", "Commodore_-_PET" },   // Commodore - PET
    { "pico", "Sega_-_PICO" },   // Sega Pico
    { "plus4", "Commodore_-_Plus-4" },   // Commodore - PLUS/4
    { "pokemini", "Nintendo_-_Pokemon_Mini" },   // Pokemon Mini
    { "ps2", "Sony_-_PlayStation_2" },   // Sony PlayStation 2
    { "ps3", "Sony_-_PlayStation_3" },   // Sony PlayStation 3
    { "psp", "Sony_-_PlayStation_Portable" },   // PlayStation Portable
    { "pspminis", "Sony_-_PlayStation_Portable_PSN" },   // PlayStation Portable Minis
    { "psx", "Sony_-_PlayStation" },   // Sony PlayStation
    { "pv1000", "Casio_-_PV-1000" },   // Casio PV-1000
    { "quake", "Quake" },   // Quake Game Engine
    { "quake2", "Quake_II" },   // Quake II Game Engine
    { "rpgmaker", "RPG_Maker" },   // RPG Maker
    { "satellaview", "Nintendo_-_Satellaview" },   // Nintendo Satellaview
    { "saturn", "Sega_-_Saturn" },   // Sega Saturn
    { "scummvm", "ScummVM" },   // ScummVM
    { "scv", "Epoch_-_Super_Cassette_Vision" },   // Epoch Super Cassette Vision
    { "sega32x", "Sega_-_32X" },   // Sega 32X
    { "segacd", "Sega_-_Mega-CD_-_Sega_CD" },   // Sega CD
    { "segacd32x", "Sega_-_32X" },   // Sega CD 32X
    { "sg1000", "Sega_-_SG-1000" },   // Sega SG-1000
    { "sgb", "Nintendo_-_Game_Boy" },   // Nintendo Super Game Boy
    { "snes", "Nintendo_-_Super_Nintendo_Entertainment_System" },   // Super Nintendo Entertainment System
    { "snesmsu1", "Nintendo_-_Super_Nintendo_Entertainment_System" },   // Super Nintendo Entertainment System - MSU-1
    { "spectravideo", "Spectravideo_-_SVI-318_-_SVI-328" },   // Spectravideo
    { "steam", "IBM_-_PC_and_Compatibles" },   // Steam
    { "studio2", "RCA_-_Studio_II" },   // RCA Studio II
    { "stv", "Sega_-_Saturn" },   // Sega ST-V
    { "sufami", "Nintendo_-_Sufami_Turbo" },   // Bandai Sufami Turbo
    { "supergrafx", "NEC_-_PC_Engine_SuperGrafx" },   // SuperGrafx
    { "supervision", "Watara_-_Supervision" },   // Watara Supervision
    { "supracan", "Funtech_-_Super_Acan" },   // Funtech Super A'Can
    { "switch", "Nintendo_-_Switch" },   // Nintendo Switch
    { "symbian", "Mobile_-_Symbian" },   // Symbian
    { "tg16", "NEC_-_PC_Engine_-_TurboGrafx_16" },   // TurboGrafx-16
    { "tgcd", "NEC_-_PC_Engine_CD_-_TurboGrafx-CD" },   // TurboGrafx-CD
    { "ti99", "Texas_Instruments_-_TI_99_4A" },   // Texas Instruments TI-99/4A
    { "tic80", "TIC-80" },   // TIC-80
    { "triforce", "Namco_Sega_Nintendo_-_TriForce_Cartridges" },   // Triforce
    { "uzebox", "Uzebox" },   // Uzebox
    { "varcade", "MAME" },   // Arcade (Vertical)
    { "vc4000", "Interton_-_VC_4000" },   // Interton VC 4000
    { "vectrex", "GCE_-_Vectrex" },   // Vectrex
    { "vic20", "Commodore_-_VIC-20" },   // VIC-20
    { "videos", "movies" },   // Video Player
    { "vircon32", "Vircon32" },   // Vircon32
    { "virtualboy", "Nintendo_-_Virtual_Boy" },   // Virtual Boy
    { "vita", "Sony_-_PlayStation_Vita" },   // Sony PS Vita
    { "vsmile", "VTech_-_V.Smile" },   // VTech V.Smile
    { "wii", "Nintendo_-_Wii" },   // Nintendo Wii
    { "wiiu", "Nintendo_-_Wii_U" },   // Nintendo Wii U
    { "wiiware", "Nintendo_-_Wii_Digital_CDN" },   // Nintendo WiiWare
    { "windows", "IBM_-_PC_and_Compatibles" },   // Windows
    { "windows3x", "IBM_-_PC_and_Compatibles" },   // Microsoft Windows 3.x
    { "windows9x", "IBM_-_PC_and_Compatibles" },   // Microsoft Windows 9x
    { "ws", "Bandai_-_WonderSwan" },   // WonderSwan
    { "wsc", "Bandai_-_WonderSwan_Color" },   // WonderSwan Color
    { "x1", "Sharp_-_X1" },   // Sharp X1
    { "x68000", "Sharp_-_X68000" },   // Sharp X68000
    { "xbox", "Microsoft_-_Xbox" },   // Microsoft - Xbox
    { "xbox360", "Microsoft_-_Xbox_360" },   // Microsoft - XBox 360
    { "xcloud", "Microsoft_-_Xbox_One_Digital" },   // XBox Game Pass
    { "zx81", "Sinclair_-_ZX_81" },   // ZX 81
    { "zxspectrum", "Sinclair_-_ZX_Spectrum" },   // ZX Spectrum
};

// Map a platform uniqueId to its bundled icon name, or "" when unmapped.
static std::string platformIconFor(const std::string& uid) {
    for (const auto& m : kPlatformIconMap)
        if (uid == m.uid) return m.icon;
    return "";
}

// Defined in NanoMenuPS3Folder.cpp.
int nano_makeUniqueSystem(std::vector<NanoMenu::XmbSystem>& systems, const std::string& name,
                          const std::string& preferredId);

// Default icon ref for a platform, shared by the emulator-picker add path and the bulk auto-add.
// Prefer the curated uniqueId->LibretroDB icon map; unmapped platforms fall back to a sanitized
// display-name guess, then to the generic cartridge inside resolveSystemIcon.
std::string NanoMenu::gsIconRefForPlatform(const std::string& platformId,
                                           const std::string& platformDisplay) {
    std::string iconName = platformIconFor(platformId);
    if (iconName.empty()) {
        iconName = platformDisplay;
        for (char& c : iconName)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-')) c = '_';
    }
    if (iconName.empty()) return "";
    return "retroarch:" + iconName;
}

// Apply a catalog entry's launch config (type / core / package / intent / exts)
// to a system, classifying RetroArch libretro players vs standalone packages.
void NanoMenu::applyEmuEntryToSystem(XmbSystem& s, const EmuCatEntry& e) {
    std::vector<std::string> toks;
    { std::string t; for (char c : e.amArgs) { if (c == ' ') { if (!t.empty()) { toks.push_back(t); t.clear(); } } else t += c; }
      if (!t.empty()) toks.push_back(t); }
    std::string comp, libretro;
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i] == "-n" && i + 1 < toks.size()) comp = toks[i + 1];
        if (toks[i] == "-e" && i + 2 < toks.size() && toks[i + 1] == "LIBRETRO") libretro = toks[i + 2];
    }
    std::string package = comp;
    { size_t sl = package.find('/'); if (sl != std::string::npos) package = package.substr(0, sl); }
    bool isRA = (package.find("retroarch") != std::string::npos) && !libretro.empty();
    if (isRA) {
        s.launchType = NanoMenu::XLT_LIBRETRO_CORE;
        s.coreSo = libretro + "_libretro_android.so";
        s.packageName.clear(); s.launchPkg.clear(); s.launchIntent.clear();
    } else {
        s.launchType = NanoMenu::XLT_CUSTOM_PACKAGE;
        s.coreSo.clear(); s.packageName = package; s.launchPkg = package;
        s.launchIntent = e.amArgs;   // tokens ({file.uri} etc.) preserved for launchXmbGame
    }
    std::string exts = extsFromRegex(e.playerRegex);
    if (exts.empty()) exts = extsFromRegex(e.platformRegex);
    if (!exts.empty()) s.acceptExts = exts;
}

void NanoMenu::applyEmulatorChoice(int catIdx) {
    if (catIdx < 0 || catIdx >= (int)mEmuCatalog.size()) return;
    const EmuCatEntry& e = mEmuCatalog[catIdx];

    bool adding = mGsAddMode;
    int idx;
    if (adding) {
        idx = nano_makeUniqueSystem(mXmbSystems, e.platform, e.platformId);
        // Platform icon from the curated uniqueId map (the Daijishou display names do not match the
        // LibretroDB icon names); unmapped platforms fall back to a sanitized-name guess, then to the
        // generic cartridge inside resolveSystemIcon. Shared with the bulk auto-add path.
        mXmbSystems[idx].iconRef = gsIconRefForPlatform(e.platformId, e.platform);
        mGsAddMode = false;
    } else {
        if (mGsEditIdx < 0 || mGsEditIdx >= (int)mXmbSystems.size()) return;
        idx = mGsEditIdx;
    }
    XmbSystem& s = mXmbSystems[idx];
    applyEmuEntryToSystem(s, e);
    ALOGI("emucatalog: %s '%s - %s' to %s (type=%d pkg=%s core=%s)",
          adding ? "added" : "applied", e.platform.c_str(), e.player.c_str(),
          s.id.c_str(), s.launchType, s.packageName.c_str(), s.coreSo.c_str());

    // Extensions likely changed: invalidate + rescan (off the bg path).
    invalidateSystemScan(s);
    forceRescanAllSystems();
    saveSystemsConfig();

    if (adding) {
        // Open the editor for the brand-new system. The picker was already
        // popped by the dispatch; the Game Systems list is below it.
        mGsEditIdx = idx;
        gsRefreshStackLevels();
        Ps3Level lvl; buildGameSystemEditor(idx, lvl); mPs3Stack.push_back(lvl);
    } else {
        gsRefreshStackLevels();
    }
    buildPs3Cats();
}

// ---- Per-game emulator (Game Options > Emulator) ----

static const char* kGameEmusPath = "/data/system/nano_game_emulators.json";

// Games are keyed by the path apps open them with, the form Recently Played stores, so a recent
// entry and the system list find the same choice.
std::string NanoMenu::gameEmuKey(const std::string& romPath) {
    if (romPath.rfind("/data/media/0/", 0) == 0) return "/sdcard/" + romPath.substr(14);
    if (romPath.rfind("/mnt/media_rw/", 0) == 0) return "/storage/" + romPath.substr(14);
    return romPath;
}

void NanoMenu::loadGameEmulators() {
    if (esdeStoreMode()) { esdeStoreLoadGameEmus(); return; }   // the gamelists' <altemulator>
    mGameEmusLoaded = true;
    mGameEmus.clear();
    mGameEmusLoadErr = false;
    if (access(kGameEmusPath, F_OK) != 0) return;   // absent: nothing chosen yet
    std::string text = readWholeFile(kGameEmusPath, 4 * 1024 * 1024);
    njson::Value root;
    const njson::Value* items = nullptr;
    if (!text.empty() && njson::parse(text, &root) && root.isObject()) items = root.find("items");
    if (!items || !items->isArray()) {
        mGameEmusLoadErr = true;   // unreadable: keep the file rather than overwrite it
        ALOGW("emucatalog: %s unreadable; not overwriting it", kGameEmusPath);
        return;
    }
    for (const auto& it : items->arr) {
        if (!it.isObject()) continue;
        std::string rom = it.getString("rom"), am = it.getString("am");
        if (rom.empty() || am.empty()) continue;
        mGameEmus[rom] = GameEmulator{it.getString("label"), am};
    }
}

void NanoMenu::saveGameEmulators() {
    if (mGameEmusLoadErr || esdeStoreMode()) return;
    njson::Value root = njson::Value::makeObject();
    njson::Value items = njson::Value::makeArray();
    for (const auto& kv : mGameEmus) {
        njson::Value o = njson::Value::makeObject();
        o.set("rom")   = njson::Value::makeString(kv.first);
        o.set("label") = njson::Value::makeString(kv.second.label);
        o.set("am")    = njson::Value::makeString(kv.second.amArgs);
        items.arr.push_back(std::move(o));
    }
    root.set("items") = std::move(items);
    std::string text = njson::serialize(root, true);
    std::string tmp = std::string(kGameEmusPath) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { ALOGW("emucatalog: cannot write %s", tmp.c_str()); return; }
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size() && fsync(fd) == 0;
    close(fd);
    if (ok) rename(tmp.c_str(), kGameEmusPath);
    else    unlink(tmp.c_str());
}

const NanoMenu::GameEmulator* NanoMenu::gameEmulatorFor(const std::string& romPath) {
    if (!mGameEmusLoaded) loadGameEmulators();
    auto it = mGameEmus.find(gameEmuKey(romPath));
    return it == mGameEmus.end() ? nullptr : &it->second;
}

void NanoMenu::setGameEmulator(const std::string& romPath, const GameEmulator* emu) {
    if (!mGameEmusLoaded) loadGameEmulators();
    const std::string key = gameEmuKey(romPath);
    if (emu) mGameEmus[key] = *emu;
    else     mGameEmus.erase(key);
    if (esdeStoreMode()) esdeStoreSaveGameEmu(romPath, emu);
    else saveGameEmulators();
    // Recently Played records the emulator a game was started with, and the overlay launches from
    // that record: give the game's entry its system's current emulator, then its own, if it has one.
    bool changed = false;
    for (auto& re : mXmbRecent) {
        if (gameEmuKey(re.romPath) != key) continue;
        const int si = recentSystemIndex(re);
        if (si < 0) continue;
        const XmbSystem& sys = mXmbSystems[si];
        XmbSystem g;   // just the system's launch fields
        g.launchType = sys.launchType; g.coreSo = sys.coreSo; g.launchPkg = sys.launchPkg;
        g.packageName = sys.packageName; g.launchIntent = sys.launchIntent;
        applyGameEmulator(re.romPath, g);
        re.coreSo = g.coreSo; re.launchPkg = g.launchPkg; re.launchIntent = g.launchIntent;
        re.standalone = g.isStandalone();
        changed = true;
    }
    if (changed) saveXmbRecent();
}

// A platform added twice shares its shortname across both systems, so the folder decides; the
// shortname is only for an entry without a folder, or one whose folder is no longer a system.
int NanoMenu::recentSystemIndex(const XmbRecentEntry& re) const {
    if (!re.romDir.empty())
        for (size_t i = 0; i < mXmbSystems.size(); i++)
            if (mXmbSystems[i].romDir == re.romDir) return (int)i;
    if (!re.systemName.empty())
        for (size_t i = 0; i < mXmbSystems.size(); i++)
            if (mXmbSystems[i].shortname == re.systemName) return (int)i;
    return -1;
}

bool NanoMenu::applyGameEmulator(const std::string& romPath, XmbSystem& s) {
    const GameEmulator* g = gameEmulatorFor(romPath);
    if (!g) return false;
    EmuCatEntry e;
    e.amArgs = g->amArgs;
    applyEmuEntryToSystem(s, e);
    s.launchArgs.clear();   // the system's extra arguments belong to its own emulator
    return true;
}

// ---- OA-38: a pick this platform cannot run starts with the image's default instead ----

// The device's store of the image's default emulators: {"version": 1, "systems": {<key>: {"core":
// "<core>_libretro_android.so"} or {"package": "...", "intent": "..."}}}, keyed by the system's ROM
// folder (the built-in systems' id) or its id. Written once, on the first run (seedEmuDefaults).
static const char* kEmuDefaultsPath = "/data/system/nano_emu_defaults.json";

void NanoMenu::loadEmuDefaults() {
    mEmuDefaultsLoaded = true;
    mEmuDefaults.clear();
    const std::string text = readWholeFile(kEmuDefaultsPath, 1024 * 1024);
    njson::Value root;
    if (text.empty() || !njson::parse(text, &root) || !root.isObject()) return;
    const njson::Value* systems = root.find("systems");
    if (!systems || !systems->isObject()) return;
    for (const auto& kv : systems->obj) {
        if (kv.first.empty() || !kv.second.isObject()) continue;
        EmuDefault d{kv.second.getString("core"), kv.second.getString("package"), kv.second.getString("intent")};
        if (d.coreSo.empty() == d.package.empty()) continue;   // a core or an app, not both
        mEmuDefaults[kv.first] = std::move(d);
    }
}

bool NanoMenu::emuDefaultsStored() {
    struct stat st;
    return stat(kEmuDefaultsPath, &st) == 0 || errno != ENOENT;
}

bool NanoMenu::emuDefaultsSave(const std::map<std::string, EmuDefault>& defaults) {
    njson::Value systems = njson::Value::makeObject();
    for (const auto& kv : defaults) {
        njson::Value o = njson::Value::makeObject();
        if (!kv.second.coreSo.empty()) {
            o.set("core") = njson::Value::makeString(kv.second.coreSo);
        } else {
            o.set("package") = njson::Value::makeString(kv.second.package);
            o.set("intent") = njson::Value::makeString(kv.second.intent);
        }
        systems.set(kv.first) = std::move(o);
    }
    njson::Value root = njson::Value::makeObject();
    root.set("version") = njson::Value::makeNumber(1);
    root.set("systems") = std::move(systems);
    const std::string text = njson::serialize(root, true);
    const std::string tmp = std::string(kEmuDefaultsPath) + ".tmp." + std::to_string(getpid());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { ALOGW("emucatalog: cannot write %s", tmp.c_str()); return false; }
    const bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size() && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp.c_str(), kEmuDefaultsPath) != 0) { unlink(tmp.c_str()); return false; }
    mEmuDefaultsLoaded = false;   // read again at the next fallback
    return true;
}

bool NanoMenu::emuCanRun(const XmbSystem& s) {
    return s.isStandalone() ? packageInstalled(s.launchPkg) : coreSoExists(s.coreSo);
}

bool NanoMenu::emuApplyDefault(XmbSystem& s) {
    if (!mEmuDefaultsLoaded) loadEmuDefaults();
    auto it = mEmuDefaults.find(s.romDir);
    if (it == mEmuDefaults.end()) it = mEmuDefaults.find(s.id);
    if (it == mEmuDefaults.end()) return false;
    XmbSystem d;   // the default's launch fields, as applyEmuEntryToSystem sets them
    if (!it->second.coreSo.empty()) {
        d.launchType = XLT_LIBRETRO_CORE;
        d.coreSo = it->second.coreSo;
    } else {
        d.launchType = XLT_CUSTOM_PACKAGE;
        d.packageName = d.launchPkg = it->second.package;
        d.launchIntent = it->second.intent;
    }
    if (!emuCanRun(d)) return false;
    ALOGI("emucatalog: %s's emulator (%s) cannot run here; this launch uses the default %s", it->first.c_str(),
          s.isStandalone() ? s.launchPkg.c_str() : s.coreSo.c_str(),
          d.isStandalone() ? d.launchPkg.c_str() : d.coreSo.c_str());
    s.launchType = d.launchType;
    s.coreSo = d.coreSo;
    s.packageName = d.packageName;
    s.launchPkg = d.launchPkg;
    s.launchIntent = d.launchIntent;
    s.launchArgs.clear();   // the system's extra arguments belong to its own emulator
    return true;
}

// What a launch whose emulator cannot run here starts instead, for that launch only (nothing is
// saved): the image's default for its system, else, for a game whose own pick s carries (own is
// that game's system), the system's own emulator, as such a game started before its pick was kept.
NanoMenu::EmuFallback NanoMenu::emuLaunchFallback(XmbSystem& s, const XmbSystem* own) {
    if (emuCanRun(s)) return EmuFallback::None;
    if (emuApplyDefault(s)) return EmuFallback::ImageDefault;
    if (!own || !emuCanRun(*own)) return EmuFallback::None;
    ALOGI("emucatalog: the game's emulator (%s) cannot run here and %s has no default that can; this launch "
          "uses the system's own %s", s.isStandalone() ? s.launchPkg.c_str() : s.coreSo.c_str(), own->id.c_str(),
          own->isStandalone() ? own->launchPkg.c_str() : own->coreSo.c_str());
    s.launchType = own->launchType;
    s.coreSo = own->coreSo;
    s.packageName = own->packageName;
    s.launchPkg = own->launchPkg;
    s.launchIntent = own->launchIntent;
    s.launchArgs = own->launchArgs;   // its own emulator's extra arguments
    return EmuFallback::SystemOwn;
}

// ---- Emulators as ES-DE names them (the ES-DE folder as Nano's store) ----

// The label of ES-DE's command for esdeSys that starts launch's emulator; "" when ES-DE has none.
std::string NanoMenu::esdeEmulatorLabel(const std::string& esdeSys, const XmbSystem& launch) {
    const std::string pkg = launch.packageName.empty() ? launch.launchPkg : launch.packageName;
    return esdesys::esdeLabelForKey(esdeSys, esdesys::esdeEmulatorKey(launch.launchType != XLT_CUSTOM_PACKAGE,
                                                                      launch.coreSo, pkg));
}

// The emulator of the system's platform that ES-DE's command `label` (for esdeSys) starts, when it
// can run here (or, with runnableOnly false, the first such emulator Nano's catalog has); -1 when
// there is none.
int NanoMenu::esdeCatalogForLabel(int sysIdx, const std::string& esdeSys, const std::string& label,
                                  const std::set<std::string>& installed, bool runnableOnly) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return -1;
    const std::vector<std::string> keys = esdesys::esdeKeysForLabel(esdeSys, label);
    if (keys.empty()) return -1;
    loadEmuCatalog();
    std::vector<int> idx;
    const std::vector<nanosys::Player> players = emuPlatformPlayers(emuPlatformFor(mXmbSystems[sysIdx]), &idx);
    for (const auto& k : keys)
        for (size_t i = 0; i < players.size(); i++) {
            const nanosys::Player& p = players[i];
            if (esdesys::esdeEmulatorKey(p.libretro, p.coreSo, p.package) != k) continue;
            if (!runnableOnly || (p.libretro ? coreSoExists(p.coreSo) : installed.count(p.package) > 0))
                return idx[i];
        }
    return -1;
}

std::vector<std::string> NanoMenu::esdeSystemFolders(int sysIdx) {
    std::vector<std::string> out;
    for (const auto& r : esdeSystemRoots(sysIdx)) {
        const std::string f = esdeRootFolder(r, sysIdx);
        if (!f.empty() && std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
    }
    return out;
}

// The ES-DE system whose commands an ES-DE folder's emulators are: the folder's, or for a folder ES-DE
// has no system for, the one Nano's system is (its custom system has that one's commands).
std::string NanoMenu::esdeCommandSystem(const std::string& folder, int sysIdx) {
    if (esdesys::esdeKnownSystem(folder) || sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return folder;
    return esdesys::esdePlatformSystemName(mXmbSystems[sysIdx].romDir, mXmbSystems[sysIdx].shortname);
}

// What each ES-DE folder's system emulator was when Nano and its gamelist last agreed (kept across
// restarts, so a change made on either side while the card was out is told from the other's).
static const char* const kEsdeSysEmuSync = "/data/system/nano_esde_sysemu.sync";

void NanoMenu::esdeSysEmuSyncedLoad() {
    if (mEsdeSysEmuSyncedLoaded) return;
    mEsdeSysEmuSyncedLoaded = true;
    std::string text = readWholeFile(kEsdeSysEmuSync, 1024 * 1024);
    for (size_t p = 0; p < text.size();) {
        size_t nl = text.find('\n', p);
        if (nl == std::string::npos) nl = text.size();
        const std::string l = text.substr(p, nl - p);
        p = nl + 1;
        const size_t tab = l.find('\t');
        if (tab != std::string::npos && tab > 0) mEsdeSysEmuSynced[l.substr(0, tab)] = l.substr(tab + 1);
    }
}

void NanoMenu::esdeSysEmuSyncedSave() {
    std::string text;
    for (const auto& kv : mEsdeSysEmuSynced) text += kv.first + "\t" + kv.second + "\n";
    const std::string tmp = std::string(kEsdeSysEmuSync) + ".tmp." + std::to_string(getpid());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    const bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    close(fd);
    if (!ok || rename(tmp.c_str(), kEsdeSysEmuSync) != 0) unlink(tmp.c_str());
}

// Each system's <alternativeEmulator> (chosen in ES-DE, or by Nano before): the system's emulator, when
// Nano's catalog has it and it can run here, unless the gamelist still says what was agreed and Nano's
// changed since (then the gamelist gets Nano's). A gamelist without one gets the system's when it is not
// ES-DE's default. A folder two systems share goes by the first.
void NanoMenu::esdeStoreApplySystemEmus() {
    if (!esdeStoreReady() || mEsdeSystemsAdopted) return;
    if (mBgScanThreadRunning) { mEsdeApplyAfterLoad = true; return; }   // the scan reads the systems: after it
    esdeSysEmuSyncedLoad();
    const std::map<std::string, std::string> syncedBefore = mEsdeSysEmuSynced;
    std::set<std::string> installed, seen;
    bool haveInstalled = false, changed = false;
    for (int s = 0; s < (int)mXmbSystems.size(); s++) {
        std::vector<std::string> folders;
        for (const auto& f : esdeSystemFolders(s)) if (seen.insert(f).second) folders.push_back(f);
        std::string folder, label;
        for (const auto& f : folders) {
            auto it = mEsdeGlSysEmu.find(f);
            if (it != mEsdeGlSysEmu.end()) { folder = f; label = it->second; break; }
        }
        auto agreed = mEsdeSysEmuSynced.find(folder);
        const bool nanoChanged = agreed != mEsdeSysEmuSynced.end() && agreed->second == label
                                 && esdeEmulatorLabel(esdeCommandSystem(folder, s), mXmbSystems[s]) != label;
        if (!label.empty() && !nanoChanged) {
            const std::string cmds = esdeCommandSystem(folder, s);
            const std::vector<std::string> keys = esdesys::esdeKeysForLabel(cmds, label);
            const XmbSystem& sys = mXmbSystems[s];
            const std::string pkg = sys.packageName.empty() ? sys.launchPkg : sys.packageName;
            const std::string cur = esdesys::esdeEmulatorKey(sys.launchType != XLT_CUSTOM_PACKAGE, sys.coreSo, pkg);
            if (!keys.empty() && std::find(keys.begin(), keys.end(), cur) == keys.end()) {
                if (!haveInstalled) { installed = emuInstalledPackages(); haveInstalled = true; }
                const int cat = esdeCatalogForLabel(s, cmds, label, installed);
                if (cat >= 0) {
                    applyEmuEntryToSystem(mXmbSystems[s], mEmuCatalog[cat]);
                    mXmbSystems[s].launchArgs.clear();   // the old emulator's
                    invalidateSystemScan(mXmbSystems[s]);   // its extensions can differ
                    changed = true;
                    ALOGI("esde: %s plays with %s (its gamelist's emulator)", mXmbSystems[s].id.c_str(), label.c_str());
                }
            }
        }
        for (const auto& f : folders) {
            if (mEsdeGlUnreadable.count(f)) continue;   // what it says is not known: nothing to agree on
            const std::string mine = esdeEmulatorLabel(esdeCommandSystem(f, s), mXmbSystems[s]);
            auto ag = syncedBefore.find(f);
            auto gl = mEsdeGlSysEmu.find(f);
            if (ag != syncedBefore.end() && gl != mEsdeGlSysEmu.end() && gl->second == ag->second && mine != ag->second) {
                esdeGamelistSetSystem(f, "alternativeEmulator", mine);   // Nano's choice made while the card was out
                if (mine.empty()) mEsdeGlSysEmu.erase(gl); else gl->second = mine;
            } else if (!mine.empty() && gl == mEsdeGlSysEmu.end() && !mXmbSystems[s].roms.empty()
                       && mine != esdesys::esdeDefaultLabel(esdeCommandSystem(f, s))) {
                esdeGamelistSetSystem(f, "alternativeEmulator", mine);   // ES-DE told, where nobody chose one
                mEsdeGlSysEmu[f] = mine;
            }
            mEsdeSysEmuSynced[f] = mine;
        }
    }
    if (mEsdeSysEmuSynced != syncedBefore) esdeSysEmuSyncedSave();
    if (changed) {
        saveSystemsConfig();
        forceRescanAllSystems();
        gsRefreshStackLevels();
        mPs3CatsStale = true;
    }
}

// A system's emulator changed in Nano: its gamelists' <alternativeEmulator>, or none when ES-DE has no
// command for it (ES-DE then uses its default; Nano keeps its own). A folder not read yet
// (esdeStoreApplySystemEmus) is left alone: what the card says is not known.
void NanoMenu::esdeStoreSaveSystemEmus() {
    if (!esdeStoreReady() || mEsdeSystemsAdopted) return;
    esdeSysEmuSyncedLoad();
    const std::map<std::string, std::string> syncedBefore = mEsdeSysEmuSynced;
    std::set<std::string> seen;
    for (int s = 0; s < (int)mXmbSystems.size(); s++) {
        for (const auto& f : esdeSystemFolders(s)) {
            if (!seen.insert(f).second || mEsdeGlUnreadable.count(f)) continue;
            auto it = mEsdeSysEmuSynced.find(f);
            if (it == mEsdeSysEmuSynced.end()) continue;
            const std::string label = esdeEmulatorLabel(esdeCommandSystem(f, s), mXmbSystems[s]);
            if (it->second == label) continue;
            it->second = label;
            auto have = mEsdeGlSysEmu.find(f);
            if (label.empty()) {
                if (have == mEsdeGlSysEmu.end()) continue;
                esdeGamelistSetSystem(f, "alternativeEmulator", std::string());
                mEsdeGlSysEmu.erase(have);
                continue;
            }
            if (have != mEsdeGlSysEmu.end() && have->second == label) continue;
            esdeGamelistSetSystem(f, "alternativeEmulator", label);
            mEsdeGlSysEmu[f] = label;
        }
    }
    if (mEsdeSysEmuSynced != syncedBefore) esdeSysEmuSyncedSave();
}

} // namespace android
