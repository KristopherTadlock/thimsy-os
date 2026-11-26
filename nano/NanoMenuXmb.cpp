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

// XMB system definitions, ROM scanning, game launch, recently played,
// on-screen keyboard (search), and XMB rendering.

#define LOG_TAG "GammaOSNano"

#include <algorithm>
#include <thread>
#include <mutex>
#include <set>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <strings.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>

#include <cutils/properties.h>
#include <android-base/properties.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>

#include <GLES2/gl2.h>

#include "NanoMenuDrm.h"
#include "NanoMenuUtils.h"
#include "NanoMenu.h"
#include "NanoI18n.h"      // trDyn() runtime translation of hardcoded UI strings
#include "NanoMenuShaders.h"
#include "NanoJson.h"
#include "NanoLaunch.h"
#include "NanoLaunchCommit.h"   // commitLaunch: the launch handshake's writes
#include "NanoSystemsHeal.h"
#include "NanoBiosUtil.h"
#include "NanoScummvmList.h"
#include <openssl/md5.h>
#include <fstream>
#include <cstdarg>

// printf into a std::string; used with translated format strings (trDyn("%s ...")).
static std::string xmbFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string xmbFmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// ---------------------------------------------------------------------------
// Mupen64Plus AE direct launch. The stock VIEW intent goes SplashActivity ->
// GalleryActivity -> GameActivity: two activities in the app's main process (which
// then only exists to be reaped by the low memory killer) that on the 1 GB RG DS cost
// 10 to 20 s of black screen before the game process even starts. GameActivity itself
// only needs what the gallery computes from the ROM: its MD5 over the byte-order
// normalised (z64) image, the header CRC pair, the internal header name and the
// mupen64plus.ini good name. Compute those here and start GameActivity directly, so
// only the :EmulationProcess is created. Measured: game running 17 s after the intent
// instead of 35 to 45 s. Zips and 7z archives keep the stock path (the gallery extracts
// them); any read failure also falls back to the stock intent.
// ---------------------------------------------------------------------------
static bool mupenRomMeta(const std::string& romPath, std::string& md5Hex, std::string& crc,
                         std::string& headerName) {
    int fd = open(romPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0x1000) { close(fd); return false; }
    uint8_t hdr[4];
    if (read(fd, hdr, 4) != 4) { close(fd); return false; }
    // 80371240 = z64 (big-endian, native); 37804012 = v64 (16-bit swapped); 40123780 = n64 (32-bit LE)
    enum { Z64, V64, N64 } fmt;
    if (hdr[0] == 0x80 && hdr[1] == 0x37) fmt = Z64;
    else if (hdr[0] == 0x37 && hdr[1] == 0x80) fmt = V64;
    else if (hdr[0] == 0x40 && hdr[1] == 0x12) fmt = N64;
    else { close(fd); return false; }
    // md5 cache: path|size|mtime|md5|crc|name (one ROM per line)
    const char* cachePath = "/data/system/nano_mupen_rom_cache.txt";
    char key[640];
    snprintf(key, sizeof key, "%s|%lld|%lld|", romPath.c_str(), (long long)st.st_size, (long long)st.st_mtime);
    {
        std::ifstream cf(cachePath);
        std::string line;
        while (std::getline(cf, line)) {
            if (line.compare(0, strlen(key), key) == 0) {
                std::string rest = line.substr(strlen(key));
                size_t a = rest.find('|'), b = (a == std::string::npos) ? a : rest.find('|', a + 1);
                if (a != std::string::npos && b != std::string::npos) {
                    md5Hex = rest.substr(0, a); crc = rest.substr(a + 1, b - a - 1); headerName = rest.substr(b + 1);
                    close(fd);
                    return true;
                }
            }
        }
    }
    lseek(fd, 0, SEEK_SET);
    MD5_CTX ctx;
    MD5_Init(&ctx);
    std::vector<uint8_t> buf(1 << 20);
    std::vector<uint8_t> first(64);
    bool haveFirst = false;
    for (;;) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n < 0) { close(fd); return false; }
        if (n == 0) break;
        size_t len = (size_t)n & ~(size_t)3;   // ROMs are 4-byte multiples; drop any stray tail
        if (fmt == V64) {
            for (size_t i = 0; i + 1 < len; i += 2) std::swap(buf[i], buf[i + 1]);
        } else if (fmt == N64) {
            for (size_t i = 0; i + 3 < len; i += 4) { std::swap(buf[i], buf[i + 3]); std::swap(buf[i + 1], buf[i + 2]); }
        }
        if (!haveFirst) { memcpy(first.data(), buf.data(), 64); haveFirst = true; }
        MD5_Update(&ctx, buf.data(), len);
    }
    close(fd);
    unsigned char dig[16];
    MD5_Final(dig, &ctx);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02X", dig[i]);
    md5Hex = hex;
    char crcbuf[24];
    uint32_t c1 = ((uint32_t)first[0x10] << 24) | ((uint32_t)first[0x11] << 16) | ((uint32_t)first[0x12] << 8) | first[0x13];
    uint32_t c2 = ((uint32_t)first[0x14] << 24) | ((uint32_t)first[0x15] << 16) | ((uint32_t)first[0x16] << 8) | first[0x17];
    snprintf(crcbuf, sizeof crcbuf, "%08X %08X", c1, c2);
    crc = crcbuf;
    headerName.assign((const char*)first.data() + 0x20, 20);
    while (!headerName.empty() && (headerName.back() == ' ' || headerName.back() == '\0')) headerName.pop_back();
    for (char& ch : headerName) if (ch == '\t' || ch == '\n' || ch == '|') ch = ' ';
    {
        std::ofstream cf(cachePath, std::ios::app);
        cf << key << md5Hex << '|' << crc << '|' << headerName << '\n';
    }
    return true;
}

static std::string mupenGoodName(const std::string& pkg, const std::string& md5Hex) {
    std::string ini = "/data/data/" + pkg + "/files/mupen64plus.ini";
    std::ifstream f(ini);
    if (!f) return "";
    std::string line, want = "[" + md5Hex + "]";
    bool in = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') { in = (line == want); continue; }
        if (in && line.compare(0, 9, "GoodName=") == 0) return line.substr(9);
    }
    return "";
}

// Build the GameActivity intent arguments. tabbed=true yields the tab separated token
// list the framework's nano intent file expects (RootWindowContainer.parseAmIntent);
// tabbed=false yields a shell-quoted `am start` argument string.
bool android::NanoMenu::mupenDirectIntent(const std::string& pkg, const std::string& romPath,
                                 const std::string& contentUri, bool tabbed, std::string& out) {
    if (pkg.compare(0, 18, "org.mupen64plusae.") != 0) return false;
    std::string lower = romPath;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".zip") == 0 || lower.compare(lower.size() - 3, 3, ".7z") == 0))
        return false;
    std::string md5, crc, hname;
    if (!mupenRomMeta(romPath, md5, crc, hname)) return false;
    std::string good = mupenGoodName(pkg, md5);
    std::string display = romPath;
    { size_t ls = display.rfind('/'); if (ls != std::string::npos) display = display.substr(ls + 1);
      size_t dot = display.rfind('.'); if (dot != std::string::npos) display.erase(dot); }
    if (good.empty()) good = display;
    // Hand the ROM over as a file URI on the app-visible path, not a SAF content URI:
    // GammaOS relaxes storage access, so the emulator opens the file directly, nothing
    // is copied, and the ExternalStorageProvider process (about 45 MB) is never started.
    std::string appPath = romPath;
    if (appPath.rfind("/data/media/0/", 0) == 0) appPath = "/storage/emulated/0/" + appPath.substr(14);
    else if (appPath.rfind("/mnt/media_rw/", 0) == 0) appPath = "/storage/" + appPath.substr(14);
    const std::string fileUri = nanolaunch::fileUri(appPath);
    (void)contentUri;
    const std::string K = "paulscode.android.mupen64plusae.ActivityHelper.Keys.";
    const std::vector<nanolaunch::AmArg> args = {
        {"-n", {pkg + "/paulscode.android.mupen64plusae.game.GameActivity"}},
        {"-a", {"android.intent.action.MAIN"}},
        {"-d", {fileUri}},
        {"-es", {K + "ROM_PATH", fileUri}},
        {"-es", {K + "ROM_MD5", md5}},
        {"-es", {K + "ROM_CRC", crc}},
        {"-es", {K + "ROM_HEADER_NAME", hname}},
        {"-es", {K + "ROM_GOOD_NAME", good}},
        {"-es", {K + "ROM_DISPLAY_NAME", display}},
        {"-es", {K + "ROM_ART_PATH", ""}},
    };
    // A name that cannot be carried as one token falls back to the stock intent.
    if (!nanolaunch::joinAmArgs(args, tabbed, out)) return false;
    ALOGI("NanoMenu: mupen direct launch md5=%s crc=%s name='%s' good='%s'", md5.c_str(), crc.c_str(),
          hname.c_str(), good.c_str());
    return true;
}

namespace android {

// ---------------------------------------------------------------------------
// XMB system definitions (from Daijisho database)
// ---------------------------------------------------------------------------

struct SystemDef {
    const char* name;
    const char* shortname;
    const char* romDir;       // Directory name under /sdcard/ROMs/
    const char* coreSo;       // RetroArch core .so filename (empty for standalone)
    const char* launchPkg;    // Package name for standalone emulators (empty for RetroArch)
    const char* launchIntent; // Intent template for standalone ({file.uri} placeholder)
    float r, g, b;            // Icon color
    const char* acceptExts;   // Comma-separated accepted extensions (lowercase, with dots)
};

// For RetroArch cores: coreSo is set, launchPkg/launchIntent are empty
// For standalone emulators: coreSo is empty, launchPkg+launchIntent are set
// Intent uses {file.uri} as placeholder for the ROM URI
static const SystemDef kXmbSystemDefs[] = {
    {"NES",             "NES",  "nes",          "nestopia_libretro_android.so",               "", "",
     0.89f, 0.00f, 0.06f, ".nes,.fds,.unf,.unif"},
    {"SNES",            "SNES", "snes",         "snes9x_libretro_android.so",                "", "",
     0.48f, 0.49f, 0.49f, ".smc,.sfc,.fig,.swc"},
    {"Game Boy",        "GB",   "gb",           "gambatte_libretro_android.so",              "", "",
     0.61f, 0.73f, 0.06f, ".gb"},
    {"Game Boy Color",  "GBC",  "gbc",          "gambatte_libretro_android.so",              "", "",
     0.42f, 0.25f, 0.63f, ".gbc,.gb"},
    {"Game Boy Advance","GBA",  "gba",          "gpsp_libretro_android.so",                  "", "",
     0.36f, 0.25f, 0.63f, ".gba"},
    {"Nintendo 64",     "N64",  "n64",          "",
     "org.mupen64plusae.v3.fzurita",
     "-n org.mupen64plusae.v3.fzurita/paulscode.android.mupen64plusae.SplashActivity -a android.intent.action.VIEW -d {file.uri}",
     0.00f, 0.60f, 0.00f, ".bin,.n64,.ndd,.u1,.v64,.z64"},
    {"Nintendo DS",     "NDS",  "nds",          "",
     "com.dsemu.drastic",
     "-n com.dsemu.drastic/.DraSticActivity -a android.intent.action.VIEW -d {file.uri} --activity-clear-task --activity-clear-top",
     0.63f, 0.63f, 0.63f, ".nds"},
    {"Genesis",         "GEN",  "genesis",      "genesis_plus_gx_libretro_android.so",       "", "",
     0.00f, 0.38f, 0.66f, ".md,.gen,.smd,.bin"},
    {"Master System",   "SMS",  "mastersystem", "genesis_plus_gx_libretro_android.so",       "", "",
     0.78f, 0.00f, 0.00f, ".sms,.sg"},
    {"Game Gear",       "GG",   "gamegear",     "genesis_plus_gx_libretro_android.so",       "", "",
     0.09f, 0.09f, 0.85f, ".gg"},
    {"PlayStation",     "PSX",  "psx",          "pcsx_rearmed_libretro_android.so",          "", "",
     0.00f, 0.19f, 0.53f, ".cue,.pbp,.chd,.iso,.m3u,.img"},
    {"PSP",             "PSP",  "psp",          "",
     "org.ppsspp.ppsspp",
     "-n org.ppsspp.ppsspp/.PpssppActivity -a android.intent.action.VIEW -d {file.uri} -t application/octet-stream --activity-clear-task --activity-clear-top",
     0.10f, 0.10f, 0.10f, ".iso,.cso,.chd,.pbp"},
    {"Dreamcast",       "DC",   "dreamcast",    "",
     "com.flycast.emulator",
     "-n com.flycast.emulator/com.flycast.emulator.MainActivity -a android.intent.action.VIEW -d {file.uri}",
     1.00f, 0.50f, 0.00f, ".cdi,.gdi,.chd,.cue"},
    {"Neo Geo Pocket",  "NGP",  "ngpc",         "mednafen_ngp_libretro_android.so",          "", "",
     0.50f, 0.50f, 0.50f, ".ngp,.ngc,.npc"},
    {"PICO-8",          "P-8",  "pico8",        "fake08_libretro_android.so",                "", "",
     1.00f, 0.00f, 0.30f, ".p8,.png"},
};
static const int kNumXmbSystemDefs = sizeof(kXmbSystemDefs) / sizeof(kXmbSystemDefs[0]);

// Defined further down (next to the scan code); forward-declared here so
// buildScanCandidates (which precedes it) can use it.
static std::string findCaseInsensitive(const std::string& parent, const std::string& target);

static std::string romDisplayName(const std::string& rom);
static void buildRomDisplayNames(const std::vector<std::string>& roms,
                                 std::vector<std::string>& displayNames);
static void sortRomEntriesByDisplayName(std::vector<std::string>& roms,
                                        std::vector<std::string>& displayNames);

// Font layout constants come from NanoMenuShaders.h (shared with NanoMenu.cpp).

// ---------------------------------------------------------------------------
// XMB System Initialization
// ---------------------------------------------------------------------------

void NanoMenu::initXmbSystems() {
    esdeGamelistFlush();   // edits waiting for the gamelists, before they are read again
    if (mEsdeSystemsAdopted) mEsdeApplyAfterLoad = true;   // the card's systems: then their emulators
    mEsdeSystemsAdopted = false;
    mXmbSystems.clear();
    mSystemsHealChecked = false;
    mSystemsHealNextMs = 0;
    loadHiddenGames();   // before the cached lists below are loaded, so hidden games never show
    loadFavorites();     // and favourites lead them

    // Migrate any legacy cache dir ownership: older nano builds ran as
    // uid graphics and left /data/system/nano_xmb_cache/ as 0700
    // graphics:graphics, which blocks the current root-uid nano from
    // traversing it when DAC_OVERRIDE is absent. Force to root:root with
    // 0755 on the directory and 0644 on contents so subsequent boots
    // work regardless of capability set. No-op if already correct.
    {
        const char* cacheDir = "/data/system/nano_xmb_cache";
        struct stat dst;
        if (stat(cacheDir, &dst) == 0) {
            if (dst.st_uid != 0 || dst.st_gid != 0)
                (void)chown(cacheDir, 0, 0);
            if ((dst.st_mode & 0777) != 0755)
                (void)chmod(cacheDir, 0755);
            DIR* d = opendir(cacheDir);
            if (d) {
                struct dirent* e;
                while ((e = readdir(d)) != nullptr) {
                    if (e->d_name[0] == '.') continue;
                    std::string fp = std::string(cacheDir) + "/" + e->d_name;
                    struct stat fst;
                    if (stat(fp.c_str(), &fst) == 0) {
                        if (fst.st_uid != 0 || fst.st_gid != 0)
                            (void)chown(fp.c_str(), 0, 0);
                        if ((fst.st_mode & 0777) != 0644)
                            (void)chmod(fp.c_str(), 0644);
                    }
                }
                closedir(d);
            }
        }
    }

    // Load the persisted dynamic-systems config from DE storage
    // (/data/system/nano_systems.json), readable at early boot before user
    // unlock. On first run / missing file / parse error, seed it from the
    // built-in defaults so behavior is byte-for-byte unchanged.
    if (!loadSystemsConfig()) {
        seedSystemsConfig();
    }

    for (auto& sys : mXmbSystems) {
        // Legacy per-system prop overrides keyed on the original built-in
        // romDir (== id). Props win, preserving today's precedence even after
        // the JSON config exists. Custom systems have no such props.
        if (sys.builtin) {
            char propBuf[PROPERTY_VALUE_MAX] = {};
            char propKey[160];
            snprintf(propKey, sizeof(propKey),
                     "persist.gammaos.nano.xmb.%s.dir", sys.id.c_str());
            property_get(propKey, propBuf, "");
            if (propBuf[0]) sys.romDir = propBuf;

            snprintf(propKey, sizeof(propKey),
                     "persist.gammaos.nano.xmb.%s.core", sys.id.c_str());
            property_get(propKey, propBuf, "");
            if (propBuf[0]) sys.coreSo = propBuf;
        }
    }
    for (auto& sys : mXmbSystems) {
        // Disabled systems are never scanned and never appear in Game; they
        // hold no ROM cache in memory.
        if (!sys.enabled) continue;

        // Load this system's cached ROM list (DE storage, keyed on stable id).
        loadRomCacheForSystem(sys);
    }
    if (esdeStoreMode()) {   // the gamelists name games by their system folders: known from the lists now
        loadHiddenGames();
        loadFavorites();
        mRomNameOverrideLoaded = true;
        loadRomNameOverrides();
        for (auto& sys : mXmbSystems) if (sys.enabled) applyRomNameOverrides(sys);
    }

    int enabledCount = 0;
    for (const auto& s : mXmbSystems) if (s.enabled) enabledCount++;
    ALOGD("NanoMenu: initialized %d XMB systems (%d enabled)",
          (int)mXmbSystems.size(), enabledCount);

    // If all enabled systems loaded from cache, mark scan as done. Disabled
    // systems are excluded from the gate (they are never scanned).
    bool allCached = true;
    for (const auto& s : mXmbSystems) {
        if (s.enabled && !s.scanned) { allCached = false; break; }
    }
    if (allCached) mXmbRomScanDone = true;
}

// Load a system's cached ROM list from DE storage, keyed on the stable id
// (built-in id == romDir so existing {romDir}.list files are reused). Cache
// format: each line is a full ROM path. Legacy compat: if line 1 is a directory
// and remaining lines are bare filenames, prepend the directory to each. Clears
// then repopulates roms/displayNames/activePath and marks the system scanned if
// any ROMs were loaded (so early boot does not clear them before the bg rescan).
void NanoMenu::loadRomCacheForSystem(XmbSystem& sys) {
    sys.roms.clear();
    sys.displayNames.clear();
    std::string cachePath = xmbCachePath(sys);
    int cfd = open(cachePath.c_str(), O_RDONLY);
    if (cfd >= 0) {
        struct stat cst;
        if (fstat(cfd, &cst) == 0 && cst.st_size > 0 && cst.st_size < 64 * 1024 * 1024) {
            std::string content(cst.st_size, '\0');
            ssize_t rd = read(cfd, &content[0], cst.st_size);
            if (rd > 0) {
                content.resize(rd);
                std::vector<std::string> lines;
                size_t pos = 0;
                while (pos < content.size()) {
                    size_t eol = content.find('\n', pos);
                    if (eol == std::string::npos) eol = content.size();
                    std::string line = content.substr(pos, eol - pos);
                    pos = eol + 1;
                    if (!line.empty()) lines.push_back(std::move(line));
                }
                // Detect legacy format: first line is a directory, rest bare.
                std::string dirPrefix;
                if (lines.size() >= 2 && lines[0].find('/') != std::string::npos) {
                    bool allBare = true;
                    for (size_t i = 1; i < lines.size(); i++) {
                        if (lines[i].find('/') != std::string::npos) { allBare = false; break; }
                    }
                    if (allBare) {
                        dirPrefix = lines[0];
                        if (!dirPrefix.empty() && dirPrefix.back() == '/') dirPrefix.pop_back();
                        lines.erase(lines.begin());
                        ALOGD("NanoMenu: %s: legacy cache format, dir=%s",
                              sys.name.c_str(), dirPrefix.c_str());
                    }
                }
                for (auto& l : lines) {
                    if (!dirPrefix.empty()) sys.roms.push_back(dirPrefix + "/" + l);
                    else                    sys.roms.push_back(std::move(l));
                }
                if (!sys.roms.empty()) {
                    size_t ls = sys.roms[0].rfind('/');
                    if (ls != std::string::npos) sys.activePath = sys.roms[0].substr(0, ls);
                    sys.pathExists = true;
                }
                buildRomDisplayNames(sys.roms, sys.displayNames);
                if (!sys.roms.empty())
                    ALOGD("NanoMenu: %s: loaded %zu ROMs from cache",
                          sys.name.c_str(), sys.roms.size());
            }
        }
        close(cfd);
    }
    // If cache had ROMs, mark as scanned so early boot does not clear them.
    if (!sys.roms.empty()) sys.scanned = true;
    applyRomNameOverrides(sys);   // patch in any per-game title overrides
}

// Patch display names from the per-game title override sidecar and keep the ROM list
// ordered by those names. Called only from the render thread: scraperEnsureLoaded()
// may lazily read the metadata index. NOT called from the bg-scan worker.
void NanoMenu::applyRomNameOverrides(std::vector<std::string>& roms,
                                     std::vector<std::string>& displayNames) {
    if (roms.empty()) return;
    // Hidden games leave the list here, the one step every scan, cache load and rename goes
    // through, so nothing that reads a system's list can show them (labels stay paired).
    if (!mHiddenGames.empty()) {
        const bool paired = displayNames.size() == roms.size();
        size_t w = 0;
        for (size_t i = 0; i < roms.size(); i++) {
            if (mHiddenGames.count(hiddenKey(roms[i]))) continue;
            if (w != i) {
                roms[w] = std::move(roms[i]);
                if (paired) displayNames[w] = std::move(displayNames[i]);
            }
            w++;
        }
        roms.resize(w);
        if (paired) displayNames.resize(w);
        else displayNames.clear();   // rebuilt below
        if (roms.empty()) { displayNames.clear(); return; }
    }
    // Display Name view OFF: show and order the list by the raw ROM file name. Rebuild the labels
    // from the file names (dropping any previously-applied scraped/renamed title) and sort by them,
    // which is exactly an order-by-file-name (the labels are the basenames). See mShowDisplayNames.
    if (!mShowDisplayNames) {
        buildRomDisplayNames(roms, displayNames);
        sortRomEntriesByDisplayName(roms, displayNames);
        favoritesFirst(roms, displayNames);
        return;
    }
    if (displayNames.empty()) buildRomDisplayNames(roms, displayNames);
    size_t n = roms.size() < displayNames.size() ? roms.size() : displayNames.size();
    for (size_t i = 0; i < n; i++) {
        // Priority: a manual Rename/Edit Title wins; otherwise, once a game has been scraped, show its
        // matched title instead of the ROM filename; otherwise keep the scanned basename. Games with
        // no Display Name therefore keep the file name and sort alongside the named ones below.
        const std::string* ov = romNameOverrideFor(roms[i]);
        if (ov && !ov->empty()) { displayNames[i] = *ov; continue; }
        if (!mEsdeGlNames.empty()) {   // the ES-DE folder's title (scraped or typed in ES-DE)
            auto gi = mEsdeGlNames.find(esdeKey(roms[i]));
            if (gi != mEsdeGlNames.end()) { displayNames[i] = gi->second; continue; }
        }
        const ScrapeEntry* se = scrapeEntryFor(roms[i]);
        if (se && !se->title.empty()) { displayNames[i] = se->title; continue; }
        // DSi theme: the title from the DS cartridge's own banner, when the user has not
        // named the game and it has not been scraped (persist.gammaos.nano.nds.romtitle).
        if (mNdsTheme && ndsRomTitleEnabled()) {
            const std::string bt = ndsBannerTitleFor(roms[i]);
            if (!bt.empty()) displayNames[i] = bt;
        }
    }
    sortRomEntriesByDisplayName(roms, displayNames);
    favoritesFirst(roms, displayNames);
}

// Favourites lead the list, each group keeping its name order. The ES-DE theme (and its settings
// in the Minima look) also counts the favourites imported from ES-DE's gamelist, as esdeIsFavorite.
void NanoMenu::favoritesFirst(std::vector<std::string>& roms, std::vector<std::string>& displayNames) {
    if (roms.size() != displayNames.size()) return;
    const bool esde = (mEsdeTheme || mEsdeSettingsMinima) && !mEsdeGamelistMeta.empty();
    if (mXmbFavorites.empty() && !esde) return;
    const std::unordered_set<std::string>& favs = mXmbFavoriteKeys;
    const std::unordered_set<std::string> unfavs(mEsdeUnfavorites.begin(), mEsdeUnfavorites.end());
    std::vector<size_t> order, rest;
    for (size_t i = 0; i < roms.size(); i++) {
        bool fav = !favs.empty() && favs.count(hiddenKey(roms[i])) > 0;
        if (!fav && esde) {
            auto it = mEsdeGamelistMeta.find(roms[i]);
            fav = it != mEsdeGamelistMeta.end() && it->second.favorite && !unfavs.count(roms[i]);
        }
        (fav ? order : rest).push_back(i);
    }
    if (order.empty() || rest.empty()) return;
    order.insert(order.end(), rest.begin(), rest.end());
    std::vector<std::string> r2, d2;
    r2.reserve(roms.size()); d2.reserve(roms.size());
    for (size_t i : order) { r2.push_back(std::move(roms[i])); d2.push_back(std::move(displayNames[i])); }
    roms.swap(r2); displayNames.swap(d2);
}

// Re-order a system after one of its games is starred or un-starred, keeping the open lists, the
// ES-DE gamelist and an open option menu on the same game.
void NanoMenu::favoritesResort(const std::string& romPath) {
    int s = -1, r = -1;
    if (collectionResolveRom(romPath, &s, &r)) favoritesResort(s, std::string(mXmbSystems[s].roms[r]));
}

// keepTop: a gamelist loaded for the first time stays at its top.
void NanoMenu::favoritesResort(int owner, std::string keepRom, bool keepTop) {
    if (owner < 0 || owner >= (int)mXmbSystems.size()) return;
    const auto& roms = mXmbSystems[owner].roms;
    if (keepRom.empty()) {
        for (const auto& lvl : mPs3Stack) {
            if (lvl.sysIdx != owner || lvl.sel < 0 || lvl.sel >= (int)lvl.items.size()) continue;
            const Ps3Item& it = lvl.items[lvl.sel];
            if (it.kind == PS3_ROM && it.b >= 0 && it.b < (int)roms.size()) keepRom = roms[it.b];
        }
    }
    const bool esdeHere = mEsdeSysSel >= 0 && mEsdeSysSel < (int)mEsdeSysList.size() && mEsdeSysList[mEsdeSysSel] == owner;
    const std::string esdeRom = (esdeHere && mEsdeGameSel >= 0 && mEsdeGameSel < (int)roms.size()) ? roms[mEsdeGameSel] : std::string();
    const std::string ctxRom = (mPs3OptCtxKind == PS3_ROM && mPs3OptCtxA == owner && mPs3OptCtxB >= 0 && mPs3OptCtxB < (int)roms.size())
                               ? roms[mPs3OptCtxB] : std::string();
    // Each open level's focused game, while the indexes still point at it.
    auto romAt = [this](const Ps3Item& it) {
        return it.kind == PS3_ROM && it.a >= 0 && it.a < (int)mXmbSystems.size() && it.b >= 0
                       && it.b < (int)mXmbSystems[it.a].roms.size()
                   ? mXmbSystems[it.a].roms[it.b] : std::string();
    };
    std::vector<std::string> selBefore;
    for (const auto& lvl : mPs3Stack)
        selBefore.push_back(lvl.sel >= 0 && lvl.sel < (int)lvl.items.size() ? romAt(lvl.items[lvl.sel]) : std::string());
    const std::vector<std::string> romsBefore = roms;
    applyRomNameOverrides(mXmbSystems[owner]);
    // Search results point in by index too: old index -> new index.
    if (!mGSearchResults.empty() || !mSearchResults.empty()) {
        std::unordered_map<std::string, int> now;
        for (size_t i = 0; i < roms.size(); i++) now.emplace(roms[i], (int)i);
        auto remap = [&](int& idx) {
            if (idx < 0 || idx >= (int)romsBefore.size()) return;
            auto it = now.find(romsBefore[idx]);
            if (it != now.end()) idx = it->second;
        };
        for (auto& gr : mGSearchResults) if (gr.kind == PS3_ROM && gr.a == owner) remap(gr.b);
        for (auto& sr : mSearchResults) if (sr.sysIdx == owner) remap(sr.gameIdx);
    }
    rebuildOpenRomLevels(owner, keepRom);
    if (!esdeRom.empty() && !(keepTop && mEsdeGameSel == 0)) {
        auto ri = std::find(roms.begin(), roms.end(), esdeRom);
        if (ri != roms.end()) mEsdeGameSel = (int)(ri - roms.begin());
    }
    // The lists that point into the system by index: collections, Favourites, Recently Played.
    for (size_t li = 0; li < mPs3Stack.size(); li++) {
        Ps3Level& lvl = mPs3Stack[li];
        const std::string& keepPath = selBefore[li];
        const int keep = lvl.sel;
        if (lvl.collectionIdx >= 0) buildCollectionSubmenu(lvl.collectionIdx, lvl);
        else if (lvl.title == "Favorites") buildFavoritesSubmenu(lvl);
        else if (!lvl.items.empty() && lvl.items[0].kind == PS3_RECENT) buildRecentSubmenu(lvl);
        else continue;
        const int n = (int)lvl.items.size();
        lvl.sel = keep >= n ? n - 1 : keep;
        if (lvl.sel < 0) lvl.sel = 0;
        for (int i = 0; i < n && !keepPath.empty(); i++)
            if (romAt(lvl.items[i]) == keepPath) { lvl.sel = i; break; }
    }
    if (!ctxRom.empty()) {
        auto ri = std::find(roms.begin(), roms.end(), ctxRom);
        if (ri != roms.end()) mPs3OptCtxB = (int)(ri - roms.begin());
    }
    mPs3CatsStale = true;
    mDisplayDirty = true;
}

void NanoMenu::applyRomNameOverrides(XmbSystem& sys) {
    // DSi theme: queue the DS system's banners (async); ndsBannerTick re-applies the names
    // once they land. Only the DS system: a zip elsewhere is never a DS ROM.
    {
        std::string dir = sys.romDir; for (auto& c : dir) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (dir == "nds" || sys.shortname == "NDS") ndsBannerPrefetch(sys.roms);
    }
    applyRomNameOverrides(sys.roms, sys.displayNames);
}

// Patch the Recently Played list's display names from the override sidecar. Recent
// entries persist the name captured at launch time, so a game renamed afterwards would
// keep the old name in Recently Played + search until it is played again; this re-derives
// them from the override map. Called at load and after a rename.
void NanoMenu::applyRomNameOverridesToRecents() {
    for (auto& e : mXmbRecent) {
        // Match the game list: with Display Names off, Recently Played shows the raw file name too.
        if (!mShowDisplayNames) { e.displayName = romDisplayName(e.romPath); continue; }
        const std::string* ov = romNameOverrideFor(e.romPath);
        if (ov && !ov->empty()) { e.displayName = *ov; continue; }
        if (!mEsdeGlNames.empty()) {
            auto gi = mEsdeGlNames.find(esdeKey(e.romPath));
            if (gi != mEsdeGlNames.end()) { e.displayName = gi->second; continue; }
        }
        const ScrapeEntry* se = scrapeEntryFor(e.romPath);
        if (se && !se->title.empty()) { e.displayName = se->title; continue; }
        if (mNdsTheme && ndsRomTitleEnabled() && e.systemName == "NDS") {
            const std::string bt = ndsBannerTitleFor(e.romPath);
            if (!bt.empty()) e.displayName = bt;
        }
    }
}

// ---------------------------------------------------------------------------
// Dynamic systems config (/data/system/nano_systems.json) - see the
// dynamic-game-systems plan. DE storage, JSON schema mirrors Daijishou.
// ---------------------------------------------------------------------------

// Split a comma-separated extension list ("\.nes,.fds") into a JSON array.
static njson::Value extsToJsonArray(const std::string& extStr) {
    njson::Value a = njson::Value::makeArray();
    size_t pos = 0;
    while (pos < extStr.size()) {
        size_t comma = extStr.find(',', pos);
        if (comma == std::string::npos) comma = extStr.size();
        std::string ext = extStr.substr(pos, comma - pos);
        while (!ext.empty() && ext[0] == ' ') ext.erase(0, 1);
        while (!ext.empty() && ext.back() == ' ') ext.pop_back();
        if (!ext.empty()) a.arr.push_back(njson::Value::makeString(ext));
        pos = comma + 1;
    }
    return a;
}

// Join a JSON extensions array back into the comma-separated runtime form.
static std::string jsonArrayToExts(const njson::Value& a) {
    std::string out;
    if (!a.isArray()) return out;
    for (const auto& e : a.arr) {
        if (!e.isString() || e.str.empty()) continue;
        if (!out.empty()) out += ",";
        out += e.str;
    }
    return out;
}

static const char* launchTypeToStr(int lt) {
    switch (lt) {
        case NanoMenu::XLT_RETROARCH_INTENT: return "retroarch-intent";
        case NanoMenu::XLT_CUSTOM_PACKAGE:   return "custom-package";
        case NanoMenu::XLT_LIBRETRO_CORE:
        default:                             return "libretro-core";
    }
}

static int launchTypeFromStr(const std::string& s) {
    if (s == "custom-package")   return NanoMenu::XLT_CUSTOM_PACKAGE;
    if (s == "retroarch-intent") return NanoMenu::XLT_RETROARCH_INTENT;
    return NanoMenu::XLT_LIBRETRO_CORE;
}

// Serialize one runtime system to its JSON object form.
static njson::Value systemToJson(const NanoMenu::XmbSystem& sys) {
    njson::Value o = njson::Value::makeObject();
    o.set("id") = njson::Value::makeString(sys.id);
    o.set("builtin") = njson::Value::makeBool(sys.builtin);
    o.set("enabled") = njson::Value::makeBool(sys.enabled);
    o.set("order") = njson::Value::makeNumber(sys.order);
    o.set("displayName") = njson::Value::makeString(sys.name);
    o.set("shortname") = njson::Value::makeString(sys.shortname);
    o.set("romDir") = njson::Value::makeString(sys.romDir);
    o.set("launchType") = njson::Value::makeString(launchTypeToStr(sys.launchType));
    o.set("coreSo") = njson::Value::makeString(sys.coreSo);
    o.set("package") = njson::Value::makeString(sys.packageName);
    o.set("launchArgs") = njson::Value::makeString(sys.launchArgs);
    o.set("intentTemplate") = njson::Value::makeString(sys.launchIntent);
    njson::Value srcs = njson::Value::makeArray();
    for (const auto& s : sys.scanSources) {
        njson::Value sv = njson::Value::makeObject();
        sv.set("type") = njson::Value::makeString(s.type == 1 ? "safuri" : "rawpath");
        sv.set("value") = njson::Value::makeString(s.value);
        if (!s.rawHint.empty()) sv.set("rawHint") = njson::Value::makeString(s.rawHint);
        if (s.subfolders >= 0) sv.set("subfolders") = njson::Value::makeBool(s.subfolders == 1);
        srcs.arr.push_back(std::move(sv));
    }
    o.set("scanSources") = std::move(srcs);
    if (!sys.disabledDefaultFolders.empty()) {
        njson::Value dd = njson::Value::makeArray();
        for (const auto& d : sys.disabledDefaultFolders) dd.arr.push_back(njson::Value::makeString(d));
        o.set("disabledDefaultFolders") = std::move(dd);
    }
    o.set("extensions") = extsToJsonArray(sys.acceptExts);
    njson::Value icon = njson::Value::makeObject();
    icon.set("ref") = njson::Value::makeString(sys.iconRef);
    icon.set("tintR") = njson::Value::makeNumber(sys.iconR);
    icon.set("tintG") = njson::Value::makeNumber(sys.iconG);
    icon.set("tintB") = njson::Value::makeNumber(sys.iconB);
    o.set("icon") = std::move(icon);
    // Boxart/cover scraper per-system overrides. Only emit the sub-object when at
    // least one field is set, so untouched systems keep a clean config file.
    if (!sys.scraperOverride.empty() || !sys.scrapeUser.empty() || !sys.scrapePass.empty()
        || !sys.scrapeKey.empty() || !sys.scrapePlatform.empty()) {
        njson::Value sc = njson::Value::makeObject();
        if (!sys.scraperOverride.empty()) sc.set("override")  = njson::Value::makeString(sys.scraperOverride);
        if (!sys.scrapeUser.empty())      sc.set("user")      = njson::Value::makeString(sys.scrapeUser);
        if (!sys.scrapePass.empty())      sc.set("pass")      = njson::Value::makeString(sys.scrapePass);
        if (!sys.scrapeKey.empty())       sc.set("key")       = njson::Value::makeString(sys.scrapeKey);
        if (!sys.scrapePlatform.empty())  sc.set("platform")  = njson::Value::makeString(sys.scrapePlatform);
        o.set("scraper") = std::move(sc);
    }
    return o;
}

// Construct a runtime system from its JSON object form, deriving the legacy
// backward-compat fields (launchPkg from package for custom-package, etc.).
static NanoMenu::XmbSystem jsonToSystem(const njson::Value& o) {
    NanoMenu::XmbSystem sys;
    sys.id = o.getString("id");
    sys.builtin = o.getBool("builtin", false);
    sys.enabled = o.getBool("enabled", true);
    sys.order = o.getInt("order", 0);
    sys.name = o.getString("displayName");
    sys.shortname = o.getString("shortname");
    sys.romDir = o.getString("romDir", sys.id);
    sys.launchType = launchTypeFromStr(o.getString("launchType", "libretro-core"));
    sys.coreSo = o.getString("coreSo");
    sys.packageName = o.getString("package");
    sys.launchArgs = o.getString("launchArgs");
    sys.launchIntent = o.getString("intentTemplate");
    if (const njson::Value* srcs = o.find("scanSources")) {
        if (srcs->isArray()) {
            for (const auto& sv : srcs->arr) {
                if (!sv.isObject()) continue;
                NanoMenu::ScanSource s;
                s.type = (sv.getString("type") == "safuri") ? 1 : 0;
                s.value = sv.getString("value");
                s.rawHint = sv.getString("rawHint");
                if (sv.find("subfolders")) s.subfolders = sv.getBool("subfolders") ? 1 : 0;
                if (!s.value.empty()) sys.scanSources.push_back(std::move(s));
            }
        }
    }
    sys.disabledDefaultFolders.clear();
    if (const njson::Value* dd = o.find("disabledDefaultFolders")) {
        if (dd->isArray())
            for (const auto& v : dd->arr)
                if (v.isString() && !v.str.empty()) sys.disabledDefaultFolders.push_back(v.str);
    }
    if (const njson::Value* exts = o.find("extensions"))
        sys.acceptExts = jsonArrayToExts(*exts);
    if (const njson::Value* icon = o.find("icon")) {
        sys.iconRef = icon->getString("ref");
        // Tints are fractional, so read them as doubles (getInt would truncate).
        const njson::Value* tr = icon->find("tintR");
        const njson::Value* tg = icon->find("tintG");
        const njson::Value* tb = icon->find("tintB");
        if (tr) sys.iconR = (float)tr->asNumber(sys.iconR);
        if (tg) sys.iconG = (float)tg->asNumber(sys.iconG);
        if (tb) sys.iconB = (float)tb->asNumber(sys.iconB);
    }
    if (const njson::Value* sc = o.find("scraper")) {
        sys.scraperOverride = sc->getString("override");
        sys.scrapeUser      = sc->getString("user");
        sys.scrapePass      = sc->getString("pass");
        sys.scrapeKey       = sc->getString("key");
        sys.scrapePlatform  = sc->getString("platform");
    }
    // Derive legacy backward-compat fields the renderer/launch path read.
    if (sys.launchType == NanoMenu::XLT_CUSTOM_PACKAGE) {
        sys.launchPkg = sys.packageName;
    } else {
        sys.launchPkg.clear();  // libretro / retroarch-intent are not standalone
    }
    return sys;
}

// The systems of a parsed nano_systems.json in their Game category order; false when it has none.
static bool systemsFromJson(const njson::Value& root, std::vector<NanoMenu::XmbSystem>* out) {
    const njson::Value* systems = root.find("systems");
    if (!systems || !systems->isArray() || systems->arr.empty()) return false;
    std::vector<NanoMenu::XmbSystem> list;
    list.reserve(systems->arr.size());
    for (const auto& sv : systems->arr) {
        if (!sv.isObject()) continue;
        NanoMenu::XmbSystem sys = jsonToSystem(sv);
        if (sys.id.empty()) continue;
        list.push_back(std::move(sys));
    }
    if (list.empty()) return false;
    std::stable_sort(list.begin(), list.end(),
                     [](const NanoMenu::XmbSystem& a, const NanoMenu::XmbSystem& b) { return a.order < b.order; });
    *out = std::move(list);
    return true;
}

// The build's systems (nanosys::kBundledSystems); false when it has none.
static bool loadBundledSystems(std::vector<NanoMenu::XmbSystem>* out) {
    std::string text;
    const int fd = open(nanosys::kBundledSystems, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 4 * 1024 * 1024) {
        text.resize((size_t)st.st_size);
        const ssize_t rd = read(fd, &text[0], text.size());
        text.resize(rd > 0 ? (size_t)rd : 0);
    }
    close(fd);
    njson::Value root;
    if (!njson::parse(text, &root) || !systemsFromJson(root, out)) {
        ALOGW("NanoMenu: %s unreadable", nanosys::kBundledSystems);
        return false;
    }
    return true;
}

// Nanosecond mtime stamp of nano_systems.json, or -1 when absent. The resident
// overlay and the DRM home are separate processes sharing the file; each tracks
// the stamp of its own last load/save (mSystemsCfgStamp) and reloads when an
// external writer moves it.
int64_t NanoMenu::systemsConfigStamp() const {
    struct stat st;
    if (stat("/data/system/nano_systems.json", &st) != 0) return -1;
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

bool NanoMenu::loadSystemsConfig() {
    const char* path = "/data/system/nano_systems.json";
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    std::string content;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 64 * 1024 * 1024) {
        content.resize(st.st_size);
        ssize_t rd = read(fd, &content[0], st.st_size);
        if (rd > 0) content.resize(rd); else content.clear();
    }
    close(fd);
    if (content.empty()) return false;

    njson::Value root;
    if (!njson::parse(content, &root) || !root.isObject()) {
        ALOGW("NanoMenu: nano_systems.json parse failed; reseeding from defaults");
        return false;
    }
    if (!systemsFromJson(root, &mXmbSystems)) return false;
    // Kept once this process has healed: a save from the other process may predate its heal.
    if (root.getBool("healed")) mSystemsHealDone = true;
    if (root.getInt("healVersion") >= 2) mSplitHealDone = true;
    ALOGD("NanoMenu: loaded %zu systems from nano_systems.json", mXmbSystems.size());
    mSystemsCfgStamp = systemsConfigStamp();
    return true;
}

void NanoMenu::seedSystemsConfig() {
    // The build's own systems when it has them: its cores are the ones setup installs, and setup
    // replaces this seed with the same file. Legacy prop overrides are applied later in
    // initXmbSystems (props win), so the on-disk seed holds factory defaults. Either seed is the
    // build's own: nothing to heal.
    mSystemsHealDone = true;
    if (loadBundledSystems(&mXmbSystems)) {
        ALOGI("NanoMenu: seeding nano_systems.json from %s (%zu systems)", nanosys::kBundledSystems,
              mXmbSystems.size());
        saveSystemsConfig();
        return;
    }
    mXmbSystems.clear();
    mXmbSystems.reserve(kNumXmbSystemDefs);
    for (int i = 0; i < kNumXmbSystemDefs; i++) {
        XmbSystem sys;
        sys.id = kXmbSystemDefs[i].romDir;
        sys.builtin = true;
        sys.enabled = true;
        sys.order = i;
        sys.name = kXmbSystemDefs[i].name;
        sys.shortname = kXmbSystemDefs[i].shortname;
        sys.romDir = kXmbSystemDefs[i].romDir;
        sys.coreSo = kXmbSystemDefs[i].coreSo;
        sys.launchPkg = kXmbSystemDefs[i].launchPkg;
        sys.launchIntent = kXmbSystemDefs[i].launchIntent;
        sys.launchType = (kXmbSystemDefs[i].launchPkg[0] != '\0')
                             ? XLT_CUSTOM_PACKAGE : XLT_LIBRETRO_CORE;
        sys.packageName = kXmbSystemDefs[i].launchPkg;
        // Default tint is white so the console glyphs keep the clean PS3 glass
        // look; the Icon Tint editor lets users colour individual systems.
        sys.iconR = sys.iconG = sys.iconB = 1.0f;
        sys.acceptExts = kXmbSystemDefs[i].acceptExts;
        char ref[24];
        snprintf(ref, sizeof(ref), "builtin:%d", i);
        sys.iconRef = ref;
        mXmbSystems.push_back(std::move(sys));
    }
    ALOGI("NanoMenu: seeding nano_systems.json from %d built-in defaults",
          kNumXmbSystemDefs);
    saveSystemsConfig();
}

void NanoMenu::saveSystemsConfig(bool quiet) {
    if (mEsdeSystemsAdopted) {   // the card's systems wait to be loaded: these would replace them
        ALOGW("NanoMenu: systems not saved; the ES-DE folder's are being loaded");
        if (!quiet) photoShowBanner(trDyn("The microSD card's systems are loading. Try again in a moment."));
        return;
    }
    if (!writeSystemsConfig(mXmbSystems)) return;
    // Track our own write so the cross-process change poll does not see this
    // process's saves as an external edit.
    mSystemsCfgStamp = systemsConfigStamp();
    hakuxFoldersFollow();
    esdeStoreSaveSystems();
}

bool NanoMenu::writeSystemsConfig(const std::vector<XmbSystem>& list) {
    njson::Value root = njson::Value::makeObject();
    root.set("version") = njson::Value::makeNumber(1);
    njson::Value systems = njson::Value::makeArray();
    for (const auto& sys : list)
        systems.arr.push_back(systemToJson(sys));
    root.set("systems") = std::move(systems);
    if (mSystemsHealDone) root.set("healed") = njson::Value::makeBool(true);
    if (mSplitHealDone) root.set("healVersion") = njson::Value::makeNumber(2);
    std::string text = njson::serialize(root, true);

    // Atomic write: temp file, fsync, rename over the target; re-own root:root
    // 0644 (nano holds CHOWN/FOWNER), matching the existing cache hygiene. The
    // temp file is this process's: the overlay and the home may save at once.
    const char* path = "/data/system/nano_systems.json";
    const std::string tmpName = std::string(path) + ".tmp." + std::to_string(getpid());
    const char* tmp = tmpName.c_str();
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        ALOGW("NanoMenu: cannot write %s (errno %d)", tmp, errno);
        return false;
    }
    size_t off = 0;
    bool ok = true;
    while (off < text.size()) {
        ssize_t w = write(fd, text.c_str() + off, text.size() - off);
        if (w <= 0) { ok = false; break; }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (!ok) { unlink(tmp); ALOGW("NanoMenu: write of nano_systems.json failed"); return false; }
    if (rename(tmp, path) != 0) {
        ALOGW("NanoMenu: rename of nano_systems.json failed (errno %d)", errno);
        unlink(tmp);
        return false;
    }
    (void)chown(path, 0, 0);
    (void)chmod(path, 0644);
    ALOGD("NanoMenu: wrote %s (%zu systems, %zu bytes)", path, list.size(), text.size());
    return true;
}

// ---- The systems in the ES-DE folder (nano/systems.json) ----
// Nano's systems without the scraper accounts (never on the card), the card's volume written
// %CARDID% so another card laid out the same fits. The card's copy wins over Nano's unless only
// Nano's changed since the two were last the same (kEsdeSystemsSync holds that copy's hash).

static const char* const kEsdeSystemsSync = "/data/system/nano_esde_systems.sync";

static uint64_t esdeTextHash(const std::string& t) {   // FNV-1a: the same in every build
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : t) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

static uint64_t esdeSyncedHash() {
    char b[32] = {};
    FILE* f = fopen(kEsdeSystemsSync, "re");
    if (!f) return 0;
    const bool got = fgets(b, sizeof(b), f) != nullptr;
    fclose(f);
    return got ? strtoull(b, nullptr, 16) : 0;
}

static void esdeSetSyncedHash(uint64_t h) {
    const std::string tmp = std::string(kEsdeSystemsSync) + ".tmp." + std::to_string(getpid());
    FILE* f = fopen(tmp.c_str(), "we");
    if (!f) return;
    fprintf(f, "%016llx\n", (unsigned long long)h);
    if (fclose(f) != 0 || rename(tmp.c_str(), kEsdeSystemsSync) != 0) unlink(tmp.c_str());
}

// A RetroArch core's file name, nothing else (it is joined to the cores folder).
static bool esdeCoreNameOk(const std::string& so) {
    static const std::string kSuffix = "_libretro_android.so";
    if (so.size() <= kSuffix.size() || so.compare(so.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) return false;
    for (char c : so)
        if (!isalnum((unsigned char)c) && !strchr("_.+-", c)) return false;
    return so.find("..") == std::string::npos;
}

static std::string esdeCardId(const std::string& esdeDir) {
    if (esdeDir.compare(0, 9, "/storage/") != 0) return std::string();
    const std::string id = esdeDir.substr(9, esdeDir.find('/', 9) == std::string::npos ? std::string::npos
                                                                                       : esdeDir.find('/', 9) - 9);
    return id == "emulated" || id == "self" ? std::string() : id;
}

static void esdeReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
}

static const char* const kEsdeKeptSystems = "/data/system/nano_esde_kept_systems.json";

void NanoMenu::esdeKeptSystemsSave() {
    njson::Value root = njson::Value::makeObject();
    root.set("systems") = mEsdeKeptSystems;
    root.set("for") = njson::Value::makeString(mEsdeKeptFor);
    const std::string text = njson::serialize(root, true), tmp = std::string(kEsdeKeptSystems) + ".tmp." + std::to_string(getpid());
    FILE* f = fopen(tmp.c_str(), "we");
    const bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f && fclose(f) != 0) unlink(tmp.c_str());
    else if (!ok || rename(tmp.c_str(), kEsdeKeptSystems) != 0) unlink(tmp.c_str());
    mEsdeKeptLoaded = true;
}

std::string NanoMenu::esdeSystemsJson() {
    if (!mEsdeKeptLoaded) {   // the card's systems Nano left out (esdeStoreAdoptSystems), from before a restart
        mEsdeKeptLoaded = true;
        mEsdeKeptSystems = njson::Value::makeArray();
        std::string text;
        if (FILE* f = fopen(kEsdeKeptSystems, "re")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        njson::Value root;
        if (njson::parse(text, &root) && root.isObject()) {
            if (const njson::Value* a = root.find("systems")) if (a->isArray()) mEsdeKeptSystems = *a;
            mEsdeKeptFor = root.getString("for");
        }
    }
    const std::string id = esdeCardId(esdeDataDir());
    njson::Value root = njson::Value::makeObject();
    root.set("version") = njson::Value::makeNumber(1);
    njson::Value systems = njson::Value::makeArray();
    for (XmbSystem sys : mXmbSystems) {
        sys.scrapeUser.clear();
        sys.scrapePass.clear();
        sys.scrapeKey.clear();
        for (auto& src : sys.scanSources) {
            esdeReplaceAll(src.value, id, "%CARDID%");
            esdeReplaceAll(src.rawHint, id, "%CARDID%");
        }
        systems.arr.push_back(systemToJson(sys));
    }
    if (mEsdeKeptFor == esdeDataDir()) for (const auto& o : mEsdeKeptSystems.arr) systems.arr.push_back(o);
    root.set("systems") = std::move(systems);
    return njson::serialize(root, true);
}

// After a save: the card gets Nano's systems, unless its copy changed elsewhere (read at the next
// esdeStoreAdoptSystems instead).
void NanoMenu::esdeStoreSaveSystems() {
    // Not while the card's systems wait to be loaded, or the other Nano process saved newer ones.
    if (!esdeStoreReady() || mEsdeSystemsAdopted || systemsConfigStamp() != mSystemsCfgStamp) return;
    const std::string path = esdeDataDir() + "/nano/systems.json";
    const std::string mine = esdeSystemsJson();
    std::string card;
    bool push = true;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[65536];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) card.append(buf, (size_t)n);
        const bool readOk = n == 0;
        close(fd);
        // Changed elsewhere since: the card's are read instead (esdeStoreAdoptSystems, next tick).
        if (!readOk || (card != mine && esdeTextHash(card) != esdeSyncedHash())) { push = false; mEsdeAdoptLater = true; }
    } else if (errno != ENOENT) {
        push = false;
    }
    if (push && card != mine) {
        const std::string tmp = path + ".nanotmp";
        mkdir((esdeDataDir() + "/nano").c_str(), 0775);
        FILE* f = fopen(tmp.c_str(), "we");
        const bool ok = f && fwrite(mine.data(), 1, mine.size(), f) == mine.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (f) fclose(f);
        if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); ALOGW("esde: writing %s failed", path.c_str()); push = false; }
        else esdeNoteWrite(path);
    }
    if (push) esdeSetSyncedHash(esdeTextHash(mine));
    esdeStoreWriteCustomSystems();
    esdeStoreSaveSystemEmus();
}

// The card's systems, when they are not Nano's: nano_systems.json is written from them (with this
// device's scraper accounts) and the home reloads it as for one written by the other Nano process.
// True when it did.
bool NanoMenu::esdeStoreAdoptSystems() {
    if (!esdeStoreReady() || mEsdeSystemsAdopted || systemsConfigStamp() != mSystemsCfgStamp) return false;
    const std::string path = esdeDataDir() + "/nano/systems.json";
    std::string card;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) esdeStoreSaveSystems();   // none there yet: Nano's go
        return false;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) card.append(buf, (size_t)n);
    close(fd);
    const std::string mine = esdeSystemsJson();
    const uint64_t synced = esdeSyncedHash();
    if (card == mine) {
        if (synced != esdeTextHash(mine)) esdeSetSyncedHash(esdeTextHash(mine));
        return false;
    }
    if (esdeTextHash(card) == synced) { esdeStoreSaveSystems(); return false; }   // only Nano's changed
    std::string text = card;
    const std::string id = esdeCardId(esdeDataDir());
    if (!id.empty()) esdeReplaceAll(text, "%CARDID%", id);
    njson::Value root;
    std::vector<XmbSystem> list;
    if (!njson::parse(text, &root) || !root.isObject() || !systemsFromJson(root, &list)) {
        ALOGW("esde: %s unreadable; Nano's systems kept", path.c_str());
        return false;
    }
    // How a system starts its games is never taken from the card as it is: anything can write there,
    // and Nano starts them as root. Only what this device or Nano's catalog has is taken.
    std::vector<XmbSystem> bundled;
    loadBundledSystems(&bundled);
    loadEmuCatalog();
    auto sameLaunch = [](const XmbSystem& a, const XmbSystem& b) {
        return a.launchType == b.launchType && a.coreSo == b.coreSo && a.packageName == b.packageName
               && a.launchIntent == b.launchIntent && a.launchArgs == b.launchArgs;
    };
    auto known = [&](const XmbSystem& sys, const XmbSystem* cur) {
        if (cur && sameLaunch(sys, *cur)) return true;
        for (const auto& b : bundled) if (b.id == sys.id && sameLaunch(sys, b)) return true;
        if (sys.launchType == XLT_LIBRETRO_CORE && sys.packageName.empty() && sys.launchIntent.empty()
            && sys.launchArgs.empty() && esdeCoreNameOk(sys.coreSo))
            return true;
        for (const auto& e : mEmuCatalog) {
            XmbSystem g;
            applyEmuEntryToSystem(g, e);
            if (sys.launchArgs.empty() && g.launchType == sys.launchType && g.coreSo == sys.coreSo
                && g.packageName == sys.packageName && g.launchIntent == sys.launchIntent)
                return true;
        }
        return false;
    };
    // Names that become paths are single names (no folder of their own); folders are on storage;
    // icons are Nano's own.
    auto plainName = [](const std::string& n) {
        if (n.empty() || n == "." || n == ".." || n.size() > 128) return false;
        for (char c : n) if (c == '/' || c == '\\' || (unsigned char)c < 0x20) return false;
        return true;
    };
    std::vector<XmbSystem> safe;
    std::set<std::string> ids;
    std::map<std::string, int> keep;   // id -> card entries of it Nano does not use (kept on the card)
    for (auto& sys : list) {
        if (!plainName(sys.id) || !plainName(sys.romDir) || !ids.insert(sys.id).second) {
            ALOGW("esde: a system from the card with the name %s is left out", sys.id.c_str());
            keep[sys.id]++;
            continue;
        }
        sys.scanSources.erase(std::remove_if(sys.scanSources.begin(), sys.scanSources.end(),
                                             [&](const ScanSource& src) {
                                                 return !esdeStoragePath(src.value) || (!src.rawHint.empty() && !esdeStoragePath(src.rawHint));
                                             }),
                              sys.scanSources.end());
        const bool iconOk = (sys.iconRef.compare(0, 8, "builtin:") == 0 && sys.iconRef.size() > 8
                             && std::all_of(sys.iconRef.begin() + 8, sys.iconRef.end(), ::isdigit))
                            || (sys.iconRef.compare(0, 10, "retroarch:") == 0 && plainName(sys.iconRef.substr(10)));
        const XmbSystem* cur = nullptr;
        for (const auto& c : mXmbSystems) if (c.id == sys.id) cur = &c;
        if (cur) { sys.scrapeUser = cur->scrapeUser; sys.scrapePass = cur->scrapePass; sys.scrapeKey = cur->scrapeKey; }
        if (!iconOk) sys.iconRef = cur ? cur->iconRef : std::string();
        if (!known(sys, cur)) {
            if (!cur) {
                ALOGW("esde: %s's emulator from the card is not one Nano knows; left out", sys.id.c_str());
                keep[sys.id]++;
                continue;
            }
            ALOGW("esde: %s's emulator from the card is not one Nano knows; this device's kept", sys.id.c_str());
            sys.launchType = cur->launchType; sys.coreSo = cur->coreSo; sys.packageName = cur->packageName;
            sys.launchPkg = cur->launchPkg; sys.launchIntent = cur->launchIntent; sys.launchArgs = cur->launchArgs;
        }
        safe.push_back(std::move(sys));
    }
    // What Nano leaves out stays in the card's copy (esdeSystemsJson writes it back as it was), so
    // the device it came from still has it.
    mEsdeKeptSystems = njson::Value::makeArray();
    mEsdeKeptFor = esdeDataDir();
    njson::Value raw;
    if (!keep.empty() && njson::parse(card, &raw) && raw.isObject())
        if (const njson::Value* arr = raw.find("systems"))
            if (arr->isArray()) {
                std::set<std::string> usedIds, seen;
                for (const auto& s : safe) usedIds.insert(s.id);
                for (const auto& o : arr->arr) {
                    if (!o.isObject()) continue;
                    const std::string oid = o.getString("id");
                    if (!keep.count(oid)) continue;
                    // A duplicate: the first is the one used, the others kept; one not used: all kept.
                    if (usedIds.count(oid) && seen.insert(oid).second) continue;
                    mEsdeKeptSystems.arr.push_back(o);
                }
            }
    esdeKeptSystemsSave();
    if (safe.empty()) return false;
    list.swap(safe);
    if (!writeSystemsConfig(list)) return false;
    mEsdeSystemsAdopted = true;
    esdeSetSyncedHash(esdeTextHash(card));
    ALOGI("esde: the systems from %s (%zu)", path.c_str(), list.size());
    return true;
}

// Through test 27 Nano seeded its own table on its first start, before setup copied the build's
// systems, and could save that table over them (a microSD move during setup adding the card's
// folders, say): NES then named Nestopia, which this build does not ship, and said "emulator not
// installed". Such systems get the build's cores, and the build's systems lost with them come back.
// RetroArch's cores are in credential-encrypted storage, readable only once the user is unlocked.
static nanosys::SysCore systemCore(const NanoMenu::XmbSystem& s) {
    nanosys::SysCore c;
    c.id = s.id;
    c.romDir = s.romDir;
    c.shortname = s.shortname;
    c.builtin = s.builtin;
    for (const auto& src : s.scanSources) {
        const std::string& f = src.type == 0 ? src.value : src.rawHint;
        if (!f.empty()) c.folders.push_back(f);
    }
    c.libretro = s.launchType == NanoMenu::XLT_LIBRETRO_CORE && !s.isStandalone();
    c.coreSo = s.coreSo;
    c.package = s.packageName;
    return c;
}

void NanoMenu::systemsHealTick() {
    splitFoldersHealTick();
    if (mSystemsHealDone || mSystemsHealChecked || mSetupWizardActive || mBgScanThreadRunning) return;
    const int64_t now = elapsedRealtime();
    if (now < mSystemsHealNextMs) return;
    mSystemsHealNextMs = now + 2000;
    // The other process saved since this one loaded: heal what it saved, once reloaded.
    if (systemsConfigStamp() != mSystemsCfgStamp) return;
    struct stat st;
    if (stat("/data/data/com.retroarch.aarch64/cores", &st) != 0 || !S_ISDIR(st.st_mode)) return;
    mSystemsHealChecked = true;
    const auto view = systemCore;
    auto views = [&view](const std::vector<XmbSystem>& list) {
        std::vector<nanosys::SysCore> out;
        out.reserve(list.size());
        for (const auto& s : list) out.push_back(view(s));
        return out;
    };
    const auto coreInstalled = [this](const std::string& so) { return coreSoExists(so); };
    bool changed = false, recentsChanged = false, rescan = false;
    // This process's Recently Played can be older than the file (the other process launched since).
    bool recentsLoaded = false;
    const auto freshRecents = [this, &recentsLoaded]() -> std::vector<XmbRecentEntry>& {
        if (!recentsLoaded) loadXmbRecent();
        recentsLoaded = true;
        return mXmbRecent;
    };

    std::vector<XmbSystem> bundled;
    if (loadBundledSystems(&bundled)) {
        std::vector<std::pair<std::string, std::string>> nanoCores;
        for (int i = 0; i < kNumXmbSystemDefs; i++)
            if (kXmbSystemDefs[i].coreSo[0]) nanoCores.push_back({kXmbSystemDefs[i].romDir, kXmbSystemDefs[i].coreSo});
        const auto cur = views(mXmbSystems), bun = views(bundled);
        const auto repairs = nanosys::staleCoreRepairs(cur, bun, nanoCores, coreInstalled);
        for (const auto& r : repairs) {
            XmbSystem& s = mXmbSystems[r.index];
            ALOGW("NanoMenu: system %s named Nano's own core %s, which is not installed; now the build's %s",
                  s.id.c_str(), s.coreSo.c_str(), r.coreSo.c_str());
            for (auto& re : freshRecents()) {
                if (re.standalone || re.romDir != s.romDir || re.coreSo != s.coreSo) continue;
                re.coreSo = r.coreSo;
                recentsChanged = true;
            }
            s.coreSo = r.coreSo;
            changed = true;
        }
        if (!repairs.empty()) {
            int maxOrder = -1;
            for (const auto& s : mXmbSystems) maxOrder = std::max(maxOrder, s.order);
            for (size_t i : nanosys::missingBundledSystems(cur, bun)) {
                XmbSystem s = bundled[i];
                s.order = ++maxOrder;
                invalidateSystemScan(s);
                ALOGI("NanoMenu: the build's system %s restored", s.id.c_str());
                mXmbSystems.push_back(std::move(s));
                rescan = true;
            }
        }
    }

    // A system the emulator catalog made before it picked players that run (the microSD move's
    // card folders: WonderSwan got the SwanEmu app, which this build does not ship) gets the first
    // one that does, as the emulator picker lists them.
    bool anyCustom = false;
    for (const auto& s : mXmbSystems) if (!s.builtin) { anyCustom = true; break; }
    // packageInstalled says yes to everything when the package list cannot be read.
    const bool packagesReadable = access("/data/system/packages.list", R_OK) == 0;
    if (anyCustom && packagesReadable) {
        if (mEmuCatalog.empty()) loadEmuCatalog();
        const auto pkgInstalled = [this](const std::string& pkg) { return packageInstalled(pkg); };
        for (auto& s : mXmbSystems) {
            if (s.builtin) continue;
            const std::string uid = emuPlatformFor(s);
            if (uid.empty()) continue;
            std::vector<int> catIdx;
            const auto players = emuPlatformPlayers(uid, &catIdx);
            const int pi = nanosys::playerRepair(view(s), players, coreInstalled, pkgInstalled);
            if (pi < 0) continue;
            const std::string oldCore = s.coreSo, oldPkg = s.launchPkg, oldExts = s.acceptExts;
            const bool wasStandalone = s.isStandalone();
            applyEmuEntryToSystem(s, mEmuCatalog[catIdx[pi]]);
            s.launchArgs.clear();   // the old player's extra arguments
            ALOGW("NanoMenu: system %s started %s, which cannot run here; now %s", s.id.c_str(),
                  wasStandalone ? oldPkg.c_str() : oldCore.c_str(), mEmuCatalog[catIdx[pi]].player.c_str());
            for (auto& re : freshRecents()) {
                if (re.romDir != s.romDir || re.standalone != wasStandalone
                    || (wasStandalone ? re.launchPkg != oldPkg : re.coreSo != oldCore))
                    continue;
                re.coreSo = s.coreSo;
                re.launchPkg = s.launchPkg;
                re.launchIntent = s.launchIntent;
                re.standalone = s.isStandalone();
                recentsChanged = true;
            }
            if (s.acceptExts != oldExts) {
                invalidateSystemScan(s);
                rescan = true;
            }
            changed = true;
        }
    }

    if (!anyCustom || packagesReadable) mSystemsHealDone = true;   // saved with the next save
    if (!changed) return;
    saveSystemsConfig(true);
    if (recentsChanged) saveXmbRecent();
    if (rescan) forceRescanAllSystems();
    mPs3CatsStale = true;
}

static std::string autoAddLower(const std::string& in);
int nano_makeUniqueSystem(std::vector<NanoMenu::XmbSystem>& systems, const std::string& name,
                          const std::string& preferredId);

// Satellaview, Super Game Boy, 64DD, Neo Geo CD and the like have systems of their own now
// (nanosys::splitOffFolders), and the systems that scanned their folders through test 27 no longer
// do. Once per config ("healVersion" 2), each such folder under a ROM root that holds games and that
// no system scans gets its platform's system, as the folder auto-add makes it, so its games stay
// listed. The folder probe runs on a worker (a card or share can be slow); the systems change here.
void NanoMenu::splitFoldersHealTick() {
    if (mSplitHealDone || mSetupWizardActive) return;
    const int state = mSplitHealScan.load(std::memory_order_acquire);
    if (state == 1) return;
    if (state == 0) {
        const int64_t now = elapsedRealtime();
        if (now < mSplitHealNextMs) return;
        mSplitHealNextMs = now + 2000;
        // The emulator choice needs RetroArch's cores (credential-encrypted storage) and the package
        // list, as the other heal does.
        struct stat st;
        if (stat("/data/data/com.retroarch.aarch64/cores", &st) != 0 || !S_ISDIR(st.st_mode)) return;
        if (access("/data/system/packages.list", R_OK) != 0) return;
        // The cores are readable well before the storage mounts at boot, and a probe of the bare
        // mount points would find nothing and end the heal for good.
        if (!isPrimaryStorageReady() || !nanosys::drivesServed("/mnt/media_rw", readSelfMounts())) return;
        if (mEmuCatalog.empty()) loadEmuCatalog();
        mSplitHealIds.clear();
        for (const auto& e : mEmuCatalog) {
            const std::string pid = autoAddLower(e.platformId);
            if (!pid.empty() && std::find(mSplitHealIds.begin(), mSplitHealIds.end(), pid) == mSplitHealIds.end())
                mSplitHealIds.push_back(pid);
        }
        std::map<int, std::string> exts;   // each split-off platform's players' extensions
        for (const auto& f : nanosys::splitOffFolders()) {
            const int pi = nanosys::platformForFolder(f, mSplitHealIds);
            if (pi < 0 || exts.count(pi)) continue;
            std::string all;
            for (const auto& e : mEmuCatalog) {
                if (autoAddLower(e.platformId) != mSplitHealIds[pi]) continue;
                XmbSystem tmp;
                applyEmuEntryToSystem(tmp, e);
                if (!tmp.acceptExts.empty()) all += (all.empty() ? "" : ",") + tmp.acceptExts;
            }
            exts[pi] = all;
        }
        const std::vector<std::string> ids = mSplitHealIds, shares = mountedShareNames();
        mSplitHealScan.store(1, std::memory_order_release);
        std::thread([this, ids, exts, shares]() {
            std::vector<std::string> roots = {"/storage/emulated/0/ROMs"};
            if (DIR* d = opendir("/storage")) {
                for (struct dirent* e; (e = readdir(d)) != nullptr;) {
                    const std::string n = e->d_name;
                    if (n[0] == '.' || n == "emulated" || n == "self"
                        || std::find(shares.begin(), shares.end(), n) != shares.end())
                        continue;
                    const std::string base = "/storage/" + n;
                    const std::string roms = findCaseInsensitive(base, "ROMs");
                    if (!roms.empty()) roots.push_back(roms);
                    roots.push_back(base);
                }
                closedir(d);
            }
            auto found = nanosys::findSplitOffFolders(roots, ids, [&exts](int pi) {
                const auto it = exts.find(pi);
                return it == exts.end() ? std::string() : it->second;
            });
            { std::lock_guard<std::mutex> lk(mSplitHealLock); mSplitHealFound = std::move(found); }
            mSplitHealScan.store(2, std::memory_order_release);
            FrameFlag::wake();
        }).detach();
        return;
    }
    // bgScanThreadFunc reads mXmbSystems unlocked; and a save by the other process since this one
    // loaded is applied first (it may have healed).
    if (mBgScanThreadRunning || systemsConfigStamp() != mSystemsCfgStamp) return;
    std::vector<nanosys::FoundFolder> found;
    { std::lock_guard<std::mutex> lk(mSplitHealLock); found.swap(mSplitHealFound); }
    mSplitHealScan.store(0, std::memory_order_release);
    std::vector<nanosys::SysCore> cur;
    for (const auto& s : mXmbSystems) cur.push_back(systemCore(s));
    int added = 0;
    for (const auto& a : nanosys::splitFolderAdds(cur, found, mSplitHealIds)) {
        int ci = -1;   // the platform's first catalog entry
        for (int i = 0; i < (int)mEmuCatalog.size() && ci < 0; i++)
            if (autoAddLower(mEmuCatalog[i].platformId) == mSplitHealIds[a.platform]) ci = i;
        if (ci < 0) continue;
        const std::string platformId = mEmuCatalog[ci].platformId;
        bool noneReady = true;
        const std::vector<int> ready = emuPlatformChoices(platformId, noneReady);
        const int pick = !noneReady && !ready.empty() ? ready[0] : ci;
        const int idx = nano_makeUniqueSystem(mXmbSystems, mEmuCatalog[ci].platform, platformId);
        XmbSystem& s = mXmbSystems[idx];
        applyEmuEntryToSystem(s, mEmuCatalog[pick]);
        s.iconRef = gsIconRefForPlatform(platformId, mEmuCatalog[ci].platform);
        for (const auto& f : a.folders) {
            ScanSource src; src.type = 0; src.value = f; src.subfolders = 1;
            s.scanSources.push_back(src);
        }
        invalidateSystemScan(s);
        ALOGI("NanoMenu: system %s added for its own folder %s (%zu folders)", s.id.c_str(), a.folders[0].c_str(),
              a.folders.size());
        added++;
    }
    mSplitHealDone = true;
    saveSystemsConfig(true);
    if (added == 0) return;
    forceRescanAllSystems();
    mPs3CatsStale = true;
}

// Restore a built-in system's config to its factory defaults (the build's
// systems, else kXmbSystemDefs, matched by id), keeping its enabled/order/id, and
// rescans it from the default folders. Returns false for custom systems or
// unknown ids. Caller persists + rebuilds.
bool NanoMenu::resetSystemToBuiltinDefaults(int sysIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return false;
    XmbSystem& sys = mXmbSystems[sysIdx];
    if (!sys.builtin) return false;
    std::vector<XmbSystem> bundled;
    if (loadBundledSystems(&bundled)) {
        for (const auto& b : bundled) {
            if (!b.builtin || b.id != sys.id) continue;
            sys.name = b.name;
            sys.shortname = b.shortname;
            sys.romDir = b.romDir;
            sys.coreSo = b.coreSo;
            sys.launchPkg = b.launchPkg;
            sys.launchIntent = b.launchIntent;
            sys.launchType = b.launchType;
            sys.packageName = b.packageName;
            sys.launchArgs = b.launchArgs;
            sys.iconRef = b.iconRef;
            sys.iconR = b.iconR; sys.iconG = b.iconG; sys.iconB = b.iconB;
            sys.acceptExts = b.acceptExts;
            sys.scanSources = b.scanSources;
            sys.disabledDefaultFolders = b.disabledDefaultFolders;
            invalidateSystemScan(sys);
            forceRescanAllSystems();
            ALOGI("ps3menu: reset system %s to the build's defaults", sys.id.c_str());
            return true;
        }
    }
    int idx = -1;
    for (int i = 0; i < kNumXmbSystemDefs; i++) {
        if (sys.id == kXmbSystemDefs[i].romDir) { idx = i; break; }
    }
    if (idx < 0) return false;
    const SystemDef& def = kXmbSystemDefs[idx];
    sys.name = def.name;
    sys.shortname = def.shortname;
    sys.romDir = def.romDir;
    sys.coreSo = def.coreSo;
    sys.launchPkg = def.launchPkg;
    sys.launchIntent = def.launchIntent;
    sys.launchType = (def.launchPkg[0] != '\0') ? XLT_CUSTOM_PACKAGE : XLT_LIBRETRO_CORE;
    sys.packageName = def.launchPkg;
    sys.launchArgs.clear();
    sys.iconR = sys.iconG = sys.iconB = 1.0f;   // white default tint
    sys.acceptExts = def.acceptExts;
    sys.scanSources.clear();
    sys.disabledDefaultFolders.clear();
    char ref[24]; snprintf(ref, sizeof(ref), "builtin:%d", idx); sys.iconRef = ref;
    invalidateSystemScan(sys);
    forceRescanAllSystems();
    ALOGI("ps3menu: reset system %s to built-in defaults", sys.id.c_str());
    return true;
}

// The system's romDir plus the other folder names of its group (nanosys::romFolderAliases).
static std::vector<std::string> getRomFolderAliases(const std::string& romDir) {
    return nanosys::romFolderAliases(romDir);
}

// ---- ES-DE-style bulk auto-add (scan a ROMs root, add every recognised system folder) ----
// Defined in NanoMenuPS3Folder.cpp.
int nano_makeUniqueSystem(std::vector<NanoMenu::XmbSystem>& systems, const std::string& name,
                          const std::string& preferredId);

static std::string autoAddLower(const std::string& in) {
    std::string o; o.reserve(in.size());
    for (char c : in) o += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return o;
}

// Broad ROM-extension set used only for the bulk-add "does this folder hold ROMs" gate on the worker
// thread (where the specific system's exts are not yet known). Deliberately lenient: the alias match
// already excludes non-system folders, so this only needs to reject a matched-but-empty folder.
static const char* kBulkAddGenericExts =
    ".nes,.fds,.unf,.unif,.sfc,.smc,.fig,.swc,.bs,.gb,.gbc,.gba,.agb,.n64,.z64,.v64,.ndd,.nds,.dsi,"
    ".md,.gen,.smd,.bin,.sms,.sg,.gg,.pce,.sgx,.cue,.ccd,.chd,.iso,.cdi,.gdi,.mdf,.mds,.img,.pbp,"
    ".cso,.m3u,.a26,.a78,.lnx,.ws,.wsc,.ngp,.ngc,.npc,.col,.int,.vec,.d64,.t64,.prg,.crt,.adf,.dsk,"
    ".rom,.p8,.32x,.gcm,.ciso,.rvz,.wbfs,.cas,.tap,.j64,.jag,.vb,.min,.sv,.gam,.pc2,.xiso,"
    // MAME4droid's computers and handhelds
    ".2mg,.uef,.csw,.ccc,.atm,.tgc,.rpk,.mgt,.sad,.d77,.d88,.t77,.ddp,.st2,.cmd,.dmk,.ssd,.dsd,.hfe,.td0,.imd,.ipf";

// Kick an ES-DE-style bulk import off the RENDER thread. The read-only folder probe (opendir/readdir/
// stat over the chosen root, which may be a slow NAS/FTP share) runs on a DETACHED worker so it can
// never freeze the render heartbeat and trip the watchdog; the worker only produces a list of matched
// (folder -> catalog index) candidates, which gsAutoAddTick() applies on the render thread. Matching
// and the has-ROMs gate touch only the immutable emulator catalog + pure helpers, never mXmbSystems.
void NanoMenu::gsAutoAddFromRoot(const std::string& root, bool quiet) {
    if (root.empty()) return;
    if (mBulkAddScanning.load(std::memory_order_acquire)) {   // one scan at a time
        if (!quiet && mBulkAddQuiet) {   // an ES-DE move's quiet scan: the user's folder runs after it
            mBulkAddNextRoot = root;
            showXmbMessage(trDyn("Scanning folder..."), trDyn("Looking for game systems to add."), 100000);
        } else if (quiet && !mBulkAddQuiet) {   // the user's folder scan: the card's quiet one runs after it
            mBulkAddNextQuietRoot = root;
        }
        return;
    }
    if (mEmuCatalog.empty()) loadEmuCatalog();

    mBulkAddScanning.store(true, std::memory_order_release);
    mBulkAddDone.store(false, std::memory_order_release);
    mBulkAddQuiet = quiet;
    { std::lock_guard<std::mutex> lk(mBulkAddLock); mBulkAddResults.clear(); }
    if (!quiet) showXmbMessage(trDyn("Scanning folder..."), trDyn("Looking for game systems to add."), 100000);

    // Snapshot the catalog's (platformId -> first index) so the worker never races a catalog reload.
    std::vector<std::pair<std::string,int>> catIds;
    for (int i = 0; i < (int)mEmuCatalog.size(); i++) {
        std::string pid = autoAddLower(mEmuCatalog[i].platformId);
        if (!pid.empty()) catIds.push_back({pid, i});
    }

    std::thread([this, root, catIds]() {
        std::vector<BulkAddCand> cands;
        std::vector<std::string> ids;
        for (const auto& kv : catIds) ids.push_back(kv.first);
        DIR* d = opendir(root.c_str());
        if (d) {
            struct dirent* de;
            int examined = 0;
            while ((de = readdir(d)) != nullptr && examined < 4000) {
                if (de->d_name[0] == '.') continue;
                examined++;
                std::string name = de->d_name;
                std::string full = root + "/" + name;
                struct stat st;
                if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

                // The ES-DE name or the catalog's, through the folder's alias group.
                const int pi = nanosys::platformForFolder(name, ids);
                const int ci = pi < 0 ? -1 : catIds[pi].second;
                if (ci < 0) continue;   // not a recognised system folder (media / gamelists / etc.)
                if (!nanosys::folderHasRom(full, kBulkAddGenericExts)) continue;   // empty -> skip

                BulkAddCand c; c.folder = full; c.catIdx = ci; cands.push_back(std::move(c));
            }
            closedir(d);
        }
        {
            std::lock_guard<std::mutex> lk(mBulkAddLock);
            mBulkAddResults = std::move(cands);
        }
        mBulkAddDone.store(true, std::memory_order_release);
        FrameFlag::wake();
    }).detach();
}

static int romScanMaxDepth();

// Render thread: once the bulk-add worker has finished, apply its matched folders. Creating/linking
// systems and touching mXmbSystems must happen here (render thread), never on the worker.
void NanoMenu::gsAutoAddTick() {
    systemsHealTick();   // the same render paths, and the same rule for changing mXmbSystems
    if (!mBulkAddScanning.load(std::memory_order_acquire)) return;
    if (!mBulkAddDone.load(std::memory_order_acquire)) return;
    // bgScanThreadFunc reads mXmbSystems unlocked: apply once it is done (it never waits on this).
    if (mBgScanThreadRunning) return;

    std::vector<BulkAddCand> cands;
    { std::lock_guard<std::mutex> lk(mBulkAddLock); cands.swap(mBulkAddResults); }
    mBulkAddDone.store(false, std::memory_order_release);
    mBulkAddScanning.store(false, std::memory_order_release);
    std::string next, nextQuiet;
    next.swap(mBulkAddNextRoot);
    nextQuiet.swap(mBulkAddNextQuietRoot);

    // Find an existing system that already represents a platform/folder (attach rather than duplicate).
    auto findExisting = [&](const std::string& folderLower, const std::string& platformIdLower) -> int {
        for (int i = 0; i < (int)mXmbSystems.size(); i++) {
            if (autoAddLower(mXmbSystems[i].id) == platformIdLower) return i;
            for (const auto& a : getRomFolderAliases(mXmbSystems[i].romDir)) {
                std::string la = autoAddLower(a);
                if (la == folderLower || la == platformIdLower) return i;
            }
        }
        return -1;
    };

    int added = 0, linked = 0, needEmu = 0;
    for (const auto& c : cands) {
        if (c.catIdx < 0 || c.catIdx >= (int)mEmuCatalog.size()) continue;
        const std::string full = c.folder;
        std::string nameLower = full;
        { size_t sl = nameLower.rfind('/'); if (sl != std::string::npos) nameLower = nameLower.substr(sl + 1); }
        nameLower = autoAddLower(nameLower);
        std::string platformId = mEmuCatalog[c.catIdx].platformId;
        std::string platformIdLower = autoAddLower(platformId);
        int ex = findExisting(nameLower, platformIdLower);
        if (ex >= 0 && nanoscummvm::listsGames(mXmbSystems[ex].romDir, mXmbSystems[ex].launchPkg)) continue;   // its games are ScummVM's list

        if (ex >= 0) {
            XmbSystem& s = mXmbSystems[ex];
            if (mBulkAddQuiet && s.scanSources.empty()) {
                // A source replaces the default folders: leave the system alone when they cover this
                // folder (or the user removed its alias), else keep them as sources beside it.
                bool skip = false;
                for (const auto& d : s.disabledDefaultFolders) if (d == nameLower) { skip = true; break; }
                const std::vector<std::string> defs = buildScanCandidates(s);
                for (const auto& p : defs) if (p == full) { skip = true; break; }
                if (skip) continue;
                auto norm = [](const std::string& p) -> std::string {
                    char rp[4096]; std::string q = realpath(p.c_str(), rp) ? std::string(rp) : p;
                    if (q.rfind("/data/media/0/", 0) == 0) q = "/storage/emulated/0/" + q.substr(14);
                    else if (q.rfind("/sdcard/", 0) == 0)  q = "/storage/emulated/0/" + q.substr(8);
                    return q;
                };
                const std::string fullNorm = norm(full);
                const int defSub = romScanMaxDepth() > 0 ? 1 : 0;
                for (const auto& p : defs) {
                    struct stat st;
                    if (stat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                    const std::string q = norm(p);
                    bool have = (q == fullNorm);
                    for (const auto& src : s.scanSources) if (src.value == q) { have = true; break; }
                    if (have) continue;
                    ScanSource ds; ds.type = 0; ds.value = q; ds.subfolders = defSub; s.scanSources.push_back(ds);
                }
            }
            bool dup = false;
            for (const auto& src : s.scanSources) if (src.value == full) { dup = true; break; }
            if (dup) continue;
            ScanSource src; src.type = 0; src.value = full; src.subfolders = 1; s.scanSources.push_back(src);
            invalidateSystemScan(s);
            linked++;
            if (s.isStandalone() ? !packageInstalled(s.launchPkg) : !coreSoExists(s.coreSo)) needEmu++;
        } else {
            // The platform's first player may be one this device can't run (WonderSwan's is the
            // SwanEmu app): the first that can, when there is one.
            bool noneReady = true;
            const std::vector<int> ready = emuPlatformChoices(platformId, noneReady);
            const int ci = !noneReady && !ready.empty() ? ready[0] : c.catIdx;
            int idx = nano_makeUniqueSystem(mXmbSystems, mEmuCatalog[c.catIdx].platform, platformId);
            applyEmuEntryToSystem(mXmbSystems[idx], mEmuCatalog[ci]);
            mXmbSystems[idx].iconRef = gsIconRefForPlatform(platformId, mEmuCatalog[c.catIdx].platform);
            ScanSource src; src.type = 0; src.value = full; src.subfolders = 1; mXmbSystems[idx].scanSources.push_back(src);
            invalidateSystemScan(mXmbSystems[idx]);
            added++;
            if (mXmbSystems[idx].isStandalone() ? !packageInstalled(mXmbSystems[idx].launchPkg)
                                                : !coreSoExists(mXmbSystems[idx].coreSo)) needEmu++;
        }
    }

    if (added > 0 || linked > 0) {
        saveSystemsConfig(true);
        forceRescanAllSystems();
        if (mBulkAddQuiet) mEsdeReloadAfterScan = true;   // their gamelists' flags and collection games
    }
    // Pop the folder browser back to the Game Systems list, if it is still showing (single level).
    if (!mBulkAddQuiet && !mPs3Stack.empty() && mPs3Stack.back().screenKind == GS_FOLDERBROWSE) mPs3Stack.pop_back();
    gsRefreshStackLevels();
    buildPs3Cats();

    if (mBulkAddQuiet && added == 0 && linked == 0) {
        if (!next.empty()) gsAutoAddFromRoot(next);
        if (!nextQuiet.empty()) gsAutoAddFromRoot(nextQuiet, true);
        return;
    }
    if (added == 0 && linked == 0) {
        showXmbMessage(trDyn("No new systems found"),
                       trDyn("No subfolders matched a known system with ROMs."), 300);
    } else {
        std::string l1 = xmbFmt(trDyn(added == 1 ? "Added %d system" : "Added %d systems"), added);
        std::string l2;
        if (linked && needEmu)
            l2 = xmbFmt(trDyn("Linked %d existing; %d need an emulator"), linked, needEmu);
        else if (linked)
            l2 = xmbFmt(trDyn("Linked %d to existing systems"), linked);
        else if (needEmu)
            l2 = xmbFmt(trDyn(needEmu == 1 ? "%d needs an emulator installed"
                                           : "%d need an emulator installed"), needEmu);
        else
            l2 = trDyn("Scanning for games now...");
        showXmbMessage(l1, l2, 340);
    }
    if (!next.empty()) gsAutoAddFromRoot(next);
    if (!nextQuiet.empty()) gsAutoAddFromRoot(nextQuiet, true);   // queued again if `next` started a scan
}

// Build the ordered, de-duplicated list of directories to scan for a system's
// ROMs. When the user has set scanSources they are authoritative: only those are
// scanned. Otherwise the legacy default candidates are used (so built-ins with
// empty scanSources behave as before), expanded across the system's ES-DE /
// libretro folder-name aliases minus any the user removed (disabledDefaultFolders).
// A persist.gammaos.nano.xmb.<id>.path override is always prepended at the front.
std::vector<std::string> NanoMenu::buildScanCandidates(const XmbSystem& sys) {
    // The ScummVM app's games are its own game list, kept as files in a folder of Nano's.
    if (nanoscummvm::listsGames(sys.romDir, sys.launchPkg)) return {nanoscummvm::mirrorDir(sys.launchPkg)};
    std::vector<std::string> scanPaths;

    // 0. User-chosen scan sources (highest priority). safuri sources scan via
    //    their resolved raw mount; sources lacking a usable path are skipped.
    for (const auto& src : sys.scanSources) {
        if (src.type == 0 && !src.value.empty())
            scanPaths.push_back(src.value);
        else if (src.type == 1 && !src.rawHint.empty())
            scanPaths.push_back(src.rawHint);
    }

    const std::string romDir = sys.romDir;
    // Default folder aliases to scan. scanSources are authoritative when set (documented contract):
    // in that mode we scan ONLY the user's chosen folders, so the alias list is empty. Otherwise it
    // is the romDir's alias group minus any default folders the user removed (disabledDefaultFolders).
    std::vector<std::string> aliases;
    if (sys.scanSources.empty()) {
        auto lc = [](const std::string& s) {
            std::string o; o.reserve(s.size());
            for (char c : s) o += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            return o;
        };
        for (const auto& a : getRomFolderAliases(romDir)) {
            std::string la = lc(a);
            bool off = false;
            for (const auto& d : sys.disabledDefaultFolders) if (d == la) { off = true; break; }
            if (!off) aliases.push_back(a);
        }
    }

    // 1. Internal storage (raw + FUSE) - for every folder-name alias
    for (const auto& a : aliases) {
        scanPaths.push_back("/data/media/0/ROMs/" + a);
        scanPaths.push_back("/sdcard/ROMs/" + a);
        scanPaths.push_back("/storage/emulated/0/ROMs/" + a);
    }

    // 2. External volumes -- case-insensitive matching for ROMs dir and system dir
    auto addExternalVolume = [&](const std::string& base) {
        std::string romsDir = findCaseInsensitive(base, "ROMs");
        for (const auto& a : aliases) {
            scanPaths.push_back(base + "/ROMs/" + a);
            scanPaths.push_back(base + "/roms/" + a);
            scanPaths.push_back(base + "/" + a);
            if (!romsDir.empty()) {
                std::string sysDir = findCaseInsensitive(romsDir, a);
                if (!sysDir.empty()) scanPaths.push_back(sysDir);
            }
            std::string directDir = findCaseInsensitive(base, a);
            if (!directDir.empty()) scanPaths.push_back(directDir);
        }
    };
    {
        // A share is bind-mounted into /storage as well as /mnt/shares, so without this it gets
        // scanned twice for the same ROMs. Over a network that is slow rather than merely
        // redundant, and it produces duplicate entries; the share pass covers it once.
        const std::vector<std::string> storageShareNames = mountedShareNames();
        DIR* storageDir = opendir("/storage");
        if (storageDir) {
            struct dirent* sEntry;
            while ((sEntry = readdir(storageDir)) != nullptr) {
                if (sEntry->d_name[0] == '.') continue;
                if (!strcmp(sEntry->d_name, "emulated")) continue;
                if (!strcmp(sEntry->d_name, "self")) continue;
                if (std::find(storageShareNames.begin(), storageShareNames.end(),
                              std::string(sEntry->d_name)) != storageShareNames.end()) continue;
                addExternalVolume(std::string("/storage/") + sEntry->d_name);
            }
            closedir(storageDir);
        }
        DIR* mntDir = opendir("/mnt/media_rw");
        if (mntDir) {
            struct dirent* mEntry;
            while ((mEntry = readdir(mntDir)) != nullptr) {
                if (mEntry->d_name[0] == '.') continue;
                addExternalVolume(std::string("/mnt/media_rw/") + mEntry->d_name);
            }
            closedir(mntDir);
        }
    }

    // 3. Prop-overridden custom path (highest priority): prepend at front.
    char customPath[PROPERTY_VALUE_MAX] = {};
    char propKey[160];
    snprintf(propKey, sizeof(propKey), "persist.gammaos.nano.xmb.%s.path", sys.id.c_str());
    property_get(propKey, customPath, "");
    if (customPath[0]) scanPaths.insert(scanPaths.begin(), std::string(customPath));

    // Deduplicate candidate paths (preserve order; exact string match).
    {
        std::set<std::string> seen;
        std::vector<std::string> unique;
        for (auto& p : scanPaths) {
            if (seen.insert(p).second) unique.push_back(std::move(p));
        }
        scanPaths = std::move(unique);
    }
    return scanPaths;
}

// ---------------------------------------------------------------------------
// ROM Path Scanning
// ---------------------------------------------------------------------------

// Helper: find a case-insensitive match for 'target' in directory 'parent'
static std::string findCaseInsensitive(const std::string& parent, const std::string& target) {
    DIR* d = opendir(parent.c_str());
    if (!d) return "";
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strcasecmp(e->d_name, target.c_str()) == 0) {
            std::string result = parent + "/" + e->d_name;
            closedir(d);
            return result;
        }
    }
    closedir(d);
    return "";
}

// Lowercase a string in place (ASCII only) -- the scanners already do this inline
// per site; the recursive helpers below reuse this single copy.
static std::string romLower(const std::string& in) {
    std::string o = in;
    for (auto& c : o) if (c >= 'A' && c <= 'Z') c += 32;
    return o;
}

static std::string romDisplayName(const std::string& rom) {
    size_t sl = rom.rfind('/');
    std::string name = (sl == std::string::npos) ? rom : rom.substr(sl + 1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    // A PICO-8 cart image (Celeste.p8.png) is named after the cart it holds: drop the .p8 too.
    if (name.size() > 3 && strcasecmp(name.c_str() + name.size() - 3, ".p8") == 0
        && strcasecmp(rom.c_str() + rom.size() - 4, ".png") == 0)
        name.resize(name.size() - 3);
    return name;
}

static void buildRomDisplayNames(const std::vector<std::string>& roms,
                                 std::vector<std::string>& displayNames) {
    displayNames.clear();
    displayNames.reserve(roms.size());
    for (const auto& rom : roms) displayNames.push_back(romDisplayName(rom));
}

// Keep ROM paths paired with their labels while sorting by the name shown in the UI.
static void sortRomEntriesByDisplayName(std::vector<std::string>& roms,
                                        std::vector<std::string>& displayNames) {
    if (roms.size() != displayNames.size()) return;
    std::vector<size_t> order;
    order.reserve(roms.size());
    for (size_t i = 0; i < roms.size(); i++) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        int nameCmp = strcasecmp(displayNames[a].c_str(), displayNames[b].c_str());
        if (nameCmp != 0) return nameCmp < 0;
        return strcasecmp(roms[a].c_str(), roms[b].c_str()) < 0;
    });

    std::vector<std::string> sortedRoms;
    std::vector<std::string> sortedNames;
    sortedRoms.reserve(roms.size());
    sortedNames.reserve(displayNames.size());
    for (size_t i : order) {
        sortedRoms.push_back(std::move(roms[i]));
        sortedNames.push_back(std::move(displayNames[i]));
    }
    roms.swap(sortedRoms);
    displayNames.swap(sortedNames);
}

// Recursively scan one candidate directory for ROMs. Mirrors the inner readdir
// loop that used to be duplicated at the three scan sites (extension filter, junk
// blacklist, case-insensitive dedup, 0-byte skip) and adds bounded-depth recursion
// following the NanoMenuMusic scanDirRecursive precedent. On a directory entry it
// recurses when depth < maxDepth (maxDepth 0 = top level only, current behavior);
// DT_UNKNOWN entries are lstat'd to classify, and symlinked directories are skipped
// to avoid loops. An .m3u/.m3u8 file is pushed to BOTH outRoms and outM3u so the
// playlist appears as a launchable entry and the grouping pass can resolve its discs.
// cnt is incremented per accepted ROM so the caller can pick the busiest candidate.
static void scanSystemDir(const std::string& dir, int depth, int maxDepth,
                          const std::set<std::string>& exts,
                          std::set<std::string>& seenNames,
                          std::vector<std::string>& outRoms,
                          std::vector<std::string>& outM3u,
                          int& cnt) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string name(entry->d_name);
        std::string child = dir + "/" + name;

        // Directory handling: recurse (bounded) into real subdirectories. Trust
        // d_type when the filesystem provides it; only lstat on DT_UNKNOWN. A
        // symlinked directory is skipped so a self/parent link cannot loop.
        bool isDir = false, isLnk = false;
        if (entry->d_type == DT_DIR) {
            isDir = true;
        } else if (entry->d_type == DT_LNK) {
            isLnk = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            struct stat lst;
            if (lstat(child.c_str(), &lst) == 0) {
                if (S_ISLNK(lst.st_mode)) isLnk = true;
                else if (S_ISDIR(lst.st_mode)) isDir = true;
            }
        }
        if (isLnk) continue;
        if (isDir) {
            if (depth < maxDepth) subdirs.push_back(child);
            continue;
        }

        size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;

        std::string ext = name.substr(dot);
        for (size_t i = 0; i < ext.size(); i++)
            if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] += 32;
        // A PICO-8 cart can be a PNG (Celeste.p8.png: the cartridge picture with the game stored
        // in it), so a .png passes when the system takes .png and the name has the cart form;
        // other images in the folder (covers, screenshots) stay out of the list.
        const bool pngCart = ext == ".png" && exts.count(ext) && name.size() > 7
            && strcasecmp(name.c_str() + name.size() - 7, ".p8.png") == 0;
        if (!pngCart && (ext == ".txt" || ext == ".jpg" || ext == ".png" || ext == ".xml"
            || ext == ".srm" || ext == ".sav" || ext == ".state" || ext == ".rtc"
            || ext == ".dat" || ext == ".bak" || ext == ".cfg" || ext == ".log")) {
            continue;
        }

        bool isM3u = (ext == ".m3u" || ext == ".m3u8");
        if (!isM3u && !exts.count(ext)) continue;

        std::string nameLower = name;
        for (size_t i = 0; i < nameLower.size(); i++)
            if (nameLower[i] >= 'A' && nameLower[i] <= 'Z') nameLower[i] += 32;
        if (!seenNames.insert(nameLower).second) continue;

        struct stat st;
        if (stat(child.c_str(), &st) == 0 && st.st_size == 0) continue;

        outRoms.push_back(child);
        if (isM3u) outM3u.push_back(child);
        cnt++;
    }
    closedir(d);

    // Descend in a stable, case-insensitive order so the merged list is
    // deterministic across scans regardless of readdir order.
    std::sort(subdirs.begin(), subdirs.end(),
              [](const std::string& a, const std::string& b) {
                  return strcasecmp(a.c_str(), b.c_str()) < 0;
              });
    for (const auto& s : subdirs)
        scanSystemDir(s, depth + 1, maxDepth, exts, seenNames, outRoms, outM3u, cnt);
}

// Parse an .m3u/.m3u8 playlist into its referenced disc paths. Mirrors
// NanoMenu::parseM3u (NanoMenuMusic.cpp) but WITHOUT the audio-extension gate and
// without an on-disk existence check: it resolves relative entries against the
// m3u's own directory, keeps absolute entries as-is, converts Windows separators,
// and skips blank / #-comment lines. The disc need not exist on disk for grouping
// (a subfolder disc referenced by name must still be folded away).
static void parseM3uEntries(const std::string& m3uPath,
                            std::vector<std::string>& out) {
    FILE* f = fopen(m3uPath.c_str(), "rb");
    if (!f) return;
    std::string base = m3uPath.substr(0, m3uPath.rfind('/') + 1);
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        s = s.substr(b);
        if (s.empty() || s[0] == '#') continue;      // comment / directive
        for (auto& c : s) if (c == '\\') c = '/';     // windows separators
        std::string path = (s[0] == '/') ? s : (base + s);
        out.push_back(std::move(path));
    }
    fclose(f);
}

// Fold multi-disc entries under their .m3u playlist. For every scanned .m3u, parse
// the discs it references and drop those discs from the ROM list, keeping the .m3u
// itself as the single launchable entry. A disc that no playlist references stays.
// The referenced set is matched case-insensitively and across the internal-storage
// alias forms (/data/media/0 <-> /sdcard <-> /storage/emulated/0) so a playlist that
// spells its discs one way still folds a disc the scanner found via another mount.
// Runs ONCE per system after all candidate paths merge and BEFORE the sort +
// displayNames build, so the column shows the .m3u basename in place of the discs.
static void applyM3uGrouping(std::vector<std::string>& roms,
                             const std::vector<std::string>& m3uPaths) {
    if (m3uPaths.empty() || roms.empty()) return;

    // Rewrite the internal-storage aliases to a single canonical prefix so the two
    // spellings compare equal. External volumes are left untouched.
    auto canon = [](const std::string& in) -> std::string {
        std::string p = romLower(in);
        if (p.rfind("/sdcard/", 0) == 0)
            p = "/data/media/0/" + p.substr(8);
        else if (p.rfind("/storage/emulated/0/", 0) == 0)
            p = "/data/media/0/" + p.substr(20);
        return p;
    };

    std::set<std::string> referenced;
    for (const auto& m3u : m3uPaths) {
        std::vector<std::string> discs;
        parseM3uEntries(m3u, discs);
        for (const auto& d : discs) referenced.insert(canon(d));
    }
    if (referenced.empty()) return;

    std::vector<std::string> kept;
    kept.reserve(roms.size());
    for (const auto& r : roms) {
        // Never drop a playlist even if some other playlist lists it.
        std::string rl = romLower(r);
        bool isM3u = (rl.size() >= 4 && rl.compare(rl.size() - 4, 4, ".m3u") == 0)
                  || (rl.size() >= 5 && rl.compare(rl.size() - 5, 5, ".m3u8") == 0);
        if (!isM3u && referenced.count(canon(r))) continue;   // a folded disc
        kept.push_back(r);
    }
    roms.swap(kept);
}

// Read the two ROM-scan toggles. Multi-disc .m3u grouping defaults ON. Recursive
// subfolder scanning defaults OFF for now: it is reliable on a settled home but was
// observed NOT to recurse on a fresh boot (top-level files scan, subdirs are missed,
// likely a storage mount-namespace visibility timing issue at boot). Kept opt-in
// until that is fixed, so no user regresses. The scanners call this once at the top
// so a per-frame prop read is avoided.
static const int kRomScanMaxDepth = 6;   // bounded recursion, matches the music/photo/video scanners
static int romScanMaxDepth() {
    char buf[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.rom.recursive", buf, "0");
    bool on = (buf[0] == '1' || strcasecmp(buf, "true") == 0);
    return on ? kRomScanMaxDepth : 0;
}
// The scan depth of each folder the user added: its subfolders too unless it was added with This
// Folder Only, whatever Scan ROM Subfolders says (that covers only the default folders). Read with
// the scan paths, before the scan: the menu can add a folder meanwhile.
static std::map<std::string, int> folderScanDepths(const NanoMenu::XmbSystem& sys) {
    std::map<std::string, int> out;
    for (const auto& src : sys.scanSources) {
        const int depth = src.subfolders == 0 ? 0 : kRomScanMaxDepth;
        out[src.value] = depth;
        if (!src.rawHint.empty()) out[src.rawHint] = depth;
    }
    return out;
}

// A ScummVM system's game files brought up to ScummVM's game list, before the system is scanned
// (NanoScummvmList.h). A list that can't be read (ScummVM not started yet) or is caught while
// ScummVM rewrites it leaves them as they are; with ScummVM gone, there are none.
static void syncScummvmList(const NanoMenu::XmbSystem& sys) {
    if (!nanoscummvm::listsGames(sys.romDir, sys.launchPkg)) return;
    const std::string dir = nanoscummvm::mirrorDir(sys.launchPkg);
    std::string ini;
    if (nanobios::appFileRead(nanoscummvm::iniPath(sys.launchPkg), &ini)) {
        // ScummVM rewrites the whole file in place (it does on quitting): one changed in the last
        // seconds, or cut off before its [scummvm] section, may be half written, so its games are
        // added but none taken away until a scan reads it whole.
        struct stat ist;
        const bool settled = stat(nanoscummvm::iniPath(sys.launchPkg).c_str(), &ist) == 0
                             && time(nullptr) - ist.st_mtime >= 3;
        nanoscummvm::syncDir(dir, nanoscummvm::parseGames(ini), settled && nanoscummvm::iniComplete(ini));
        return;
    }
    struct stat st;
    if (stat(("/data/data/" + sys.launchPkg).c_str(), &st) != 0 && errno == ENOENT) nanoscummvm::syncDir(dir, {});
}

static bool romM3uGroupEnabled() {
    char buf[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.rom.m3u_group", buf, "1");
    return (buf[0] != '0' && strcasecmp(buf, "false") != 0);
}

void NanoMenu::scanRomPaths() {
    ALOGD("NanoMenu: scanning ROM paths");
    for (auto& sys : mXmbSystems) {
        if (!sys.enabled) continue;   // disabled systems are never scanned
        if (sys.scanned) continue;

        syncScummvmList(sys);
        std::vector<std::string> scanPaths = buildScanCandidates(sys);
        const std::map<std::string, int> folderDepths = folderScanDepths(sys);

        // Build extension set from comma-separated list
        std::set<std::string> exts;
        {
            const std::string& extStr = sys.acceptExts;
            size_t pos = 0;
            while (pos < extStr.size()) {
                size_t comma = extStr.find(',', pos);
                if (comma == std::string::npos) comma = extStr.size();
                std::string ext = extStr.substr(pos, comma - pos);
                while (!ext.empty() && ext[0] == ' ') ext.erase(0, 1);
                if (!ext.empty()) exts.insert(ext);
                pos = comma + 1;
            }
        }
        exts.insert(".zip");
        exts.insert(".7z");

        // Clear any cached data -- fresh scan replaces it
        sys.roms.clear();
        sys.displayNames.clear();
        sys.activePaths.clear();

        // Track filenames already seen (case-insensitive) for deduplication
        std::set<std::string> seenFilenames;
        // Per-path ROM count for determining activePath (largest collection)
        std::string bestPath;
        int bestCount = 0;
        bool anyPath = false;
        // Recursive-scan depth + .m3u grouping toggles (default ON).
        int maxDepth = romScanMaxDepth();
        bool groupM3u = romM3uGroupEnabled();
        std::vector<std::string> m3uPaths;

        // Scan ALL candidate paths and merge results
        for (const auto& candidatePath : scanPaths) {
            int pathRomCount = 0;
            const auto fd = folderDepths.find(candidatePath);
            const int depth = fd != folderDepths.end() ? fd->second : maxDepth;
            scanSystemDir(candidatePath, 0, depth, exts, seenFilenames,
                          sys.roms, m3uPaths, pathRomCount);

            if (pathRomCount > 0) {
                sys.activePaths.push_back(candidatePath);
                anyPath = true;
                if (pathRomCount > bestCount) {
                    bestCount = pathRomCount;
                    bestPath = candidatePath;
                }
            }
        }

        // Fold multi-disc discs under their .m3u playlist (once, before the sort).
        if (groupM3u) applyM3uGrouping(sys.roms, m3uPaths);

        if (!anyPath) {
            sys.pathExists = false;
            ALOGD("NanoMenu: %s: no accessible path found (will retry)", sys.name.c_str());
            continue;
        }

        sys.scanned = true;
        sys.pathExists = true;
        sys.activePath = bestPath;
        sys.lastScanTime = elapsedRealtime();

        buildRomDisplayNames(sys.roms, sys.displayNames);
        applyRomNameOverrides(sys);   // patch in any per-game title overrides

        ALOGD("NanoMenu: %s: %zu ROMs across %zu paths (primary: %s)",
              sys.name.c_str(), sys.roms.size(), sys.activePaths.size(),
              bestPath.c_str());

        // Save cache to DE storage -- each line is a full ROM path
        {
            std::string cacheDir = "/data/system/nano_xmb_cache";
            mkdir(cacheDir.c_str(), 0755);
            std::string cachePath = cacheDir + "/" + sys.id + ".list";
            int cfd = open(cachePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (cfd >= 0) {
                for (const auto& rom : sys.roms) {
                    std::string line = rom + "\n";
                    write(cfd, line.c_str(), line.size());
                }
                close(cfd);
            }
        }
    }
    // Only mark scan complete if all systems were scanned
    bool allScanned = true;
    for (const auto& s : mXmbSystems) {
        if (!s.scanned) { allScanned = false; break; }
    }
    mXmbRomScanDone = allScanned;
}

// Scan a single system's ROM paths into temp buffers. Only updates
// the system's rom/displayNames/activePath if the result differs from
// the current data. Returns true if the system was updated.
bool NanoMenu::scanOneSystemAsync(int sysIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return false;
    auto& sys = mXmbSystems[sysIdx];
    if (!sys.enabled) return false;   // disabled systems are never scanned

    syncScummvmList(sys);
    std::vector<std::string> scanPaths = buildScanCandidates(sys);
    const std::map<std::string, int> folderDepths = folderScanDepths(sys);

    // Build extension set
    std::set<std::string> exts;
    {
        const std::string& extStr = sys.acceptExts;
        size_t pos = 0;
        while (pos < extStr.size()) {
            size_t comma = extStr.find(',', pos);
            if (comma == std::string::npos) comma = extStr.size();
            std::string ext = extStr.substr(pos, comma - pos);
            while (!ext.empty() && ext[0] == ' ') ext.erase(0, 1);
            if (!ext.empty()) exts.insert(ext);
            pos = comma + 1;
        }
    }
    exts.insert(".zip");
    exts.insert(".7z");

    // Scan into TEMP buffers (don't touch sys.roms yet)
    std::vector<std::string> newRoms;
    std::vector<std::string> newActivePaths;
    std::set<std::string> seenFilenames;
    std::string newBestPath;
    int bestCount = 0;
    // Recursive-scan depth + .m3u grouping toggles (default ON).
    int maxDepth = romScanMaxDepth();
    bool groupM3u = romM3uGroupEnabled();
    std::vector<std::string> m3uPaths;

    for (const auto& candidatePath : scanPaths) {
        int pathRomCount = 0;
        const auto fd = folderDepths.find(candidatePath);
        const int depth = fd != folderDepths.end() ? fd->second : maxDepth;
        scanSystemDir(candidatePath, 0, depth, exts, seenFilenames,
                      newRoms, m3uPaths, pathRomCount);

        if (pathRomCount > 0) {
            newActivePaths.push_back(candidatePath);
            if (pathRomCount > bestCount) {
                bestCount = pathRomCount;
                newBestPath = candidatePath;
            }
        }
    }

    // Fold multi-disc discs under their .m3u playlist (once, before the sort).
    if (groupM3u) applyM3uGrouping(newRoms, m3uPaths);

    std::vector<std::string> newDisplayNames;
    buildRomDisplayNames(newRoms, newDisplayNames);
    {   // DSi theme: queue this DS system's banners (async, dedup) so the titles land shortly after
        std::string dir = sys.romDir; for (auto& c : dir) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (dir == "nds" || sys.shortname == "NDS") ndsBannerPrefetch(newRoms);
    }
    applyRomNameOverrides(newRoms, newDisplayNames);

    ALOGD("NanoMenu: %s: scan found %zu ROMs across %zu paths (current: %zu ROMs)",
          sys.name.c_str(), newRoms.size(), newActivePaths.size(), sys.roms.size());

    // Check if result differs from current data
    bool changed = (newRoms != sys.roms);

    // Guard against downgrading cached data when storage is partially
    // mounted (e.g., SD card not yet available after reboot). Only
    // replace with fewer ROMs if the new scan found at least as many
    // source directories. Otherwise storage likely isn't fully mounted.
    if (changed && !newRoms.empty() && newRoms.size() < sys.roms.size()
        && sys.scanned) {
        size_t curPathCount = sys.activePaths.size();
        if (curPathCount == 0 && !sys.roms.empty()) {
            std::set<std::string> dirs;
            for (const auto& r : sys.roms) {
                size_t sl = r.rfind('/');
                if (sl != std::string::npos) dirs.insert(r.substr(0, sl));
            }
            curPathCount = dirs.size();
        }
        if (newActivePaths.size() < curPathCount) {
            sys.lastScanTime = elapsedRealtime();
            return false;
        }
    }

    if (changed || !sys.scanned) {
        // Swap in new data atomically (fast -- just pointer swaps)
        sys.roms = std::move(newRoms);
        sys.activePaths = std::move(newActivePaths);
        sys.activePath = newBestPath;
        sys.pathExists = !sys.roms.empty();

        sys.displayNames = std::move(newDisplayNames);

        // Update cache file (xmbCachePath keys on the stable id, matching the
        // loader and the bg-scan writer; romDir is NOT the cache key)
        mkdir("/data/system/nano_xmb_cache", 0755);
        std::string cachePath = xmbCachePath(sys);
        int cfd = open(cachePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cfd >= 0) {
            for (const auto& rom : sys.roms) {
                std::string line = rom + "\n";
                write(cfd, line.c_str(), line.size());
            }
            close(cfd);
        }

        if (changed) {
            ALOGD("NanoMenu: %s: %zu ROMs across %zu paths (primary: %s)",
                  sys.name.c_str(), sys.roms.size(), sys.activePaths.size(),
                  sys.activePath.c_str());
            mDisplayDirty = true;
        }
    }

    sys.scanned = true;
    sys.lastScanTime = elapsedRealtime();
    return changed;
}

// A system's folders or extensions changed: a scan already running read the old ones, so its
// result for this system is dropped when it lands (it would mark the system scanned and leave
// the follow-up scan to the partial-mount guard). The caller then calls forceRescanAllSystems.
void NanoMenu::invalidateSystemScan(XmbSystem& sys) {
    unlink(xmbCachePath(sys).c_str());
    sys.scanned = false;
    sys.scanInputsGen++;
}

void NanoMenu::forceRescanAllSystems() {
    // Launch a background thread to scan all systems. The thread builds
    // results in mBgScanResults; the render loop swaps them in when ready.
    if (mBgScanThreadRunning) { mBgScanAgain = true; return; }   // scan again once this one is done
    mBgScanAgain = false;
    mBgScanThreadRunning = true;
    ALOGI("NanoMenu: launching background scan thread");
    std::thread(&NanoMenu::bgScanThreadFunc, this).detach();
}

// Re-scan the whole ROM library after a scan-behaviour toggle (Scan ROM
// Subfolders / Group Multi-Disc) changes. The recursive / grouping decision is
// baked into every system's cached .list, so a stale cache would keep showing the
// old grouping until something else forced a rescan; drop the per-system caches and
// the scanned flags so the fresh bg scan re-derives the library with the new
// settings and republishes it to the render thread.
void NanoMenu::romRescanFromSettings() {
    for (auto& sys : mXmbSystems) invalidateSystemScan(sys);
    mXmbRomScanDone = false;
    forceRescanAllSystems();
}

// Background thread: scans all systems and stores results for the render
// thread to pick up. Never touches sys.roms/displayNames directly -- only
// writes to mBgScanResults behind a mutex.
void NanoMenu::bgScanThreadFunc() {
    int numSys = (int)mXmbSystems.size();
    std::vector<BgScanResult> results(numSys);

    for (int si = 0; si < numSys; si++) {
        const auto& sys = mXmbSystems[si];
        auto& res = results[si];
        res.valid = false;
        res.scanInputsGen = sys.scanInputsGen;
        res.id = sys.id;
        if (!sys.enabled) continue;   // disabled systems are never scanned

        syncScummvmList(sys);
        std::vector<std::string> scanPaths = buildScanCandidates(sys);
        const std::map<std::string, int> folderDepths = folderScanDepths(sys);

        // Build extension set
        std::set<std::string> exts;
        { const std::string& es = sys.acceptExts;
          size_t pos = 0;
          while (pos < es.size()) {
              size_t c = es.find(',', pos);
              if (c == std::string::npos) c = es.size();
              std::string ext = es.substr(pos, c - pos);
              while (!ext.empty() && ext[0] == ' ') ext.erase(0, 1);
              if (!ext.empty()) exts.insert(ext);
              pos = c + 1;
          }
        }
        exts.insert(".zip");
        exts.insert(".7z");

        // Scan all paths, merge
        std::set<std::string> seenNames;
        std::string bestPath;
        int bestCount = 0;
        // Recursive-scan depth + .m3u grouping toggles (default ON).
        int maxDepth = romScanMaxDepth();
        bool groupM3u = romM3uGroupEnabled();
        std::vector<std::string> m3uPaths;

        for (const auto& cp : scanPaths) {
            int cnt = 0;
            const auto fd = folderDepths.find(cp);
            const int depth = fd != folderDepths.end() ? fd->second : maxDepth;
            scanSystemDir(cp, 0, depth, exts, seenNames, res.roms, m3uPaths, cnt);
            if (cnt > 0) {
                res.activePaths.push_back(cp);
                res.valid = true;
                if (cnt > bestCount) { bestCount = cnt; bestPath = cp; }
            }
        }

        // Fold multi-disc discs under their .m3u playlist (once, before the sort).
        if (groupM3u) applyM3uGrouping(res.roms, m3uPaths);
        res.activePath = bestPath;

        // Sort by display name
        buildRomDisplayNames(res.roms, res.displayNames);
        sortRomEntriesByDisplayName(res.roms, res.displayNames);
    }

    // Publish results for the render thread
    {
        std::lock_guard<std::mutex> lock(mBgScanMutex);
        mBgScanResults = std::move(results);
        mBgScanResultReady = true;
    }
    mBgScanThreadRunning = false;
    ALOGI("NanoMenu: background scan thread complete");
}

// ---------------------------------------------------------------------------
// XMB Recently Played
// ---------------------------------------------------------------------------

static const char* kXmbRecentFile = "/data/system/nano_xmb_recent.list";

void NanoMenu::loadXmbRecent() {
    mXmbRecent.clear();
    int fd = open(kXmbRecentFile, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0 || st.st_size > 64 * 1024 * 1024) {
        close(fd); return;
    }
    std::string content(st.st_size, '\0');
    ssize_t rd = read(fd, &content[0], st.st_size);
    close(fd);
    if (rd <= 0) return;
    content.resize(rd);

    for (auto& r : nanolaunch::parseRecents(content, mXmbRecentMax > 0 ? (size_t)mXmbRecentMax : 0)) {
        XmbRecentEntry e;
        e.romPath = std::move(r.romPath);
        e.coreSo = std::move(r.coreSo);
        e.launchPkg = std::move(r.launchPkg);
        e.launchIntent = std::move(r.launchIntent);
        e.displayName = std::move(r.displayName);
        e.systemName = std::move(r.systemName);
        e.romDir = std::move(r.romDir);
        e.standalone = !e.launchPkg.empty();
        mXmbRecent.push_back(std::move(e));
    }
    if (dropHiddenRecents()) saveXmbRecent();   // a game hidden by the other nano process
    applyRomNameOverridesToRecents();   // patch in any per-game title overrides
    ALOGD("NanoMenu: loaded %zu recent XMB entries", mXmbRecent.size());
}

// Written whole and renamed into place, from a temp file of this process's (the overlay and the
// home both save it), so a crash mid-write never empties Recently Played. No fsync: a game launch
// saves it.
void NanoMenu::saveXmbRecent() {
    std::vector<nanolaunch::RecentRecord> recs;
    recs.reserve(mXmbRecent.size());
    for (const auto& e : mXmbRecent)
        recs.push_back({e.romPath, e.coreSo, e.launchPkg, e.launchIntent, e.displayName,
                        e.systemName, e.romDir});
    const std::string data = nanolaunch::serializeRecents(recs);
    const std::string tmp = std::string(kXmbRecentFile) + ".tmp." + std::to_string(getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    fchmod(fd, 0644);
    const bool ok = write(fd, data.data(), data.size()) == (ssize_t)data.size();
    close(fd);
    if (!ok || rename(tmp.c_str(), kXmbRecentFile) != 0) {
        ALOGW("NanoMenu: cannot write %s (errno %d)", kXmbRecentFile, errno);
        unlink(tmp.c_str());
    }
}

// ---- User Collections (cross-system game groups) --------------------------------------------
static const char* kCollectionsFile = "/data/system/nano_collections.txt";

void NanoMenu::loadCollections() {
    if (esdeStoreMode()) { esdeStoreLoadCollections(); return; }
    mXmbCollections.clear();
    FILE* f = fopen(kCollectionsFile, "r");
    if (!f) return;
    char line[4096];
    XmbCollection cur; bool haveName = false;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0) {                                   // blank line terminates a collection
            if (haveName) mXmbCollections.push_back(cur);
            cur = XmbCollection(); haveName = false;
            continue;
        }
        if (!haveName) { cur.name = line; haveName = true; }
        else            cur.roms.push_back(line);
    }
    if (haveName) mXmbCollections.push_back(cur);        // last one with no trailing blank
    fclose(f);
}

void NanoMenu::saveCollections() {
    if (esdeStoreMode()) { esdeStoreSaveCollections(); return; }
    int fd = open(kCollectionsFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    chmod(kCollectionsFile, 0644);
    for (const auto& c : mXmbCollections) {
        std::string block = c.name + "\n";
        for (const auto& r : c.roms) block += r + "\n";
        block += "\n";
        ssize_t w = write(fd, block.c_str(), block.size()); (void)w;
    }
    close(fd);
}

// Resolve a stored ROM path to a live (system, rom) index so a collection game reuses the normal
// PS3_ROM launch/boxart/option-menu path. Returns false if the game's system is gone or disabled.
bool NanoMenu::collectionResolveRom(const std::string& romPath, int* sysIdx, int* romIdx) {
    // The exact path, else the first other spelling of the same file (sameFile, no allocation).
    int as = -1, ar = -1;
    for (size_t s = 0; s < mXmbSystems.size(); s++) {
        const XmbSystem& sys = mXmbSystems[s];
        for (size_t r = 0; r < sys.roms.size(); r++) {
            if (sys.roms[r] == romPath) {
                if (sysIdx) *sysIdx = (int)s;
                if (romIdx) *romIdx = (int)r;
                return true;
            }
            if (as < 0 && sameFile(sys.roms[r], romPath)) { as = (int)s; ar = (int)r; }
        }
    }
    if (as < 0) return false;
    if (sysIdx) *sysIdx = as;
    if (romIdx) *romIdx = ar;
    return true;
}

void NanoMenu::buildCollectionsSubmenu(Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.title = "Collections";
    for (size_t i = 0; i < mXmbCollections.size(); i++) {
        int cnt = 0;
        for (const auto& r : mXmbCollections[i].roms)
            if (collectionResolveRom(r, nullptr, nullptr)) cnt++;
        Ps3Item it;
        it.label = mXmbCollections[i].name;
        it.kind = PS3_COLLECTION; it.a = (int)i;
        char v[16]; snprintf(v, sizeof(v), "%d", cnt); it.value = v;
        it.iconTex = mIconTextures[15]; it.nmapTex = bevelForIconIdx(15);
        it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
    Ps3Item it; it.label = "New Collection..."; it.kind = PS3_COLLECTION_NEW;
    it.iconTex = mIconTextures[18]; it.nmapTex = bevelForIconIdx(18);
    it.iconR = it.iconG = it.iconB = 1.0f;
    out.items.push_back(it);
}

void NanoMenu::buildCollectionSubmenu(int colIdx, Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.collectionIdx = colIdx;
    if (colIdx < 0 || colIdx >= (int)mXmbCollections.size()) return;
    const XmbCollection& c = mXmbCollections[colIdx];
    out.title = c.name;
    for (const auto& romPath : c.roms) {
        int s = -1, r = -1;
        if (!collectionResolveRom(romPath, &s, &r)) continue;   // skip games whose system is gone/disabled
        const XmbSystem& sys = mXmbSystems[s];
        Ps3Item it;
        it.label = (r >= 0 && r < (int)sys.displayNames.size()) ? sys.displayNames[r] : romPath;
        it.kind = PS3_ROM; it.a = s; it.b = r;
        GLuint tex = 0, nmap = 0; resolveSystemIcon(sys.iconRef, &tex, &nmap);
        it.iconTex = tex; it.nmapTex = nmap;
        it.iconR = sys.iconR; it.iconG = sys.iconG; it.iconB = sys.iconB;
        out.items.push_back(it);
    }
    if (out.items.empty()) {
        Ps3Item it; it.label = "There are no titles"; it.kind = PS3_DATA_LEAF; it.action = 0;
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
}

int NanoMenu::collectionCreate(const std::string& name) {
    if (name.empty()) return -1;
    for (size_t i = 0; i < mXmbCollections.size(); i++)
        if (esdeStoreMode() ? strcasecmp(esdeCollectionFileName(mXmbCollections[i].name).c_str(), esdeCollectionFileName(name).c_str()) == 0
                            : mXmbCollections[i].name == name)
            return (int)i;   // reuse an existing name (in the ES-DE folder, as the file it is kept in)
    XmbCollection c; c.name = name;
    mXmbCollections.push_back(c);
    saveCollections();
    return (int)mXmbCollections.size() - 1;
}

void NanoMenu::collectionAddRom(int colIdx, const std::string& romPath) {
    if (colIdx < 0 || colIdx >= (int)mXmbCollections.size() || romPath.empty()) return;
    auto& roms = mXmbCollections[colIdx].roms;
    const std::string key = hiddenKey(romPath);
    for (const auto& r : roms) if (hiddenKey(r) == key) return;   // already present, in any spelling
    roms.push_back(romPath);
    saveCollections();
}

void NanoMenu::collectionRemoveRom(int colIdx, const std::string& romPath) {
    if (colIdx < 0 || colIdx >= (int)mXmbCollections.size()) return;
    auto& roms = mXmbCollections[colIdx].roms;
    const std::string key = hiddenKey(romPath);
    roms.erase(std::remove_if(roms.begin(), roms.end(), [&](const std::string& r) { return hiddenKey(r) == key; }),
               roms.end());
    saveCollections();
}

// ---- Favourites (a single global, cross-system starred-games list) --------------------------
static const char* kFavoritesFile = "/data/system/nano_favorites.txt";
static const char* kEsdeUnfavoritesFile = "/data/system/nano_esde_unfavorites.txt";

static void loadPathList(const char* file, std::vector<std::string>& out) {
    out.clear();
    FILE* f = fopen(file, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0) continue;
        out.push_back(line);
    }
    fclose(f);
}

static void savePathList(const char* file, const std::vector<std::string>& list) {
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    chmod(file, 0644);
    for (const auto& r : list) {
        std::string ln = r + "\n";
        ssize_t w = write(fd, ln.c_str(), ln.size()); (void)w;
    }
    close(fd);
}

void NanoMenu::loadFavorites() {
    if (esdeStoreMode()) {
        std::map<std::string, std::string> hidden;
        esdeStoreLoadFlags(&mXmbFavorites, &hidden);
        mEsdeUnfavorites.clear();
        mXmbFavoriteKeys.clear();
        mEsdeStoreFavs.clear();
        for (const auto& r : mXmbFavorites) {
            mXmbFavoriteKeys.insert(hiddenKey(r));
            mEsdeStoreFavs[hiddenKey(r)] = r;
        }
        return;
    }
    loadPathList(kFavoritesFile, mXmbFavorites);
    loadPathList(kEsdeUnfavoritesFile, mEsdeUnfavorites);
    // One entry per file: older builds could store a game under two spellings.
    mXmbFavoriteKeys.clear();
    const size_t before = mXmbFavorites.size();
    mXmbFavorites.erase(std::remove_if(mXmbFavorites.begin(), mXmbFavorites.end(),
                                       [&](const std::string& r) { return !mXmbFavoriteKeys.insert(hiddenKey(r)).second; }),
                        mXmbFavorites.end());
    if (mXmbFavorites.size() != before) saveFavorites();
}

void NanoMenu::saveFavorites() {
    if (esdeStoreMode()) {
        std::map<std::string, std::string> now;
        for (const auto& r : mXmbFavorites) now[hiddenKey(r)] = r;
        esdeStoreSaveFlag("favorite", now, mEsdeStoreFavs);
        return;
    }
    savePathList(kFavoritesFile, mXmbFavorites);
    savePathList(kEsdeUnfavoritesFile, mEsdeUnfavorites);
}

// The ES-DE theme's favourite state: nano's own list, or the imported ES-DE <favorite> unless the
// user switched it off.
bool NanoMenu::esdeIsFavorite(const std::string& romPath, const ScrapeEntry* meta) const {
    if (isFavorite(romPath)) return true;
    if (esdeStoreMode()) return false;   // the favourites are the gamelists' own
    if (!meta || !meta->favorite) return false;
    for (const auto& r : mEsdeUnfavorites) if (r == romPath) return false;
    return true;
}

// Favourites are matched by file, not by spelling: Recently Played (and older builds) store the
// /sdcard spelling of a /data/media/0 path (hiddenKey).
bool NanoMenu::isFavorite(const std::string& romPath) const {
    return !romPath.empty() && mXmbFavoriteKeys.count(hiddenKey(romPath)) > 0;
}

void NanoMenu::toggleFavorite(const std::string& romPath) {
    if (romPath.empty()) return;
    const std::string key = hiddenKey(romPath);
    const size_t before = mXmbFavorites.size();
    mXmbFavorites.erase(std::remove_if(mXmbFavorites.begin(), mXmbFavorites.end(),
                                       [&](const std::string& r) { return hiddenKey(r) == key; }),
                        mXmbFavorites.end());
    if (mXmbFavorites.size() == before) { mXmbFavorites.push_back(romPath); mXmbFavoriteKeys.insert(key); }
    else mXmbFavoriteKeys.erase(key);
    saveFavorites();
}

// The global Favourites list: resolve each stored path to a live (system, rom) index so a
// favourited game reuses the normal PS3_ROM launch / boxart / option-menu path, exactly like a
// collection's game list. Games whose system is gone/disabled are skipped.
void NanoMenu::buildFavoritesSubmenu(Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.title = "Favorites";
    for (const auto& romPath : mXmbFavorites) {
        int s = -1, r = -1;
        if (!collectionResolveRom(romPath, &s, &r)) continue;
        const XmbSystem& sys = mXmbSystems[s];
        Ps3Item it;
        it.label = (r >= 0 && r < (int)sys.displayNames.size()) ? sys.displayNames[r] : romPath;
        it.kind = PS3_ROM; it.a = s; it.b = r;
        GLuint tex = 0, nmap = 0; resolveSystemIcon(sys.iconRef, &tex, &nmap);
        it.iconTex = tex; it.nmapTex = nmap;
        it.iconR = sys.iconR; it.iconG = sys.iconG; it.iconB = sys.iconB;
        out.items.push_back(it);
    }
    if (out.items.empty()) {
        Ps3Item it; it.label = "There are no titles"; it.kind = PS3_DATA_LEAF; it.action = 0;
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
}

// ---- Hidden games (taken out of every game list without deleting the file) ------------------
static const char* kHiddenGamesFile = "/data/system/nano_hidden_games.txt";

// One key per game whichever mount path it was reached through: the primary storage is seen as
// /data/media/0, /storage/emulated/0 and /sdcard, and a card as /mnt/media_rw/X or /storage/X
// (Recently Played stores the app-visible form, the scan the raw one).
static const std::pair<const char*, const char*> kPathAlias[] = {
    { "/data/media/0/", "/sdcard/" },
    { "/storage/emulated/0/", "/sdcard/" },
    { "/storage/self/primary/", "/sdcard/" },
    { "/mnt/media_rw/", "/storage/" },
};

std::string NanoMenu::hiddenKey(const std::string& romPath) {
    for (const auto& a : kPathAlias) {
        const size_t n = strlen(a.first);
        if (romPath.compare(0, n, a.first) == 0) return std::string(a.second) + romPath.substr(n);
    }
    return romPath;
}

// hiddenKey(a) == hiddenKey(b), without building either.
bool NanoMenu::sameFile(const std::string& a, const std::string& b) {
    auto split = [](const std::string& p, const char** pre, size_t* cut) {
        for (const auto& al : kPathAlias) {
            const size_t n = strlen(al.first);
            if (p.compare(0, n, al.first) == 0) { *pre = al.second; *cut = n; return; }
        }
        *pre = ""; *cut = 0;
    };
    const char *pa, *pb;
    size_t ca, cb;
    split(a, &pa, &ca);
    split(b, &pb, &cb);
    const size_t la = strlen(pa), lb = strlen(pb);
    if (la + a.size() - ca != lb + b.size() - cb) return false;
    for (size_t i = 0, n = la + a.size() - ca; i < n; i++) {
        const char x = i < la ? pa[i] : a[ca + i - la];
        const char y = i < lb ? pb[i] : b[cb + i - lb];
        if (x != y) return false;
    }
    return true;
}

bool NanoMenu::isHiddenGame(const std::string& romPath) const {
    return !romPath.empty() && mHiddenGames.count(hiddenKey(romPath)) != 0;
}

bool NanoMenu::loadHiddenGames() {
    std::map<std::string, std::string> loaded;
    if (esdeStoreMode()) {
        std::vector<std::string> favs;
        esdeStoreLoadFlags(&favs, &loaded);
        mEsdeStoreHidden = loaded;
    } else if (FILE* f = fopen(kHiddenGamesFile, "re")) {
        char line[4096];
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (n == 0) continue;
            loaded[hiddenKey(line)] = line;
        }
        fclose(f);
    }
    if (loaded == mHiddenGames) return false;
    mHiddenGames.swap(loaded);
    return true;
}

void NanoMenu::saveHiddenGames() {
    if (esdeStoreMode()) {
        esdeStoreSaveFlag("hidden", mHiddenGames, mEsdeStoreHidden);
        return;
    }
    // Written beside the real file and renamed over it, so a power cut mid-write can never leave
    // a truncated list that brings every hidden game back (or a half path that hides nothing).
    const std::string tmp = std::string(kHiddenGamesFile) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { ALOGE("NanoMenu: cannot write %s (errno %d)", tmp.c_str(), errno); return; }
    bool ok = true;
    for (const auto& kv : mHiddenGames) {
        const std::string ln = kv.second + "\n";
        if (write(fd, ln.c_str(), ln.size()) != (ssize_t)ln.size()) ok = false;
    }
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok || rename(tmp.c_str(), kHiddenGamesFile) != 0) {
        ALOGE("NanoMenu: saving the hidden games list failed (errno %d)", errno);
        unlink(tmp.c_str());
        return;
    }
    chmod(kHiddenGamesFile, 0644);
}

bool NanoMenu::dropHiddenRecents() {
    if (mHiddenGames.empty()) return false;
    const size_t before = mXmbRecent.size();
    mXmbRecent.erase(std::remove_if(mXmbRecent.begin(), mXmbRecent.end(),
                                    [this](const XmbRecentEntry& e) { return isHiddenGame(e.romPath); }),
                     mXmbRecent.end());
    return mXmbRecent.size() != before;
}

// Re-filter every system and rebuild whatever list is open, so a hidden game disappears at once.
// Rows of the open game lists carry indices into the system lists, which have just shifted, so
// every kind of game list on the stack is rebuilt, not only the one the game was hidden from.
void NanoMenu::refreshListsAfterHide() {
    for (size_t s = 0; s < mXmbSystems.size(); s++) {
        XmbSystem& sys = mXmbSystems[s];
        const size_t before = sys.roms.size();
        applyRomNameOverrides(sys);
        if (sys.roms.size() == before) continue;
        sys.pathExists = !sys.roms.empty();
        rebuildOpenRomLevels((int)s, std::string());
    }
    if (dropHiddenRecents()) saveXmbRecent();
    for (auto& lvl : mPs3Stack) {
        int keep = lvl.sel;
        if (lvl.collectionIdx >= 0) buildCollectionSubmenu(lvl.collectionIdx, lvl);
        else if (lvl.title == "Favorites") buildFavoritesSubmenu(lvl);
        else if (!lvl.items.empty() && lvl.items[0].kind == PS3_RECENT) buildRecentSubmenu(lvl);
        else continue;
        const int n = (int)lvl.items.size();
        if (keep >= n) keep = n - 1;
        lvl.sel = keep < 0 ? 0 : keep;
    }
    mPs3CatsStale = true;   // per-system game counts, and a system whose last game was hidden
    mDisplayDirty = true;
}

void NanoMenu::hideGame(const std::string& romPath) {
    if (romPath.empty()) return;
    if (!mHiddenGames.emplace(hiddenKey(romPath), romPath).second) return;
    saveHiddenGames();
    ALOGI("NanoMenu: hid %s", romPath.c_str());
    refreshListsAfterHide();
}

// The files a game is: its file, and the ones a .cue, .gdi, .ccd or .m3u names in its folder (a
// disc's tracks, a multi-disc game's discs and their tracks).
static void gameFilesOf(const std::string& path, std::vector<std::string>* out, int depth = 0) {
    if (std::find(out->begin(), out->end(), path) != out->end()) return;
    out->push_back(path);
    if (depth > 2) return;
    const size_t sl = path.find_last_of('/'), dot = path.find_last_of('.');
    if (sl == std::string::npos || dot == std::string::npos || dot < sl) return;
    const std::string dir = path.substr(0, sl);
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    auto add = [&](std::string name) {
        if (name.compare(0, 3, "\xef\xbb\xbf") == 0) name.erase(0, 3);   // a UTF-8 BOM
        while (!name.empty() && strchr("\r\n\t \"", name.back())) name.pop_back();
        while (!name.empty() && strchr("\t \"", name.front())) name.erase(name.begin());
        for (auto& c : name) if (c == '\\') c = '/';
        while (name.compare(0, 2, "./") == 0) name.erase(0, 2);
        // Only beside it or below it.
        if (name.empty() || name[0] == '/') return;
        for (size_t p = 0; p <= name.size();) {
            size_t q = name.find('/', p);
            if (q == std::string::npos) q = name.size();
            if (name.compare(p, q - p, "..") == 0 && q - p == 2) return;
            p = q + 1;
        }
        const std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) gameFilesOf(full, out, depth + 1);
    };
    if (ext == "ccd") {
        const std::string stem = path.substr(sl + 1, dot - sl - 1);
        for (const char* e : {".img", ".sub"}) add(stem + e);
        return;
    }
    if (ext == "m3u8") ext = "m3u";
    if (ext != "cue" && ext != "gdi" && ext != "m3u") return;
    std::ifstream f(path);
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (ext == "m3u") {
            if (first && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
            first = false;
            if (!line.empty() && line[0] != '#') add(line);
        } else if (ext == "cue") {
            size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos || strncasecmp(line.c_str() + p, "FILE", 4) != 0) continue;
            const size_t q1 = line.find('"', p), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
            if (q2 != std::string::npos) add(line.substr(q1 + 1, q2 - q1 - 1));
            else {   // FILE name.bin BINARY
                std::string rest = line.substr(p + 4);
                const size_t s = rest.find_first_not_of(" \t"), e = rest.find_last_of(" \t");
                if (s != std::string::npos && e != std::string::npos && e > s) add(rest.substr(s, e - s));
            }
        } else {   // gdi: a track count, then "<n> <lba> <type> <size> <file> <offset>"
            if (first) { first = false; continue; }
            const size_t q1 = line.find('"');
            if (q1 != std::string::npos) {
                const size_t q2 = line.find('"', q1 + 1);
                if (q2 != std::string::npos) add(line.substr(q1 + 1, q2 - q1 - 1));
                continue;
            }
            char name[512];
            if (sscanf(line.c_str(), "%*d %*d %*d %*d %511s", name) == 1) add(name);
        }
    }
}

void NanoMenu::gameDeleteOffer(int sysIdx, const std::string& romPath, const std::string& name) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size() || romPath.empty()) return;
    if (nanoscummvm::isMirrorPath(romPath)) {
        showXmbMessage(trDyn("Remove this game in ScummVM"),
                       trDyn("ScummVM's games come from its own game list. Open ScummVM, select the game and "
                             "choose Remove Game; its files stay where they are."), 300);
        return;
    }
    std::vector<std::string> files;
    gameFilesOf(romPath, &files);
    mGameDeletePath = romPath;
    mGameDeleteSys = sysIdx;
    const size_t sl = romPath.find_last_of('/');
    char body[900];
    if (files.size() > 1)
        snprintf(body, sizeof(body), trDyn("%s and the %zu files it uses will be deleted from %s. Delete its media too: "
                                           "cover, fan art, screenshots, videos and the rest?"),
                 name.c_str(), files.size() - 1, romPath.substr(0, sl).c_str());
    else
        snprintf(body, sizeof(body), trDyn("%s will be deleted from %s. Delete its media too: cover, fan art, "
                                           "screenshots, videos and the rest?"),
                 name.c_str(), romPath.substr(0, sl).c_str());
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 1; mPs3DlgThemeKey = 66; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    mPs3DlgTitle = "Delete Game"; mPs3DlgBody = body;
    mPs3DlgOptions.push_back("Cancel");                mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Delete Game");           mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Delete Game and Media"); mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(25);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
}

void NanoMenu::gameDelete(bool withMedia) {
    const std::string rom = mGameDeletePath;
    int sysIdx = mGameDeleteSys;
    mGameDeletePath.clear();
    mGameDeleteSys = -1;
    if (rom.empty() || sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    XmbSystem& sys = mXmbSystems[sysIdx];
    auto ri = std::find(sys.roms.begin(), sys.roms.end(), rom);
    if (ri == sys.roms.end()) return;   // rescanned meanwhile
    const std::string name = (size_t)(ri - sys.roms.begin()) < sys.displayNames.size()
                                 ? sys.displayNames[ri - sys.roms.begin()] : romDisplayName(rom);

    // The game's files first: nothing else goes when it cannot (a read-only card or share).
    std::vector<std::string> files;
    gameFilesOf(rom, &files);
    if (unlink(rom.c_str()) != 0 && errno != ENOENT) {
        char msg[300];
        snprintf(msg, sizeof(msg), trDyn("%s could not be deleted (the folder may be read-only)."), name.c_str());
        photoShowBanner(msg);
        ALOGW("NanoMenu: delete %s failed (errno %d)", rom.c_str(), errno);
        return;
    }
    int failed = 0;
    for (size_t i = 1; i < files.size(); i++)
        if (unlink(files[i].c_str()) != 0 && errno != ENOENT) failed++;
    // A game in a folder of its own: the folder too, once empty. Only one strictly below a system
    // folder (the scanned ones, or the ones a system lists before its scan).
    {
        const std::string dir = rom.substr(0, rom.find_last_of('/'));
        std::vector<std::string> roots = esdeSystemRoots(sysIdx);
        roots.insert(roots.end(), sys.activePaths.begin(), sys.activePaths.end());
        const std::string dk = hiddenKey(dir);
        bool below = false, isRoot = false;
        for (const auto& r : roots) {
            const std::string rk = hiddenKey(r);
            if (rk.empty()) continue;
            if (dk == rk) isRoot = true;
            else if (dk.size() > rk.size() && dk.compare(0, rk.size(), rk) == 0 && dk[rk.size()] == '/') below = true;
        }
        if (below && !isRoot) rmdir(dir.c_str());
    }

    // The media, the lookups going by the game's folder and name (the game is still listed).
    int media = 0;
    scraperEnsureLoaded();
    if (withMedia) {
        media = esdeDeleteGameMedia(rom, sysIdx, files);
        // Nano's own cache copies (the cover in the ES-DE folder went above, unless another game has it).
        for (const auto& f : files) {
            const ScrapeEntry* se = scrapeEntryFor(f);
            if (se)
                for (const std::string* p : {&se->box, &se->fan})
                    if (!p->empty() && p->compare(0, mScrapeCacheDir.size() + 1, mScrapeCacheDir + "/") == 0
                        && unlink(p->c_str()) == 0)
                        media++;
            for (const char* suffix : {".box.png", ".fan.jpg"})
                if (unlink((mScrapeCacheDir + "/" + nanoscraper::cacheKey(f) + suffix).c_str()) == 0) media++;
        }
    }
    ALOGI("NanoMenu: deleted %s (%zu files, %d media)", rom.c_str(), files.size(), media);

    // Its records, for each of its files (a playlist's discs may have their own).
    bool favs = false, recents = false, names = false, index = false;
    for (const auto& f : files) {
        const std::string key = hiddenKey(f);
        if (isFavorite(f)) { toggleFavorite(f); favs = true; }
        auto un = std::find(mEsdeUnfavorites.begin(), mEsdeUnfavorites.end(), f);
        if (un != mEsdeUnfavorites.end()) { mEsdeUnfavorites.erase(un); favs = true; }
        for (size_t c = 0; c < mXmbCollections.size(); c++) {
            const auto& cr = mXmbCollections[c].roms;
            if (std::any_of(cr.begin(), cr.end(), [&](const std::string& r) { return hiddenKey(r) == key; }))
                collectionRemoveRom((int)c, f);
        }
        const size_t before = mXmbRecent.size();
        mXmbRecent.erase(std::remove_if(mXmbRecent.begin(), mXmbRecent.end(),
                                        [&](const XmbRecentEntry& e) { return hiddenKey(e.romPath) == key; }),
                         mXmbRecent.end());
        recents = recents || mXmbRecent.size() != before;
        if (romNameOverrideFor(f)) { clearRomNameOverride(f); names = true; }
        if (gameEmulatorFor(f)) setGameEmulator(f, nullptr);
        if (mScrapeIndex.erase(f)) index = true;
        esdeGamelistRemove(f);
        mEsdeGamelistMeta.erase(f);
        esdeForgetGameMedia(f);
    }
    if (favs) saveFavorites();
    if (recents) saveXmbRecent();
    if (names) saveRomNameOverrides();
    if (index) saveScrapeIndex();

    // Out of every system that lists it (a folder two systems scan), their saved lists, and every
    // open list.
    for (size_t s = 0; s < mXmbSystems.size(); s++) {
        XmbSystem& os = mXmbSystems[s];
        bool changed = false;
        for (const auto& f : files) {
            auto fi = std::find(os.roms.begin(), os.roms.end(), f);
            if (fi == os.roms.end()) continue;
            const size_t at = fi - os.roms.begin();
            os.roms.erase(fi);
            if (at < os.displayNames.size()) os.displayNames.erase(os.displayNames.begin() + at);
            changed = true;
        }
        if (!changed) continue;
        os.pathExists = !os.roms.empty();
        const std::string cachePath = xmbCachePath(os), tmp = cachePath + ".tmp";
        const int cfd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (cfd >= 0) {
            bool ok = true;
            for (const auto& r : os.roms) {
                const std::string line = r + "\n";
                if (write(cfd, line.c_str(), line.size()) != (ssize_t)line.size()) ok = false;
            }
            ok = close(cfd) == 0 && ok;
            if (!ok || rename(tmp.c_str(), cachePath.c_str()) != 0) unlink(tmp.c_str());
        }
        rebuildOpenRomLevels((int)s, std::string());
        if (mEsdeSysSel >= 0 && mEsdeSysSel < (int)mEsdeSysList.size() && mEsdeSysList[mEsdeSysSel] == (int)s
            && mEsdeGameSel >= (int)os.roms.size())
            mEsdeGameSel = os.roms.empty() ? 0 : (int)os.roms.size() - 1;
    }
    refreshListsAfterHide();
    if (!mPs3Stack.empty() && isGameOptionsLevel(mPs3Stack.back()) && !gameOptResolve()) mPs3Stack.pop_back();

    char msg[300];
    if (failed > 0)
        snprintf(msg, sizeof(msg), trDyn("%s was deleted, but %d of its files could not be."), name.c_str(), failed);
    else if (withMedia)
        snprintf(msg, sizeof(msg), trDyn("%s and its media were deleted"), name.c_str());
    else
        snprintf(msg, sizeof(msg), trDyn("%s was deleted"), name.c_str());
    photoShowBanner(msg);
    mPs3CatsStale = true;
    mDisplayDirty = true;
}

void NanoMenu::unhideGame(const std::string& key) {
    if (!mHiddenGames.erase(key)) return;
    saveHiddenGames();
    ALOGI("NanoMenu: unhid %s", key.c_str());
    // The lists no longer hold the game at all: a rescan finds it again and the results go
    // through the normal path (counts, open lists, the ROM cache).
    forceRescanAllSystems();
    mPs3CatsStale = true;
    mDisplayDirty = true;
}

// Settings > Game Settings > Hidden Games: every hidden game with the system it belongs to.
// Selecting one shows it again.
void NanoMenu::buildHiddenGamesSubmenu(Ps3Level& out) {
    out.items.clear(); out.sel = 0; out.title = "Hidden Games";
    std::vector<Ps3Item> rows;
    for (const auto& kv : mHiddenGames) {
        const std::string& path = kv.second;
        Ps3Item it;
        const std::string* ov = romNameOverrideFor(path);
        const ScrapeEntry* se = ov && !ov->empty() ? nullptr : scrapeEntryFor(path);
        it.label = ov && !ov->empty() ? *ov : (se && !se->title.empty() ? se->title : romDisplayName(path));
        it.kind = PS3_HIDDEN_GAME;
        it.payloadStr = kv.first;
        it.desc = "Select to show this game again.";
        it.iconTex = iconTexForIcon(22); it.nmapTex = nmapForIcon(22);
        it.iconR = it.iconG = it.iconB = 1.0f;
        // The system: the one whose ROM folder holds the file.
        const std::string key = kv.first;
        for (const auto& sys : mXmbSystems) {
            bool mine = false;
            for (const auto& ap : sys.activePaths) {
                const std::string k = hiddenKey(ap + "/");
                if (!ap.empty() && key.compare(0, k.size(), k) == 0) { mine = true; break; }
            }
            if (!mine) continue;
            it.value = sys.shortname;
            it.desc = xmbFmt(trDyn("%s. Select to show this game again."),
                             (sys.name.empty() ? sys.shortname : sys.name).c_str());
            GLuint tex = 0, nmap = 0; resolveSystemIcon(sys.iconRef, &tex, &nmap);
            if (tex) { it.iconTex = tex; it.nmapTex = nmap; it.iconR = sys.iconR; it.iconG = sys.iconG; it.iconB = sys.iconB; }
            break;
        }
        rows.push_back(it);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Ps3Item& a, const Ps3Item& b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    out.items = std::move(rows);
    if (out.items.empty()) {
        Ps3Item it; it.label = "There are no hidden games"; it.kind = PS3_DATA_LEAF; it.action = 0;
        it.iconTex = 0; it.nmapTex = 0; it.iconR = it.iconG = it.iconB = 1.0f;
        out.items.push_back(it);
    }
}

// True when a ROM is actually present and launchable. A game whose file has been deleted or
// whose card is not mounted used to be handed to the emulator anyway, which then died on the
// missing file and looked like a crashed launch, so every launch path checks this first.
//
// Only real filesystem paths are checked. A content:// URI is resolved by the framework's SAF
// layer, not by us, and stat() on one always fails, so those are treated as present and left to
// the launch itself. An empty path is not launchable either.
bool NanoMenu::romFileExists(const std::string& romPath) {
    if (romPath.empty()) return false;
    if (romPath.rfind("content://", 0) == 0) return true;
    // A ROM on a network share is assumed present rather than stat-ed.
    //
    // This is called per entry from pruneStaleRecentEntries() on the render thread, so a Recently
    // Played list holding a few share-hosted games turns into that many round trips before a frame
    // can be drawn - and nano's watchdog aborts the process at 8s. The check exists to drop paths
    // that have genuinely gone from local storage; a share that is merely slow or briefly
    // unreachable is not the same thing, and silently deleting the user's history because their NAS
    // was asleep would be worse than the alternative. If the file really is gone, the launch fails
    // and showRomMissingMsg() says so, which is where the user finds out either way.
    if (romPath.rfind("/mnt/shares/", 0) == 0) return true;
    struct stat st;
    if (stat(romPath.c_str(), &st) != 0) return false;
    // A directory is not a ROM, and a zero-byte file is a failed copy rather than a game.
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

// Launch-time existence check. romFileExists() assumes a /mnt/shares network path is present so the
// per-frame pruner never blocks on a sleeping NAS, but that let a Recently Played row for a game
// deleted server-side sail straight into the emulator, which then black-screened on the missing
// file. At launch we can afford to actually stat the share: it runs once, for the one game being
// started, and a server that removed the file answers "not found" quickly. A genuinely dead NAS
// would fail the launch either way. content:// is left to the framework; an empty path is not
// launchable.
bool NanoMenu::romLaunchExists(const std::string& romPath) {
    if (romPath.empty()) return false;
    if (romPath.rfind("content://", 0) == 0) return true;
    struct stat st;
    if (stat(romPath.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

// True when the folder holding romPath is itself reachable. Used to tell a genuinely deleted file
// (folder present, file gone) from a share/card that is simply offline (folder unreachable): the
// former is safe to prune from Recently Played, the latter must be kept so a briefly-down NAS does
// not wipe the user's history.
bool NanoMenu::romParentDirReachable(const std::string& romPath) {
    size_t slash = romPath.rfind('/');
    if (slash == std::string::npos || slash == 0) return false;
    std::string dir = romPath.substr(0, slash);
    struct stat st;
    return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Drop a single Recently Played row after a launch-time stat proved its file is gone. Unlike
// pruneStaleRecentEntries() this removes a share-hosted entry too, because here we have positive
// proof (a live stat, not the assume-present render-safe check) rather than a possibly-asleep NAS.
void NanoMenu::recentRemoveAt(int idx) {
    if (idx < 0 || idx >= (int)mXmbRecent.size()) return;
    mXmbRecent.erase(mXmbRecent.begin() + idx);
    saveXmbRecent();
    if (mXmbGameIndex >= (int)mXmbRecent.size())
        mXmbGameIndex = mXmbRecent.empty() ? 0 : (int)mXmbRecent.size() - 1;
    mPs3CatsStale = true;
    mDisplayDirty = true;
}

// Tell the user why nothing launched. Kept in one place so every launch path says the same thing.
// A tab or line break in a file name cannot be carried in the launch intent file, which
// separates its tokens with tabs.
void NanoMenu::showUnlaunchableNameMsg(const std::string& displayName) {
    ALOGW("NanoMenu: refusing to launch, file name has a tab or line break (%s)", displayName.c_str());
    showXmbMessage(trDyn("Cannot launch this game"),
                   trDyn("Its file name contains a tab or line break. Rename the file and try again."), 260);
}

bool NanoMenu::launchFileIdFor(const std::string& tmpl, const std::string& args,
                               const std::string& romPath, const std::string& displayName,
                               std::string& id) {
    id.clear();
    if (!nanolaunch::usesFileContent(tmpl, args)) return true;
    id = nanolaunch::readLaunchFileId(romPath);
    if (!id.empty()) return true;
    ALOGW("NanoMenu: refusing to launch, no game ID in %s (%s)", romPath.c_str(), displayName.c_str());
    showXmbMessage(trDyn("Cannot launch this game"),
                   trDyn("Its file must hold the game's ID on its first line, such as sky for Beneath a Steel Sky in ScummVM."),
                   260);
    return false;
}

void NanoMenu::showRomMissingMsg(const std::string& displayName) {
    ALOGW("NanoMenu: refusing to launch, ROM missing (%s)", displayName.c_str());
    showXmbMessage(displayName.empty() ? std::string(trDyn("Game not found"))
                                       : xmbFmt(trDyn("%s not found"), displayName.c_str()),
                   trDyn("It may have been deleted, or its storage is not connected."), 260);
}

// True when the RetroArch libretro core .so for a system is present on disk. RetroArch keeps its
// cores under its own data dir; this is the exact path every launch branch feeds RetroArch. Fails
// OPEN (returns true) for an empty coreSo so a non-core launch type is never blocked here.
bool NanoMenu::coreSoExists(const std::string& coreSo) {
    if (coreSo.empty()) return true;   // not a libretro-core launch; nothing to check
    std::string corePath = "/data/data/com.retroarch.aarch64/cores/" + coreSo;
    struct stat st;
    if (stat(corePath.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode) && st.st_size > 0;
}

// True when a standalone emulator package is installed. Reads /data/system/packages.list directly
// (the authoritative DB; the package name is the first space-delimited token of each line) rather
// than mAppEntries, which drops system/com.android.*/com.gammaos.* packages an emulator may live in.
// Fails OPEN (returns true) if the list is unreadable, so a transient state never blocks a launch.
bool NanoMenu::packageInstalled(const std::string& pkg) {
    if (pkg.empty()) return true;
    // DS games route to drastic-nano, which runs libdrastic from /system and no
    // longer needs the DraStic APK: the system copy counts as "installed".
    if (pkg == "com.dsemu.drastic" &&
        property_get_bool("persist.gammaos.nano.drastic_nano", false) &&
        access("/system/lib64/libdrastic_arm64.so", R_OK) == 0)
        return true;
    int fd = open("/data/system/packages.list", O_RDONLY);
    if (fd < 0) return true;   // fail-open: never block a launch if the DB cannot be read
    std::string content;
    char b[4096];
    ssize_t n;
    while ((n = read(fd, b, sizeof(b))) > 0) content.append(b, (size_t)n);
    close(fd);
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        size_t sp = content.find(' ', pos);
        if (sp != std::string::npos && sp <= eol &&
            content.compare(pos, sp - pos, pkg) == 0)
            return true;
        pos = eol + 1;
    }
    return false;
}

// Tell the user a game's emulator/core is not on the device, instead of launching into a black
// screen. Theme-agnostic (renders via the same toast path as showRomMissingMsg).
void NanoMenu::showEmuMissingMsg(const std::string& displayName, bool standalone) {
    ALOGW("NanoMenu: refusing to launch, emulator missing (%s, standalone=%d)",
          displayName.c_str(), standalone ? 1 : 0);
    showXmbMessage(displayName.empty() ? std::string(trDyn("Emulator not installed"))
                                       : xmbFmt(trDyn("%s: emulator not installed"), displayName.c_str()),
                   standalone ? trDyn("Install the required emulator app, then try again.")
                              : trDyn("Download its core in RetroArch: Online Updater > Core Downloader."), 300);
}

// Drop Recently Played entries whose ROM file is gone, so a rescan (or a deleted game) does not
// leave rows that cannot launch. Only rewrites the list file when something actually changed.
void NanoMenu::pruneStaleRecentEntries() {
    size_t before = mXmbRecent.size();
    for (auto it = mXmbRecent.begin(); it != mXmbRecent.end();) {
        if (romFileExists(it->romPath)) ++it;
        else it = mXmbRecent.erase(it);
    }
    if (mXmbRecent.size() != before) {
        ALOGI("NanoMenu: pruned %zu stale Recently Played entries",
              before - mXmbRecent.size());
        saveXmbRecent();
        if (mXmbGameIndex >= (int)mXmbRecent.size()) {
            mXmbGameIndex = mXmbRecent.empty() ? 0 : (int)mXmbRecent.size() - 1;
        }
        mPs3CatsStale = true;   // the Recently Played submenu must be rebuilt
        mDisplayDirty = true;
    }
}

// User-triggered "Rescan Games": re-read every enabled system's ROM folders. The background scan
// rebuilds each system's list from disk rather than merging, so games that have been deleted drop
// out on their own; the Recently Played list is pruned when the results land (see the scan drain).
void NanoMenu::gamesRefresh() {
    // Prune deleted games verbatim when the results land, even if a scan is already mid-flight
    // (so a rescan requested while the boot/periodic scan is running is not silently lost - the
    // "had to refresh twice" report). The running scan's results then prune without the guard.
    mRecentPrunePending = true;
    if (mBgScanThreadRunning) {
        showXmbMessage(trDyn("Scanning for games..."), trDyn("Deleted games will be removed"), 200);
        mDisplayDirty = true;
        return;
    }
    ALOGI("NanoMenu: user-triggered game rescan");
    forceRescanAllSystems();
    showXmbMessage(trDyn("Scanning for games..."), trDyn("Deleted games will be removed"), 200);
    mDisplayDirty = true;
}

void NanoMenu::addXmbRecent(int sysIdx, int gameIdx) {
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    const auto& sys = mXmbSystems[sysIdx];
    if (gameIdx < 0 || gameIdx >= (int)sys.roms.size()) return;
    esdeGamelistPlayed(sys.roms[gameIdx]);
    XmbRecentEntry e = recentEntryFor(sysIdx, gameIdx);

    // Remove duplicate if already in list
    for (auto it = mXmbRecent.begin(); it != mXmbRecent.end(); ++it) {
        if (it->romPath == e.romPath) {
            mXmbRecent.erase(it);
            break;
        }
    }
    // Insert at front (most recent first)
    mXmbRecent.insert(mXmbRecent.begin(), std::move(e));
    // Cap size
    if ((int)mXmbRecent.size() > mXmbRecentMax) {
        mXmbRecent.resize(mXmbRecentMax);
    }
    saveXmbRecent();
}

// A system's game as Recently Played records it.
NanoMenu::XmbRecentEntry NanoMenu::recentEntryFor(int sysIdx, int gameIdx) {
    const auto& sys = mXmbSystems[sysIdx];
    XmbRecentEntry e;
    // roms[] contains full paths -- use directly, convert for app access
    e.romPath = sys.roms[gameIdx];
    if (e.romPath.find("/data/media/0/") == 0) {
        e.romPath = "/sdcard/" + e.romPath.substr(14);
    }
    if (e.romPath.find("/mnt/media_rw/") == 0) {
        // Convert raw SD path to FUSE path for app access
        e.romPath = "/storage/" + e.romPath.substr(14);
    }
    e.coreSo = sys.coreSo;
    e.launchPkg = sys.launchPkg;
    e.launchIntent = sys.launchIntent;
    e.displayName = gameIdx < (int)sys.displayNames.size() ? sys.displayNames[gameIdx] : romDisplayName(e.romPath);
    e.systemName = sys.shortname;
    e.romDir = sys.romDir;
    e.standalone = sys.isStandalone();
    // A game with its own emulator (Game Options > Emulator) is recorded with that one.
    { XmbSystem g;
      if (applyGameEmulator(e.romPath, g)) {
          e.coreSo = g.coreSo; e.launchPkg = g.launchPkg; e.launchIntent = g.launchIntent;
          e.standalone = g.isStandalone();
      } }
    return e;
}

// ---------------------------------------------------------------------------
// XMB Navigation
// ---------------------------------------------------------------------------

void NanoMenu::handleLeft() {
    if (biosDlgShown()) { if (!mSetupTransitioning || !mBiosDlgSetup) biosDlgHoriz(-1); return; }   // BIOS dialog: its buttons
    if (esdeOverlayNav(NavDir::Left)) return;   // ES-DE media viewer / game information
    if (mScrapeProgActive) return;   // modal swallows navigation
    // GammaOS Nano: navigating cancels any queued launch.
    cancelPendingLaunch();
    if (mSetupWizardActive && mMenuState == MENU_SETUP_WIZARD && !mPs3WizActive
        && mSetupStep == SETUP_DATETIME) {
        setupDateTimeCycle(-1); return;
    }
    if (mSetupWizardActive && mMenuState == MENU_SETUP_WIZARD && !mPs3WizActive
        && mSetupStep == SETUP_SDCARD && mSetupSdSel < 2 && !mSetupTransitioning) {
        setupSdCardSelect(); return;   // On <-> Off
    }
    if (mOskActive) {
        if (!oskButtonLeavesTouch()) oskMoveCursor(NavDir::Left);
        return;
    }
    if (mMenuState == MENU_WIFI || mMenuState == MENU_BT) return;
    if (mMenuState == MENU_SETTINGS) { handleSettingsTreeLeft(); return; }
    if (mPs3Xmb || mPs3WizActive) {
        // DSi / Minima game Information page / paginated info dialog: LEFT turns to the previous page.
        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && (ndsGameInfoActive() || ndsDlgInfoPaged())) { ndsInfoPage(-1); return; }
        // DSi stacked carousel: LEFT cycles the focused carousel back one card (categories at
        // the root, else the category/submenu cards). A modal (chooser/dialog) keeps XMB nav.
        // A settings LIST level: LEFT walks up to the parent (a vertical list has no horizontal move).
        if (mNdsTheme && mPs3Xmb && !ndsInModal()) { if (ndsCurLevelIsList()) ndsNavBack(); else ndsNavHoriz(-1); }
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal() && minimaAppGridActive()) minimaAppGridMove(-1, 0);   // Applications grid: a column left
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal()) ndsNavHoriz(-6);   // Minima: LEFT jumps up a page (clamped)
        else if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuCycle(-1);  // ES-DE menu: cycle value / row
        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeNav(-1, 0);       // ES-DE: previous system
        else ps3XmbLeft();   // native XMB submenu drilled in (mPs3Stack): left within the submenu
        return;
    }
    if (!mXmbMode) return;
    if (mSearchActive) return;
    int next = mXmbSystemIndex - 1;
    while (next >= -2) {
        if (next == -1 && mXmbRecent.empty()) { next--; continue; }
        if (next == -2 && mSettingsItems.empty()) { next--; continue; }
        mXmbSystemIndex = next;
        mXmbGameIndex = 0;
        mXmbGameScrollTop = 0;
        mSettingsSelectedIndex = 0;
        mDisplayDirty = true;
        return;
    }
}

void NanoMenu::handleRight() {
    if (biosDlgShown()) { if (!mSetupTransitioning || !mBiosDlgSetup) biosDlgHoriz(1); return; }   // BIOS dialog: its buttons
    if (esdeOverlayNav(NavDir::Right)) return;   // ES-DE media viewer / game information
    if (mScrapeProgActive) return;   // modal swallows navigation
    // GammaOS Nano: navigating cancels any queued launch.
    cancelPendingLaunch();
    if (mSetupWizardActive && mMenuState == MENU_SETUP_WIZARD && !mPs3WizActive
        && mSetupStep == SETUP_DATETIME) {
        setupDateTimeCycle(1); return;
    }
    if (mSetupWizardActive && mMenuState == MENU_SETUP_WIZARD && !mPs3WizActive
        && mSetupStep == SETUP_SDCARD && mSetupSdSel < 2 && !mSetupTransitioning) {
        setupSdCardSelect(); return;   // On <-> Off
    }
    if (mOskActive) {
        if (!oskButtonLeavesTouch()) oskMoveCursor(NavDir::Right);
        return;
    }
    if (mMenuState == MENU_WIFI || mMenuState == MENU_BT) return;
    if (mMenuState == MENU_SETTINGS) { handleSettingsTreeRight(); return; }
    if (mPs3Xmb || mPs3WizActive) {
        // DSi / Minima game Information page / paginated info dialog: RIGHT turns to the next page.
        if ((mNdsTheme || mMinimaTheme) && mPs3Xmb && (ndsGameInfoActive() || ndsDlgInfoPaged())) { ndsInfoPage(+1); return; }
        // DSi stacked carousel: RIGHT cycles the focused carousel forward one card.
        // A settings LIST level: RIGHT drills the focused row ONLY when it opens a submenu
        // (the ">" chevron rows); on a leaf (toggle / action) RIGHT does NOTHING, so a
        // drifting stick / temperamental d-pad diagonal cannot confirm it (confirm is X/A).
        if (mNdsTheme && mPs3Xmb && !ndsInModal()) { if (ndsCurLevelIsList()) { if (ps3FocusOpensSubmenu()) ndsNavSelect(false); } else ndsNavHoriz(+1); }
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal() && minimaAppGridActive()) minimaAppGridMove(+1, 0);   // Applications grid: a column right
        else if (mMinimaTheme && mPs3Xmb && !ndsInModal()) ndsNavHoriz(+6);   // Minima: RIGHT jumps down a page (clamped)
        else if (mEsdeTheme && (mEsdeMenuActive || mEsdeMenuClosing)) esdeMenuCycle(+1);  // ES-DE menu: cycle value / row
        else if (mEsdeTheme && mPs3Xmb && !ndsInModal() && mPs3Stack.empty()) esdeNav(+1, 0);       // ES-DE: next system
        else ps3XmbRight();   // native XMB submenu drilled in (mPs3Stack): right within the submenu
        return;
    }
    if (!mXmbMode) return;
    if (mSearchActive) return;
    int numSys = (int)mXmbSystems.size();
    int next = mXmbSystemIndex + 1;
    while (next <= numSys - 1) {
        if (next == -1 && mXmbRecent.empty()) { next++; continue; }
        mXmbSystemIndex = next;
        mXmbGameIndex = 0;
        mXmbGameScrollTop = 0;
        mSettingsSelectedIndex = 0;
        mDisplayDirty = true;
        return;
    }
}

// Launch an installed app. Overlay XMB: replace the running app with it (force-stop current +
// start new + dismiss) instead of the home exit-to-launch path. returnToApps brings the home back
// on the Applications list when the app exits.
void NanoMenu::launchInstalledApp(const std::string& pkg, bool returnToApps) {
    if ((!mOverlayMode && !isLaunchReady()) || biosWorkerBusy()) { showLaunchBusyToast(); return; }
    // An app replacing the last game means the next boot must not resume that game. Selecting the
    // app already running in the overlay only brings it back, so its Quick Resume stays.
    if (!mOverlayMode || pkg != mOverlayPausedPkg) {
        property_set("persist.gammaos.nano.qr_prepared", "0");
        property_set("persist.gammaos.nano.qr_core", "");
    }
    if (!mPanelApp.empty() && pkg == mPanelApp) { panelAppToMonitor(); return; }   // it moves there
    if (mOverlayMode) { overlayLaunchPackage(pkg); return; }
    ALOGI("ps3menu: launching app %s", pkg.c_str());
    commitLaunch(LaunchTuple()
            .app(pkg.c_str())
            .launchedPkg(pkg.c_str())
            .romPath("")
            .core(""));
    property_set("sys.gammaos.nano.return_apps", returnToApps ? "1" : "0");
    armAppLaunchTrigger();
    commitLaunch(LaunchTuple().dropInput("1"));
    mWaitForRelease = true;
}

// A ".app" game file holding the package name of an installed app (the Android Games system):
// that package, or "" for any other file.
std::string NanoMenu::appShortcutPackage(const std::string& romPath) {
    if (romPath.size() <= 4 || romPath.compare(romPath.size() - 4, 4, ".app") != 0) return "";
    std::string pkg;
    if (FILE* f = fopen(romPath.c_str(), "re")) {
        char buf[256] = {};
        if (fgets(buf, sizeof(buf), f)) pkg = buf;
        fclose(f);
    }
    while (!pkg.empty() && isspace((unsigned char)pkg.back())) pkg.pop_back();
    return (!pkg.empty() && packageInstalled(pkg)) ? pkg : "";
}

// The Android Games helper (tools/android-games) keeps these files; Nano adds one when an app is
// added by hand, and the helper keeps it while the app is installed.
static const char kAndroidGamesDir[] = "/storage/emulated/0/ROMs/androidgames";

std::string NanoMenu::androidGameFile(const std::string& pkg, bool* hidden) {
    if (hidden) *hidden = false;
    std::string shown, hid;
    if (DIR* d = opendir(kAndroidGamesDir)) {
        while (struct dirent* de = readdir(d)) {
            const std::string name = de->d_name;
            if (name.empty() || name[0] == '.' || name.size() <= 4 ||
                name.compare(name.size() - 4, 4, ".app") != 0) continue;
            const std::string path = std::string(kAndroidGamesDir) + "/" + name;
            std::string line;
            if (FILE* f = fopen(path.c_str(), "re")) {
                char buf[256] = {};
                if (fgets(buf, sizeof(buf), f)) line = buf;
                fclose(f);
            }
            while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
            if (line != pkg) continue;
            if (!mHiddenGames.count(hiddenKey(path))) { shown = path; break; }
            if (hid.empty()) hid = path;
        }
        closedir(d);
    }
    if (!shown.empty() || hid.empty()) return shown;
    if (hidden) *hidden = true;
    return hid;
}

void NanoMenu::androidGameToggle(const std::string& pkg, const std::string& label) {
    if (pkg.empty()) return;
    bool hidden = false;
    std::string file = androidGameFile(pkg, &hidden);
    const std::string shown = label.empty() ? pkg : label;
    if (!file.empty() && !hidden) {
        // Hidden rather than deleted: the helper would list an Android game again at its next sync.
        hideGame(file);
        showXmbMessage(xmbFmt(trDyn("%s is no longer in Android Games"), shown.c_str()),
                       trDyn("Add it again from Applications"), 200);
        return;
    }
    if (!file.empty()) {
        unhideGame(hiddenKey(file));
    } else {
        // The name as the helper makes it: no characters a file name can't hold, no leading dot
        // (Nano skips dot files), and a number when the name is taken.
        std::string base;
        for (unsigned char c : shown) {
            const bool bad = c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", c);
            const char out = bad ? ' ' : (char)c;
            if (out == ' ' && (base.empty() || base.back() == ' ')) continue;
            base += out;
        }
        while (!base.empty() && (base.back() == ' ')) base.pop_back();
        while (!base.empty() && (base[0] == '.' || base[0] == ' ')) base.erase(0, 1);
        if (base.empty()) base = "Game";
        mkdir(kAndroidGamesDir, 0775);
        int fd = -1;
        for (int i = 1; fd < 0 && i < 100; i++) {
            file = std::string(kAndroidGamesDir) + "/" + base + (i > 1 ? " (" + std::to_string(i) + ")" : "") + ".app";
            fd = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
            if (fd < 0 && errno != EEXIST) break;
        }
        const std::string text = pkg + "\n";
        const bool ok = fd >= 0 && write(fd, text.data(), text.size()) == (ssize_t)text.size();
        if (fd >= 0) close(fd);
        if (!ok) {
            if (fd >= 0) unlink(file.c_str());
            ALOGW("NanoMenu: can't add %s to Android Games: %s", pkg.c_str(), strerror(errno));
            showXmbMessage(xmbFmt(trDyn("Couldn't add %s to Android Games"), shown.c_str()), "", 200);
            return;
        }
        ALOGI("NanoMenu: added %s to Android Games as %s", pkg.c_str(), file.c_str());
        if (mHiddenGames.count(hiddenKey(file))) {   // hidden under this name before it was uninstalled
            unhideGame(hiddenKey(file));
        } else {
            forceRescanAllSystems();
        }
        mPs3CatsStale = true;
        mDisplayDirty = true;
    }
    // The helper saves the game's icon as its art.
    std::thread([] {
        system("am broadcast -a org.thimsy.androidgames.SYNC "
               "-n org.thimsy.androidgames/.SyncReceiver >/dev/null 2>&1");
    }).detach();
    showXmbMessage(xmbFmt(trDyn("%s is in Android Games"), shown.c_str()), "", 200);
}

// Starts the selected game directly as its app when it is an app shortcut, so nano tracks the game
// itself rather than a launcher in between, with a game launch's Recently Played entry and return
// position. The selection is read the way the caller reads it: launchXmbGame (a Recently Played
// entry, a search result or mXmbSystemIndex/mXmbGameIndex) or overlayLaunchGame (no search).
// False for any other game.
bool NanoMenu::launchSelectedAppShortcut() {
    const bool search = mSearchActive && !mOverlayMode;
    const bool recent = mXmbSystemIndex == -1 && !search;
    int s = -1, g = -1;
    std::string rom;
    if (recent) {
        if (mXmbGameIndex >= 0 && mXmbGameIndex < (int)mXmbRecent.size()) rom = mXmbRecent[mXmbGameIndex].romPath;
    } else {
        if (!search) { s = mXmbSystemIndex; g = mXmbGameIndex; }
        else if (mSearchSelectedIndex >= 0 && mSearchSelectedIndex < (int)mSearchResults.size()) {
            s = mSearchResults[mSearchSelectedIndex].sysIdx; g = mSearchResults[mSearchSelectedIndex].gameIdx;
        }
        if (s >= 0 && s < (int)mXmbSystems.size() && g >= 0 && g < (int)mXmbSystems[s].roms.size())
            rom = mXmbSystems[s].roms[g];
    }
    const std::string pkg = appShortcutPackage(rom);
    if (pkg.empty()) return false;
    if ((!mOverlayMode && !isLaunchReady()) || biosWorkerBusy()) { showLaunchBusyToast(); return true; }

    if (recent) esdeGamelistPlayed(rom);
    if (!recent) {
        addXmbRecent(s, g);
    } else if (mXmbGameIndex > 0) {
        // Move it to the front. The home only rewrites the file, as its own recent launch does, so
        // the list does not visibly shuffle during the exit.
        std::vector<XmbRecentEntry> saved = mXmbRecent;
        saved.erase(saved.begin() + mXmbGameIndex);
        saved.insert(saved.begin(), mXmbRecent[mXmbGameIndex]);
        std::swap(mXmbRecent, saved);
        saveXmbRecent();
        if (!mOverlayMode) std::swap(mXmbRecent, saved);
    }
    if (!mOverlayMode) {
        char buf[32];
        const bool fromSystem = !recent && !search;
        snprintf(buf, sizeof(buf), "%d", fromSystem ? mXmbSystemIndex : -1);
        property_set("sys.gammaos.nano.xmb_return_sys", buf);
        snprintf(buf, sizeof(buf), "%d", fromSystem ? g : 0);
        property_set("sys.gammaos.nano.xmb_return_game", buf);
        commitLaunch(LaunchTuple().returnRecent("0"));
    }
    mSearchActive = false; mOskActive = false;
    launchInstalledApp(pkg, false);
    if (!mOverlayMode) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        char buf[32];
        snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec));
        property_set("sys.gammaos.nano.drop_fence_ns", buf);
    }
    return true;
}

void NanoMenu::launchXmbGame() {
    // Overlay: the home exit-to-launch handshake (set launch_* props + exit so the framework starts
    // the app) does not apply to the resident overlay - running it would make the overlay
    // exit/restart without launching. Launching a game while the overlay is up (over a running app)
    // instead uses overlayLaunchGame(), the am-start handoff that sets app_launched, fades the
    // overlay and dismisses onto the new app (the same path the search launch uses). It reads the
    // same mXmbSystemIndex/mXmbGameIndex every caller sets (XMB/DSi/Minima select, esdeSelect), so a
    // game selected from any home theme launches correctly. Without this, every launch AFTER the
    // first was a dead no-op: once an app is behind it the home runs as the resident overlay, so
    // launching from the overlay or after returning to the theme did nothing.
    if (mOverlayMode) { overlayLaunchGame(); return; }
    if (launchSelectedAppShortcut()) return;
    int sysIdx, gameIdx;

    // GammaOS Nano: gate the entire XMB launch path until the system
    // is ready to accept a handoff to a home app. Without this, an
    // early A press on a freshly-painted XMB exits NanoMenu /
    // bootanim while user 0 is still locked, leaving the panel black.
    // See NanoMenuSystem.cpp / isLaunchReady() for the readiness
    // criteria. The press is queued -- the main loop fires
    // handleSelect() again as soon as readiness flips, so the user
    // does not need to press A a second time after boot.
    if (!isLaunchReady()) {
        ALOGI("NanoMenu: XMB launch deferred -- boot not ready");
        showLaunchBusyToast();
        return;
    }

    // Recently Played mode (index -1): launch directly from recent entry
    if (mXmbSystemIndex == -1 && !mSearchActive) {
        if (mXmbRecent.empty()) return;
        if (mXmbGameIndex < 0 || mXmbGameIndex >= (int)mXmbRecent.size()) return;
        // Take a copy -- the vector reorder below invalidates references.
        XmbRecentEntry re = mXmbRecent[mXmbGameIndex];
        // The game's own emulator (Game Options > Emulator), which may have changed since it was played.
        bool gameEmu = false;
        { XmbSystem g;
          if (applyGameEmulator(re.romPath, g)) {
              re.coreSo = g.coreSo; re.launchPkg = g.launchPkg; re.launchIntent = g.launchIntent;
              re.standalone = g.isStandalone();
              gameEmu = true;
          } }

        // A recent entry outlives the file it points at (game deleted, card removed). Refuse the
        // launch and drop the dead row rather than starting an emulator that cannot open it.
        if (!romLaunchExists(re.romPath)) {
            showRomMissingMsg(re.displayName);
            // Prune the row only when the folder is reachable (the file was genuinely deleted). If
            // the whole share/card is offline, keep it so a briefly-down NAS never wipes history.
            if (romParentDirReachable(re.romPath)) recentRemoveAt(mXmbGameIndex);
            return;
        }

        // The emulator that opens this game may not be on the device (a core was never installed, or
        // the standalone app was uninstalled). Warn instead of black-screening on launch.
        if (re.standalone) {
            if (!packageInstalled(re.launchPkg)) { showEmuMissingMsg(re.displayName, true); return; }
        } else if (!coreSoExists(re.coreSo)) {
            showEmuMissingMsg(re.displayName, false); return;
        }

        // The intent is built before anything is recorded, so a launch refused for its file name
        // leaves the play count and the Recently Played order alone.
        std::string filename = re.romPath;
        { size_t lastSlash = filename.rfind('/');
          if (lastSlash != std::string::npos) filename = filename.substr(lastSlash + 1); }
        std::string tabIntent;
        if (re.standalone) {
            // The system's extra launch args apply while the entry still uses that system's
            // emulator (a per-game emulator drops them, as on the system list).
            std::string extraArgs;
            if (!gameEmu) {
                const int si = recentSystemIndex(re);
                if (si >= 0 && mXmbSystems[si].launchIntent == re.launchIntent)
                    extraArgs = mXmbSystems[si].launchArgs;
            }
            nanolaunch::LaunchVars lv;
            lv.fileUri = nanolaunch::safContentUri(re.romPath, "");
            lv.filePath = nanolaunch::appVisiblePath(re.romPath);
            if (!launchFileIdFor(re.launchIntent, extraArgs, re.romPath, re.displayName, lv.fileContent))
                return;
            if (!mupenDirectIntent(re.launchPkg, re.romPath, lv.fileUri, true, tabIntent)) {
                std::vector<std::string> toks;
                if (!nanolaunch::buildLaunchIntent(re.launchIntent, extraArgs, lv, toks)) {
                    showUnlaunchableNameMsg(re.displayName);
                    return;
                }
                tabIntent = nanolaunch::joinTabbed(toks);
            }
        }

        esdeGamelistPlayed(re.romPath);
        // Move to front of recent list -- only on disk, not in-memory.
        // Modifying the vector causes a visible shuffle during the
        // transition frames before NanoMenu exits.
        if (mXmbGameIndex > 0) {
            std::vector<XmbRecentEntry> saved = mXmbRecent;
            saved.erase(saved.begin() + mXmbGameIndex);
            saved.insert(saved.begin(), re);
            std::swap(mXmbRecent, saved);
            saveXmbRecent();
            std::swap(mXmbRecent, saved); // restore in-memory order
        }

        if (re.standalone) {
            commitLaunch(LaunchTuple().app(re.launchPkg));
            { const char* f = "/data/system/nano_launch_intent.txt";
              int ifd = open(f, O_WRONLY|O_CREAT|O_TRUNC, 0666);
              if (ifd >= 0) { write(ifd, tabIntent.c_str(), tabIntent.size()); close(ifd); chmod(f, 0644); }
              commitLaunch(LaunchTuple().intent("file")); }
            commitLaunch(LaunchTuple()
                    .romPath("")
                    .game(recentSystemIndex(re) >= 0 ? mXmbSystems[recentSystemIndex(re)].romDir : re.romDir, re.romPath)
                    .core(""));

            // GammaOS: Drastic quick-resume prime (recent-played path).
            if (re.launchPkg == "com.dsemu.drastic" && drasticNanoAvailable()) {
                if (mQuickResumeEnabled) {
                    setQrRomPath(re.romPath);
                    android::base::SetProperty(
                            "persist.gammaos.nano.qr_core", "drastic");
                    property_set("persist.gammaos.nano.qr_prepared", "1");
                    std::string gameName = filename;
                    size_t dotPos = gameName.rfind('.');
                    if (dotPos != std::string::npos) gameName.erase(dotPos);
                    android::base::SetProperty(
                            "persist.gammaos.nano.qr_game_name", gameName);
                    { const char* qf = "/data/system/nano_drastic_qr_intent.txt";
                      int qfd = open(qf, O_WRONLY|O_CREAT|O_TRUNC, 0666);
                      if (qfd >= 0) {
                          write(qfd, tabIntent.c_str(), tabIntent.size());
                          close(qfd);
                          chmod(qf, 0644);
                      } }
                    property_set("sys.gammaos.nano.cache_ready", "0");
                    property_set("sys.gammaos.nano.cache_op", "populate_drastic");
                } else {
                    // QR disabled: clear any stale prime so we don't
                    // auto-resume a previous game after drastic exits.
                    property_set("persist.gammaos.nano.qr_prepared", "0");
                    android::base::SetProperty("persist.gammaos.nano.qr_core", "");
                }

                // GammaOS: Drastic Nano intercept. Fires regardless of
                // QR state -- drastic-nano reads the ROM path from
                // nano_drastic_nano_rom.txt, not from the QR intent.
                char dn[PROPERTY_VALUE_MAX] = {};
                property_get("persist.gammaos.nano.drastic_nano",
                             dn, "0");
                if (dn[0] == '1') {
                    setDrasticNanoRomPath(re.romPath);
                    ALOGW("drastic nano: XMB recent-played launch, "
                          "qr=%d", mQuickResumeEnabled ? 1 : 0);
                    mDrasticNanoPending = true;
                    mSearchActive = false;
                    mOskActive = false;
                    return;
                }
            } else {
                // Non-drastic standalone (PPSSPP, etc.): clear any stale
                // QR prime so the next nano start does not auto-resume an
                // unrelated drastic/retroarch game.
                property_set("persist.gammaos.nano.qr_prepared", "0");
                android::base::SetProperty("persist.gammaos.nano.qr_core", "");
            }
        } else {
            std::string corePath = "/data/data/com.retroarch.aarch64/cores/" + re.coreSo;
            commitLaunch(LaunchTuple()
                    .romPath(re.romPath)
                    .game(recentSystemIndex(re) >= 0 ? mXmbSystems[recentSystemIndex(re)].romDir : re.romDir, re.romPath,
                          corePath)
                    .core(corePath)
                    .app("com.retroarch.aarch64")
                    .intent(""));
            retroarchConfigFile();   // sys.gammaos.retroarch.cfg_dir as of now, for the launch
            property_set("sys.gammaos.nano.cache_ready", "0");
            property_set("sys.gammaos.nano.cache_op", "populate");
            if (mQuickResumeEnabled) {
                setQrRomPath(re.romPath);
                android::base::SetProperty("persist.gammaos.nano.qr_core", corePath);
                property_set("persist.gammaos.nano.qr_prepared", "1");
                std::string gameName;
                { size_t ls = re.romPath.rfind('/');
                  gameName = (ls != std::string::npos)
                          ? re.romPath.substr(ls + 1) : re.romPath; }
                size_t dotPos = gameName.rfind('.');
                if (dotPos != std::string::npos) gameName.erase(dotPos);
                android::base::SetProperty(
                        "persist.gammaos.nano.qr_game_name", gameName);
            } else {
                // QR disabled: clear any stale prime (e.g. a previously-launched
                // drastic game) so the next boot does not auto-resume an unrelated
                // game.
                property_set("persist.gammaos.nano.qr_prepared", "0");
                android::base::SetProperty("persist.gammaos.nano.qr_core", "");
            }
        }
        ALOGI("NanoMenu XMB: recent launch %s [%s]", re.displayName.c_str(), re.systemName.c_str());
        mSearchActive = false; mOskActive = false;
        property_set("sys.gammaos.nano.xmb_return_sys", "-1");
        property_set("sys.gammaos.nano.xmb_return_game", "0");
        commitLaunch(LaunchTuple().returnRecent("0"));
        property_set("sys.gammaos.nano.return_apps", "0");
        armAppLaunchTrigger();
        commitLaunch(LaunchTuple().dropInput("1"));
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t fenceNs = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
        char buf[32]; snprintf(buf, sizeof(buf), "%" PRId64, fenceNs);
        property_set("sys.gammaos.nano.drop_fence_ns", buf);
        mWaitForRelease = true;
        return;
    }

    if (mSearchActive) {
        if (mSearchSelectedIndex < 0 || mSearchSelectedIndex >= (int)mSearchResults.size()) {
            ALOGW("NanoMenu: search launch with no result %d of %zu", mSearchSelectedIndex, mSearchResults.size());
            return;
        }
        sysIdx = mSearchResults[mSearchSelectedIndex].sysIdx;
        gameIdx = mSearchResults[mSearchSelectedIndex].gameIdx;
    } else {
        sysIdx = mXmbSystemIndex;
        gameIdx = mXmbGameIndex;
    }

    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return;
    if (gameIdx < 0 || gameIdx >= (int)mXmbSystems[sysIdx].roms.size()) return;
    // A game with its own emulator (Game Options > Emulator) launches through a copy of its system
    // that carries that emulator.
    XmbSystem gameSys;
    const XmbSystem* sysp = &mXmbSystems[sysIdx];
    if (gameEmulatorFor(sysp->roms[gameIdx])) {
        gameSys = *sysp;
        applyGameEmulator(gameSys.roms[gameIdx], gameSys);
        sysp = &gameSys;
    }
    const auto& sys = *sysp;

    std::string romPath = sys.roms[gameIdx];
    if (romPath.find("/data/media/0/") == 0) {
        romPath = "/sdcard/" + romPath.substr(14);
    }
    if (romPath.find("/mnt/media_rw/") == 0) {
        romPath = "/storage/" + romPath.substr(14);
    }
    // The library can be out of date (game deleted, or its card is not mounted). Check before
    // handing the path to an emulator, which would otherwise start and die on the missing file.
    if (!romLaunchExists(romPath)) {
        showRomMissingMsg(gameIdx < (int)sys.displayNames.size()
                              ? sys.displayNames[gameIdx] : std::string());
        return;
    }

    // The emulator for this system may not be installed (a bulk-added system whose core was never
    // fetched, or an uninstalled standalone app). Warn instead of launching into a black screen.
    if (sys.isStandalone()) {
        if (!packageInstalled(sys.launchPkg)) { showEmuMissingMsg(sys.name, true); return; }
    } else if (!coreSoExists(sys.coreSo)) {
        showEmuMissingMsg(sys.name, false); return;
    }

    if (sys.isStandalone()) {
        std::string fullRomPath = sys.roms[gameIdx];
        std::string filename;
        { size_t ls = fullRomPath.rfind('/');
          filename = (ls != std::string::npos) ? fullRomPath.substr(ls + 1) : fullRomPath; }
        // custom-package launch: intent template + any user launch args, with the
        // Daijisho token vocabulary substituted ({file.uri} content URI, {file.path}
        // raw path, {file.mime} generic mime, {file.content} the game ID in the file).
        nanolaunch::LaunchVars lv;
        lv.fileUri = nanolaunch::safContentUri(fullRomPath, "");
        lv.filePath = romPath;
        if (!launchFileIdFor(sys.launchIntent, sys.launchArgs, fullRomPath,
                             gameIdx < (int)sys.displayNames.size() ? sys.displayNames[gameIdx] : filename,
                             lv.fileContent))
            return;
        const std::string& contentUri = lv.fileUri;
        std::string tabIntent;
        if (!mupenDirectIntent(sys.launchPkg, fullRomPath, contentUri, true, tabIntent)) {
            std::vector<std::string> toks;
            if (!nanolaunch::buildLaunchIntent(sys.launchIntent, sys.launchArgs, lv, toks)) {
                showUnlaunchableNameMsg(gameIdx < (int)sys.displayNames.size()
                                            ? sys.displayNames[gameIdx] : filename);
                return;
            }
            tabIntent = nanolaunch::joinTabbed(toks);
        }
        ALOGI("NanoMenu XMB: standalone launch %s uri=%s",
              sys.launchPkg.c_str(), contentUri.c_str());
        commitLaunch(LaunchTuple().app(sys.launchPkg));
        {
            const char* intentFile = "/data/system/nano_launch_intent.txt";
            int ifd = open(intentFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (ifd >= 0) {
                write(ifd, tabIntent.c_str(), tabIntent.size());
                close(ifd);
                chmod(intentFile, 0644);
                commitLaunch(LaunchTuple().intent("file"));
            } else {
                ALOGE("NanoMenu: failed to write intent file: %s", strerror(errno));
                commitLaunch(LaunchTuple().intent(""));
            }
        }
        commitLaunch(LaunchTuple()
                .romPath("")
                .game(sys.romDir, sys.roms[gameIdx])
                .core(""));

        if (sys.launchPkg == "com.dsemu.drastic" && drasticNanoAvailable()) {
            if (mQuickResumeEnabled) {
                setQrRomPath(romPath);
                android::base::SetProperty(
                        "persist.gammaos.nano.qr_core", "drastic");
                property_set("persist.gammaos.nano.qr_prepared", "1");
                std::string gameName = filename;
                size_t dotPos = gameName.rfind('.');
                if (dotPos != std::string::npos) gameName.erase(dotPos);
                android::base::SetProperty(
                        "persist.gammaos.nano.qr_game_name", gameName);
                { const char* qf = "/data/system/nano_drastic_qr_intent.txt";
                  int qfd = open(qf, O_WRONLY|O_CREAT|O_TRUNC, 0666);
                  if (qfd >= 0) {
                      write(qfd, tabIntent.c_str(), tabIntent.size());
                      close(qfd);
                      chmod(qf, 0644);
                  } }
                property_set("sys.gammaos.nano.cache_ready", "0");
                property_set("sys.gammaos.nano.cache_op", "populate_drastic");
            } else {
                // QR disabled: clear any stale prime so we don't
                // auto-resume a previous game after drastic exits.
                property_set("persist.gammaos.nano.qr_prepared", "0");
                android::base::SetProperty("persist.gammaos.nano.qr_core", "");
            }

            // GammaOS: Drastic Nano intercept (system browse path).
            // Fires regardless of QR state -- drastic-nano reads the
            // ROM path from nano_drastic_nano_rom.txt, not from the
            // QR intent file.
            char dn[PROPERTY_VALUE_MAX] = {};
            property_get("persist.gammaos.nano.drastic_nano",
                         dn, "0");
            if (dn[0] == '1') {
                setDrasticNanoRomPath(romPath);
                ALOGW("drastic nano: XMB system launch, "
                      "qr=%d", mQuickResumeEnabled ? 1 : 0);
                // Record in Recently Played, same as the normal launch tail (this
                // intercept returns before that call), so drastic-nano games show
                // up in the Recently Played row.
                addXmbRecent(sysIdx, gameIdx);
                mDrasticNanoPending = true;
                mSearchActive = false;
                mOskActive = false;
                return;
            }
        } else {
            // Non-drastic standalone (PPSSPP, etc.): clear any stale QR
            // prime so the next nano start does not auto-resume an
            // unrelated drastic/retroarch game.
            property_set("persist.gammaos.nano.qr_prepared", "0");
            android::base::SetProperty("persist.gammaos.nano.qr_core", "");
        }
    } else {
        // RetroArch core
        std::string corePath = "/data/data/com.retroarch.aarch64/cores/" + sys.coreSo;
        ALOGI("NanoMenu XMB: launching %s core=%s", romPath.c_str(), corePath.c_str());
        commitLaunch(LaunchTuple()
                .romPath(romPath)
                .game(sys.romDir, romPath, corePath)
                .core(corePath)
                .app("com.retroarch.aarch64")
                .intent(""));
        retroarchConfigFile();   // sys.gammaos.retroarch.cfg_dir as of now, for the launch
        property_set("sys.gammaos.nano.cache_ready", "0");
        property_set("sys.gammaos.nano.cache_op", "populate");

        if (mQuickResumeEnabled) {
            setQrRomPath(romPath);
            android::base::SetProperty("persist.gammaos.nano.qr_core", corePath);
            property_set("persist.gammaos.nano.qr_prepared", "1");
            std::string gameName;
            { size_t ls = romPath.rfind('/');
              gameName = (ls != std::string::npos)
                      ? romPath.substr(ls + 1) : romPath; }
            size_t dotPos = gameName.rfind('.');
            if (dotPos != std::string::npos) gameName.erase(dotPos);
            android::base::SetProperty(
                    "persist.gammaos.nano.qr_game_name", gameName);
        } else {
            // QR disabled: clear any stale prime (e.g. a previously-launched drastic
            // game) so the next boot does not auto-resume an unrelated game.
            property_set("persist.gammaos.nano.qr_prepared", "0");
            android::base::SetProperty("persist.gammaos.nano.qr_core", "");
        }
    }

    // Record in recently played
    if (!mSearchActive) {
        addXmbRecent(sysIdx, gameIdx);
    } else {
        addXmbRecent(sysIdx, gameIdx);
    }

    mSearchActive = false;
    mOskActive = false;

    // Save return state
    {
        char buf[32];
        if (!mSearchActive && mXmbSystemIndex >= 0) {
            snprintf(buf, sizeof(buf), "%d", mXmbSystemIndex);
            property_set("sys.gammaos.nano.xmb_return_sys", buf);
            snprintf(buf, sizeof(buf), "%d", gameIdx);
            property_set("sys.gammaos.nano.xmb_return_game", buf);
        } else {
            property_set("sys.gammaos.nano.xmb_return_sys", "-1");
            property_set("sys.gammaos.nano.xmb_return_game", "0");
        }
    }

    commitLaunch(LaunchTuple().returnRecent("0"));
    property_set("sys.gammaos.nano.return_apps", "0");
    armAppLaunchTrigger();
    commitLaunch(LaunchTuple().dropInput("1"));

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t fenceNs = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, fenceNs);
    property_set("sys.gammaos.nano.drop_fence_ns", buf);

    mWaitForRelease = true;
}

// ---------------------------------------------------------------------------
// On-Screen Keyboard (Search)
// ---------------------------------------------------------------------------

// openOsk / closeOsk / oskType / oskBackspace / oskConfirm and renderOsk now
// live in NanoOsk.cpp (the Leanback-derived multi-script keyboard). Only the
// XMB-coupled search-result query stays here.

void NanoMenu::updateSearchResults() {
    mSearchResults.clear();
    if (mOskQuery.empty()) return;
    for (int s = 0; s < (int)mXmbSystems.size(); s++) {
        const auto& sys = mXmbSystems[s];
        for (int g = 0; g < (int)sys.displayNames.size(); g++) {
            if (containsInsensitive(sys.displayNames[g], mOskQuery)) {
                mSearchResults.push_back({s, g});
                if (mSearchResults.size() >= 100) return; // cap results
            }
        }
    }
    mSearchActive = !mSearchResults.empty() || !mOskQuery.empty();
}

// ---------------------------------------------------------------------------
// XMB Rendering
// ---------------------------------------------------------------------------

void NanoMenu::renderXmb() {
    float sf = fminf((float)mWidth / 1080.0f, (float)mHeight / 720.0f);
    if (sf < 0.5f) sf = 0.5f;

    // Smooth animation -- frame-rate-independent exponential decay.
    float dt = mFrameDt;
    float decay = 1.0f - expf(-12.0f * dt);
    mXmbAnimX += ((float)mXmbSystemIndex - mXmbAnimX) * decay;
    if (fabsf(mXmbAnimX - mXmbSystemIndex) < 0.005f) mXmbAnimX = mXmbSystemIndex;
    // Settings column drives its own cursor variable; the rest of XMB
    // uses mXmbGameIndex / mSearchSelectedIndex. Without this check the
    // animation target stays pinned at game-index 0 while on Settings,
    // so Up/Down appear to do nothing visually.
    float targetY;
    if (mSearchActive) {
        targetY = (float)mSearchSelectedIndex;
    } else if (isOnSettingsColumn()) {
        targetY = (float)mSettingsSelectedIndex;
    } else {
        targetY = (float)mXmbGameIndex;
    }
    mXmbAnimY += (targetY - mXmbAnimY) * decay;
    if (fabsf(mXmbAnimY - targetY) < 0.005f) mXmbAnimY = targetY;

    int numSys = (int)mXmbSystems.size();
    if (numSys == 0) return;

    // PSP-style: white/gray icons, no gold tint
    float iconR = 0.85f, iconG = 0.85f, iconB = 0.85f;
    float dimIconR = 0.45f, dimIconG = 0.45f, dimIconB = 0.45f;

    // Layout -- based on RetroArch XMB driver constants
    float scaleFactor = sf;
    float iconSize = 100.0f * scaleFactor;
    float catActiveZoom = 1.0f;
    float catPassiveZoom = 0.55f;
    float itemActiveZoom = 0.8f;
    float itemPassiveZoom = 0.4f;
    float iconSpacingH = 200.0f * scaleFactor;
    float iconSpacingV = 110.0f * scaleFactor;
    float marginTop = 180.0f * scaleFactor;
    float marginLeft = 120.0f * scaleFactor;
    float labelLeft = 20.0f * scaleFactor;
    float aboveItemOff = -1.5f;
    float underItemOff = 2.5f;
    float iconBarY = marginTop;
    float selIconX = marginLeft;
    float textScale = 2.2f * sf;
    float selTextScale = 2.8f * sf;
    float footScale = 1.4f * sf;
    float catNameScale = 1.8f * sf;

    (void)itemActiveZoom;
    (void)itemPassiveZoom;
    (void)aboveItemOff;
    (void)underItemOff;

    bool isRecent = (mXmbSystemIndex == -1);
    bool isSettings = isOnSettingsColumn();

    // --- Horizontal category bar ---
    auto drawCatIcon = [&](int idx, float hOffset, bool isSel, int iconId) {
        float zoom = isSel ? catActiveZoom : catPassiveZoom;
        float sz = iconSize * zoom;
        float alpha = isSel ? 1.0f : fmaxf(0.15f, 1.0f - fabsf(hOffset) * 0.15f);
        float ix = selIconX + hOffset * iconSpacingH;
        float iy = iconBarY - sz / 2.0f;
        if (ix < -sz || ix > mWidth + sz) return; // cull
        float cr = isSel ? iconR : dimIconR;
        float cg = isSel ? iconG : dimIconG;
        float cb = isSel ? iconB : dimIconB;
        drawIcon(iconId, ix, iy, sz, cr, cg, cb, alpha);
        if (isSel) {
            const char* name = (idx == -2) ? trDyn("Settings")
                             : (idx == -1) ? trDyn("Recently Played")
                             : (idx >= 0 && idx < numSys) ? mXmbSystems[idx].name.c_str()
                             : "";
            float nameY = iy + sz + 6.0f * sf;
            float nameW = measureText(name, catNameScale);
            float nameCX = ix + sz / 2.0f - nameW / 2.0f;
            drawText(name, nameCX, nameY, catNameScale, 0.8f, 0.8f, 0.8f, 0.9f);
        }
    };

    // Settings (index -2), leftmost column
    if (!mSettingsItems.empty()) {
        float hOff = -2.0f - mXmbAnimX;
        drawCatIcon(-2, hOff, isSettings, 17);
    }
    // Recently Played (index -1)
    if (!mXmbRecent.empty()) {
        float hOff = -1.0f - mXmbAnimX;
        drawCatIcon(-1, hOff, isRecent, 15);
    }
    // System icons (index 0..N-1)
    for (int i = 0; i < numSys; i++) {
        float hOff = (float)i - mXmbAnimX;
        if (fabsf(hOff) > 8.0f) continue;
        drawCatIcon(i, hOff, !isRecent && !isSettings && i == mXmbSystemIndex,
                    i < 16 ? i : 0);
    }

    // --- Vertical item list ---
    float selSz = iconSize * catActiveZoom;
    float textStartX = selIconX + selSz + labelLeft;
    float contentRight = mWidth * 0.93f;

    (void)textStartX;

    int numItems = 0;
    bool hasItems = true;
    if (isRecent) {
        numItems = (int)mXmbRecent.size();
    } else if (mSearchActive) {
        numItems = (int)mSearchResults.size();
    } else if (isSettings) {
        numItems = (int)mSettingsItems.size();
    } else {
        int si = mXmbSystemIndex;
        if (si >= 0 && si < numSys) numItems = (int)mXmbSystems[si].roms.size();
    }
    if (numItems == 0) hasItems = false;

    int curIdx = isRecent ? mXmbGameIndex
               : mSearchActive ? mSearchSelectedIndex
               : isSettings ? mSettingsSelectedIndex : mXmbGameIndex;
    if (curIdx >= numItems) curIdx = numItems - 1;
    if (curIdx < 0) curIdx = 0;

    if (hasItems) {
        int maxAbove = (int)(iconBarY / iconSpacingV) + 1;
        int maxBelow = (int)((mHeight - iconBarY) / iconSpacingV) + 1;
        int startItem = curIdx - maxAbove;
        int endItem = curIdx + maxBelow;
        if (startItem < 0) startItem = 0;
        if (endItem >= numItems) endItem = numItems - 1;

        float selCatSz = iconSize * catActiveZoom;
        float itemListTop = iconBarY + selCatSz / 2.0f + FONT_CHAR_H * catNameScale + 140.0f * sf;

        float clipTop = iconBarY + selCatSz / 2.0f + 10.0f * sf;
        float animCur = mXmbAnimY;
        float itemIconBaseX = selIconX + (iconSize * catActiveZoom) / 2.0f;

        const float selIconSz = iconSize * catActiveZoom;
        const float listScissorX = itemIconBaseX - selIconSz / 2.0f;
        const float listScissorW = contentRight - listScissorX;
        // Composed rotation+flip mapping (see scissorLogicalRect); the old
        // rotation-only switch mirrored the band on flipped panels.
        scissorLogicalRect(listScissorX, 0.0f, listScissorW, (float)mHeight);

        for (int i = startItem; i <= endItem; i++) {
            float relPos = (float)i - animCur;
            float fy = itemListTop + relPos * iconSpacingV;
            if (fy < clipTop - iconSpacingV * 0.3f || fy > mHeight + iconSpacingV) continue;

            bool isSel = (fabsf((float)i - animCur) < 0.5f);
            float iAlpha = isSel ? 1.0f : 0.55f;
            float tSc = isSel ? selTextScale : textScale;

            const char* displayText = "";
            const char* sysLabel = nullptr;
            if (isRecent && i < (int)mXmbRecent.size()) {
                displayText = mXmbRecent[i].displayName.c_str();
                sysLabel = mXmbRecent[i].systemName.c_str();
            } else if (mSearchActive && i < (int)mSearchResults.size()) {
                const auto& res = mSearchResults[i];
                // gameIdx must be re-bounded against the CURRENT list: a background rescan can
                // shrink a system's displayNames while this overlay is up, leaving a stale
                // index in mSearchResults (which is not revalidated on rescan) -> OOB read.
                if (res.sysIdx >= 0 && res.sysIdx < numSys &&
                    res.gameIdx >= 0 &&
                    res.gameIdx < (int)mXmbSystems[res.sysIdx].displayNames.size()) {
                    displayText = mXmbSystems[res.sysIdx].displayNames[res.gameIdx].c_str();
                    sysLabel = mXmbSystems[res.sysIdx].shortname.c_str();
                }
            } else if (isSettings && i < (int)mSettingsItems.size()) {
                displayText = trDyn(mSettingsItems[i].label.c_str());
            } else {
                int si = mXmbSystemIndex;
                if (si >= 0 && si < numSys && i < (int)mXmbSystems[si].displayNames.size()) {
                    displayText = mXmbSystems[si].displayNames[i].c_str();
                }
            }

            float itemIconSz = isSel ? 50.0f * sf : 30.0f * sf;
            float iconX = itemIconBaseX - itemIconSz / 2.0f;
            float iconY = fy - itemIconSz / 2.0f;
            drawIcon(16, iconX, iconY, itemIconSz,
                     isSel ? iconR : dimIconR, isSel ? iconG : dimIconG,
                     isSel ? iconB : dimIconB, iAlpha);

            float tx = iconX + itemIconSz + 10.0f * sf;
            float ty = fy - FONT_CHAR_H * tSc * 0.4f;
            float tr = isSel ? 1.0f : 0.6f;
            float tg = isSel ? 1.0f : 0.6f;
            float tb = isSel ? 1.0f : 0.6f;

            drawText(displayText, tx, ty, tSc, tr, tg, tb, iAlpha);
            if (isSel && sysLabel && *sysLabel) {
                float tagY = ty + FONT_CHAR_H * tSc + 2.0f * sf;
                drawText(sysLabel, tx, tagY, catNameScale * 0.9f,
                         0.5f, 0.5f, 0.55f, 0.7f);
            }
        }
        glDisable(GL_SCISSOR_TEST);
    } else if (!mSearchActive) {
        const char* msg = isRecent ? trDyn("No recently played games")
                        : (mXmbSystemIndex >= 0 && mXmbSystemIndex < numSys
                           && mXmbSystems[mXmbSystemIndex].pathExists)
                          ? trDyn("No games found") : trDyn("ROM folder not found");
        float msgW = measureText(msg, textScale);
        drawText(msg, (mWidth - msgW) / 2.0f, iconBarY + 40.0f * sf,
                 textScale, 0.5f, 0.5f, 0.5f, 0.7f);
    }

    // Footer
    float footH = FONT_CHAR_H * footScale;
    float footY = mHeight - footH - 8.0f * sf;
    const char* footer = mSearchActive
        ? trDyn("Up/Dn: Browse | A: Launch | B: Clear | Y: Refine")
        : trDyn("L/R: System | Up/Dn: Game | A: Play | Y: Search | X: FX | L1: List | R1: QR");
    float fW = measureText(footer, footScale);
    if (!mOskActive)   // the OSK draws its own footer on top
        drawText(footer, (mWidth - fW) / 2.0f, footY, footScale, 0.35f, 0.35f, 0.4f, 0.8f);

    // Search indicator
    if (mSearchActive && !mOskActive) {
        char searchHdr[160];
        snprintf(searchHdr, sizeof(searchHdr), trDyn("Search: \"%s\""), mOskQuery.c_str());
        drawText(searchHdr, 10.0f * sf, footY - FONT_CHAR_H * footScale - 4.0f * sf,
                 footScale, 0.7f, 0.7f, 0.2f, 0.9f);
    }

    // OSK overlay is rendered by render() AFTER the Wi-Fi/BT screens, so the
    // password keyboard sits on top of the network list instead of being
    // overdrawn by it.
}

} // namespace android
