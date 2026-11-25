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
 */

#include "NanoScraper.h"
#include "NanoJson.h"
#include "NanoI18n.h"    // trDyn() runtime translation of hardcoded UI strings
#include "NanoScummvmList.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <utils/SystemClock.h>
#include <unistd.h>
#include <zlib.h>
#include <map>
#include <mutex>

#include <curl/curl.h>

#include <log/log.h>

#ifndef LOG_TAG
#define LOG_TAG "gammaos-scraper"
#endif

namespace android {
namespace nanoscraper {

// The scrape services' answers carry the account and developer keys in their URLs: written on
// internal storage only, never in the cache folder (on the microSD card in the ES-DE folder).
static std::string respDir() {
    static const char* const kDir = "/data/system/nano_scrape";
    mkdir(kDir, 0700);
    return kDir;
}

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

std::string cacheKey(const std::string& romPath) {
    // 64-bit FNV-1a over the full path (same scheme as the photo thumb cache).
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : romPath) { h ^= c; h *= 1099511628211ULL; }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}


static bool fileExistsNonEmpty(const std::string& path, off_t* sizeOut = nullptr) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) return false;
    if (sizeOut) *sizeOut = st.st_size;
    return true;
}

static std::string readFile(const std::string& path, size_t maxBytes) {
    std::string out;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return out;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        size_t n = (size_t)st.st_size;
        if (n > maxBytes) n = maxBytes;
        out.resize(n);
        ssize_t rd = read(fd, &out[0], n);
        if (rd > 0) out.resize((size_t)rd); else out.clear();
    }
    close(fd);
    return out;
}

// Recognize the leading bytes of the common raster formats we can decode, so an
// HTML/text error page returned with HTTP 200 is rejected as "no media".
static bool looksLikeImage(const std::string& path) {
    unsigned char b[12] = {0};
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n < 4) return false;
    if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return true;    // PNG
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)              return true;     // JPEG
    if (b[0] == 'G'  && b[1] == 'I'  && b[2] == 'F')               return true;     // GIF
    if (b[0] == 'B'  && b[1] == 'M')                               return true;     // BMP
    if (n >= 12 && b[0]=='R'&&b[1]=='I'&&b[2]=='F'&&b[3]=='F'
        && b[8]=='W'&&b[9]=='E'&&b[10]=='B'&&b[11]=='P')           return true;     // WEBP
    return false;
}

static bool looksLikePdf(const std::string& path) {
    char b[1024];
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    return n >= 5 && memmem(b, (size_t)n, "%PDF-", 5) != nullptr;
}

static bool looksLikeVideo(const std::string& path) {
    unsigned char b[12] = {0};
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n < 12) return false;
    if (b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p')               return true;   // MP4 / MOV
    if (b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3)           return true;   // MKV / WebM
    if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "AVI ", 4))                     return true;   // AVI
    return false;
}

static void mkdirsFor(const std::string& filePath) {
    for (size_t p = filePath.find('/', 1); p != std::string::npos; p = filePath.find('/', p + 1))
        if (!isVolumeRoot(filePath.substr(0, p))) mkdir(filePath.substr(0, p).c_str(), 0775);
}

// ArtDest::keep: the picture already at a destination's name, any image type ("" = none).
static std::string existingArt(const std::string& to, bool keep) {
    if (!keep || to.empty()) return std::string();
    const size_t sl = to.find_last_of('/'), dot = to.find_last_of('.');
    const std::string stem = dot != std::string::npos && (sl == std::string::npos || dot > sl) ? to.substr(0, dot) : to;
    for (const char* e : {".png", ".jpg", ".jpeg", ".webp"})
        if (access((stem + e).c_str(), R_OK) == 0) return stem + e;
    return std::string();
}

// After a download to an ArtDest name: the same picture under another image type goes (ES-DE and
// Nano would show that one first).
static void dropOtherArt(const std::string& to) {
    if (to.empty()) return;
    const size_t sl = to.find_last_of('/'), dot = to.find_last_of('.');
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return;
    const std::string stem = to.substr(0, dot);
    for (const char* e : {".png", ".jpg", ".jpeg", ".webp"})
        if (stem + e != to) unlink((stem + e).c_str());
}

// GET into outPath. getParams are URL-encoded into the query. Returns true if the transfer finished
// and a non-empty file was written (HTTP-level success is checked by the caller, which inspects the
// body for API error markers / image magic). A large media download (largeMedia) fails on an HTTP
// error and gives up only when the transfer stalls, not after timeoutSec, so a big manual on a slow
// link completes and a cut-off one is never kept. cancelled (may be empty) is polled during the
// transfer; it stops it at once.
thread_local DownloadReporter tReporter;
thread_local std::string tDownloading;   // what the art download in progress is, for tReporter

void setDownloadReporter(DownloadReporter reporter) { tReporter = std::move(reporter); }

// Names the art downloads in its scope for the progress line.
struct Downloading {
    explicit Downloading(const char* what) { tDownloading = what; if (tReporter) tReporter(tDownloading, 0.0); }
    ~Downloading() { tDownloading.clear(); if (tReporter) tReporter(std::string(), 0.0); }
};

// One handle per worker thread. It keeps its connections and TLS sessions open between requests,
// so a game's lookup and its downloads skip the connection setup; they close when the thread exits.
static CURL* threadHandle() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    thread_local struct Handle {
        CURL* h = curl_easy_init();
        ~Handle() { if (h) curl_easy_cleanup(h); }
    } handle;
    return handle.h;
}

struct Transfer {
    int fd;
    const std::function<bool()>* cancelled;
    bool report;
    int64_t lastMs;
    curl_off_t lastBytes;
    curl_off_t maxBytes;
    curl_off_t written;
};

// Largest file kept per kind. A server that sends no Content-Length (or a wrong one) is still
// stopped here, so a hostile or broken URL cannot fill the card.
static constexpr curl_off_t kMaxApiBytes = 32ll << 20;     // API responses and images
static constexpr curl_off_t kMaxManualBytes = 512ll << 20;
static constexpr curl_off_t kMaxVideoBytes = 1024ll << 20;

static size_t writeToFile(char* data, size_t size, size_t count, void* user) {
    Transfer& t = *static_cast<Transfer*>(user);
    const size_t n = size * count;
    if (t.written + (curl_off_t)n > t.maxBytes) return 0;   // aborts the transfer
    t.written += (curl_off_t)n;
    return write(t.fd, data, n) == (ssize_t)n ? n : 0;
}

// Cancels the transfer when asked; reports the speed about twice a second.
static int onProgress(void* user, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) {
    Transfer& t = *static_cast<Transfer*>(user);
    if (*t.cancelled && (*t.cancelled)()) return 1;
    if (t.report) {
        const int64_t ms = uptimeMillis();
        if (ms - t.lastMs >= 500) {
            tReporter(tDownloading, (double)(now - t.lastBytes) * 1000.0 / (double)(ms - t.lastMs));
            t.lastMs = ms; t.lastBytes = now;
        }
    }
    return 0;
}

static bool curlToFile(const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& getParams,
                       const std::string& outPath, int timeoutSec, bool largeMedia = false,
                       const std::function<bool()>& cancelled = nullptr,
                       curl_off_t maxBytes = kMaxApiBytes) {
    unlink(outPath.c_str());
    // Media URLs come from the scraping service's answer: never file://, ftp:// or the like.
    if (strncasecmp(url.c_str(), "https://", 8) != 0 && strncasecmp(url.c_str(), "http://", 7) != 0) {
        ALOGW("scraper: refusing to download a non-http URL");
        return false;
    }
    CURL* h = threadHandle();
    if (!h) return false;
    std::string full = url;
    for (size_t i = 0; i < getParams.size(); i++) {
        char* k = curl_easy_escape(h, getParams[i].first.data(), (int)getParams[i].first.size());
        char* v = curl_easy_escape(h, getParams[i].second.data(), (int)getParams[i].second.size());
        if (k && v) full += std::string(i == 0 ? "?" : "&") + k + "=" + v;
        curl_free(k); curl_free(v);
    }
    const int fd = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    Transfer t{fd, &cancelled, tReporter && !tDownloading.empty(), uptimeMillis(), 0, maxBytes, 0};
    curl_easy_reset(h);   // options only: the open connections stay
    curl_easy_setopt(h, CURLOPT_URL, full.c_str());
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");   // a redirect too
    curl_easy_setopt(h, CURLOPT_MAXFILESIZE_LARGE, maxBytes);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "gammaos-nano-scraper");
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &t);
    if (cancelled || t.report) {
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &t);
    }
    if (largeMedia) {
        curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 7200L);
    } else {
        curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)timeoutSec);
    }
    const CURLcode rc = curl_easy_perform(h);
    close(fd);
    if (rc != CURLE_OK) { unlink(outPath.c_str()); return false; }
    return fileExistsNonEmpty(outPath);
}

// CRC32 of a file (zlib), capped: very large images/ISOs are not worth hashing
// (and ScreenScraper matches CD games by name anyway). Returns "" when skipped.
static std::string crc32File(const std::string& path, off_t maxBytes) {
    off_t sz = 0;
    if (!fileExistsNonEmpty(path, &sz) || sz > maxBytes) return std::string();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    uLong crc = crc32(0L, Z_NULL, 0);
    std::vector<unsigned char> buf(256 * 1024);
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0)
        crc = crc32(crc, buf.data(), (uInt)n);
    close(fd);
    char hex[9];
    snprintf(hex, sizeof(hex), "%08lx", (unsigned long)crc);
    return std::string(hex);
}

static std::string baseName(const std::string& path) {
    size_t s = path.find_last_of('/');
    return (s == std::string::npos) ? path : path.substr(s + 1);
}

static std::string stripExt(const std::string& name) {
    size_t d = name.find_last_of('.');
    return (d == std::string::npos) ? name : name.substr(0, d);
}

// Clean a display name for a name-based query: drop bracketed/parenthesized
// region/version tags and collapse separators to spaces (es-de does similar).
static std::string cleanQueryName(const std::string& in) {
    std::string out;
    int depth = 0;
    for (char c : in) {
        if (c == '(' || c == '[') { depth++; continue; }
        if (c == ')' || c == ']') { if (depth > 0) depth--; continue; }
        if (depth > 0) continue;
        if (c == '_' || c == '.') c = ' ';
        out += c;
    }
    // collapse runs of spaces + trim
    std::string t;
    bool sp = false;
    for (char c : out) {
        if (c == ' ') { sp = true; continue; }
        if (sp && !t.empty()) t += ' ';
        sp = false;
        t += c;
    }
    return t;
}

// Decode the handful of HTML entities the APIs return in metadata text (es-de does
// the same). In-place, conservative; leaves unknown entities untouched.
static void htmlUnescape(std::string& s) {
    static const std::pair<const char*, const char*> kEnt[] = {
        {"&nbsp;", " "}, {"&amp;", "&"}, {"&#x26;", "&"}, {"&#38;", "&"},
        {"&quot;", "\""}, {"&#34;", "\""}, {"&apos;", "'"}, {"&#39;", "'"},
        {"&#039;", "'"}, {"&#x27;", "'"}, {"&copy;", "\xC2\xA9"}, {"&#169;", "\xC2\xA9"},
        {"&lt;", "<"}, {"&gt;", ">"}, {"&eacute;", "\xC3\xA9"}, {"&deg;", "\xC2\xB0"},
    };
    for (const auto& e : kEnt) {
        size_t p = 0;
        const std::string from = e.first, to = e.second;
        while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    }
    // trim leading/trailing whitespace
    size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) s.clear(); else s = s.substr(a, b - a + 1);
}

Engine engineFromName(const std::string& name) {
    if (name == "thegamesdb")   return ENGINE_THEGAMESDB;
    if (name == "off" || name == "none") return ENGINE_OFF;
    return ENGINE_SCREENSCRAPER;
}

// ---------------------------------------------------------------------------
// Platform-id mapping (romDir / shortname -> ScreenScraper systemeid + TheGamesDB
// platform id). Keyed by lowercased folder/short name with common aliases.
// Covers the systems GammaOS ships plus the usual retro set; extend as needed.
// ---------------------------------------------------------------------------
struct PlatRow { const char* key; int ss; int tgdb; };
static const PlatRow kPlatforms[] = {
    // Nintendo
    {"nes",3,7}, {"famicom",3,7}, {"fds",106,4936}, {"famicomdisksystem",106,4936},
    {"snes",4,6}, {"sfc",4,6}, {"superfamicom",4,6}, {"satellaview",107,6},
    {"n64",14,3}, {"nintendo64",14,3},
    {"gb",9,4}, {"gameboy",9,4},
    {"gbc",10,41}, {"gameboycolor",10,41},
    {"gba",12,5}, {"gameboyadvance",12,5},
    {"nds",15,8}, {"ds",15,8}, {"nintendods",15,8},
    {"3ds",17,4912}, {"n3ds",17,4912},
    {"gamecube",13,2}, {"gc",13,2}, {"ngc",13,2},
    {"wii",16,9},
    {"wiiu",18,38},
    {"virtualboy",11,4918}, {"vb",11,4918},
    {"pokemini",211,4957},
    // Sega
    {"megadrive",1,36}, {"genesis",1,18}, {"md",1,36},
    {"mastersystem",2,35}, {"sms",2,35},
    {"gamegear",21,20}, {"gg",21,20},
    {"segacd",20,21}, {"megacd",20,21},
    {"sega32x",19,33}, {"32x",19,33},
    {"saturn",22,17}, {"segasaturn",22,17},
    {"dreamcast",23,16}, {"dc",23,16},
    {"sg1000",109,4949},
    {"naomi",56,23},
    // Sony
    {"psx",57,10}, {"ps1",57,10}, {"playstation",57,10}, {"psone",57,10},
    {"ps2",58,11}, {"playstation2",58,11},
    {"psp",61,13}, {"playstationportable",61,13},
    {"psvita",62,39}, {"vita",62,39}, {"ps3",59,12}, {"playstation3",59,12},
    // NEC
    {"pcengine",31,34}, {"tg16",31,34}, {"turbografx16",31,34}, {"pce",31,34},
    {"pcenginecd",114,4955}, {"tg16cd",114,4955}, {"turbografxcd",114,4955},
    {"supergrafx",105,34},
    {"pcfx",72,4930},
    // SNK
    {"neogeo",142,24}, {"neogeoaes",142,24}, {"neogeomvs",142,24},
    {"neogeocd",70,4956},
    {"ngp",25,4922}, {"neogeopocket",25,4922},
    {"ngpc",82,4923}, {"neogeopocketcolor",82,4923},
    // Atari
    {"atari2600",26,22}, {"a2600",26,22},
    {"atari5200",40,26}, {"a5200",40,26},
    {"atari7800",41,27}, {"a7800",41,27},
    {"atarilynx",28,4924}, {"lynx",28,4924},
    {"atarijaguar",27,28}, {"jaguar",27,28},
    {"atarijaguarcd",171,29},
    {"atarist",42,4937}, {"atari800",43,4943}, {"atari8bit",43,4943},
    // Bandai
    {"wonderswan",45,4925}, {"ws",45,4925},
    {"wonderswancolor",46,4926}, {"wsc",46,4926},
    // Computers / micros
    {"c64",66,40}, {"commodore64",66,40},
    {"amiga",64,4911},
    {"amigacd32",130,4947},
    {"msx",113,4929}, {"msx2",116,4929},
    {"zxspectrum",76,4913}, {"spectrum",76,4913},
    {"amstradcpc",65,4914}, {"cpc",65,4914},
    {"x68000",79,4931},
    {"dos",135,1}, {"pc",135,1},
    {"scummvm",123,1},
    // Other consoles
    {"3do",29,25},
    {"colecovision",48,31}, {"coleco",48,31},
    {"intellivision",115,32},
    {"vectrex",102,4939},
    {"odyssey2",104,4927}, {"o2em",104,4927},
    {"channelf",80,-1},   // es-de has no TheGamesDB platform id for Channel F
    {"pico8",234,-1}, {"openbor",214,-1},   // game engines: ScreenScraper only
    {"androidgames",63,4916}, {"android",63,4916},
    // Arcade
    {"arcade",75,23}, {"mame",75,23}, {"fbneo",75,23}, {"fba",75,23}, {"cps1",75,23},
    {"cps2",75,23}, {"cps3",75,23}, {"varcade",75,23}, {"atomiswave",53,23},
    // Anbernic stock folder names, and the emulator catalog's platform names
    {"ps",57,10}, {"fc",3,7}, {"master",2,35}, {"genesismsu",1,36}, {"snesmsu1",210,6},
    {"tgcd",114,4955}, {"jaguarcd",171,29}, {"ndsi",15,8}, {"wiiware",16,9}, {"switch",225,4971},
    {"gw",52,4950}, {"ngage",30,4938}, {"megaduck",90,4948}, {"supervision",207,4959},
    {"appleii",86,4942}, {"bbcmicro",37,5013}, {"cdi",133,4917}, {"vic20",73,4945}, {"zx81",77,5010},
    {"plus4",99,5007}, {"pc88",221,4933}, {"pc98",208,4934}, {"x1",220,4977}, {"windows",138,1},
    {"xbox",32,14}, {"xbox360",33,15}, {"arduboy",263,-1}, {"tic80",222,-1}, {"uzebox",216,-1},
};

PlatformIds platformForSystem(const std::string& romDir,
                              const std::string& shortname,
                              const std::string& name,
                              const std::string& manualSsId) {
    PlatformIds out;
    if (!manualSsId.empty()) {
        int v = atoi(manualSsId.c_str());
        if (v > 0) out.ss = v;
    }
    auto strip = [](const std::string& s) {
        std::string o;
        for (char c : s) { unsigned char u = (unsigned char)c; if (isalnum(u)) o += (char)tolower(u); }
        return o;
    };
    const std::string keys[3] = { strip(romDir), strip(shortname), strip(name) };
    for (const std::string& k : keys) {
        if (k.empty()) continue;
        for (const auto& r : kPlatforms) {
            if (k == r.key) {
                if (out.ss < 0)   out.ss = r.ss;
                if (out.tgdb < 0) out.tgdb = r.tgdb;
                if (out.ss > 0 && out.tgdb > 0) return out;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Extra media
// ---------------------------------------------------------------------------
const ExtraMediaType kExtraMedia[] = {
    {"screenshot",    "Screenshots",    "screenshots",   "ss",                     "screenshot",  nullptr, EXTRA_IMAGE, "Screenshot"},
    {"titlescreen",   "Title Screens",  "titlescreens",  "sstitle",                "titlescreen", nullptr, EXTRA_IMAGE, "Title Screen"},
    {"backcover",     "Back Covers",    "backcovers",    "box-2D-back",            "boxart",      "back",  EXTRA_IMAGE, "Back Cover"},
    {"3dbox",         "3D Boxes",       "3dboxes",       "box-3D",                 nullptr,       nullptr, EXTRA_IMAGE, "3D Box"},
    {"physicalmedia", "Physical Media", "physicalmedia", "support-2D",             nullptr,       nullptr, EXTRA_IMAGE, "Physical Media"},
    {"marquee",       "Marquees",       "marquees",      "wheel-hd,wheel",         "clearlogo",   nullptr, EXTRA_IMAGE, "Marquee"},
    {"miximage",      "Mix Images",     "miximages",     "mixrbv2,mixrbv1",        nullptr,       nullptr, EXTRA_IMAGE, "Mix Image"},
    {"manual",        "Manuals",        "manuals",       "manuel",                 nullptr,       nullptr, EXTRA_PDF, "Manual"},
    {"video",         "Videos",         "videos",        "video-normalized,video", nullptr,       nullptr, EXTRA_VIDEO, "Video"},
};
const int kExtraMediaCount = (int)(sizeof(kExtraMedia) / sizeof(kExtraMedia[0]));

// Record where the service has an extra medium, for fetchExtraMedia.
static void offerExtraMedia(ExtraMediaJob& x, const std::string& url, const std::string& ext) {
    if (url.empty()) return;
    x.offered = true;
    x.url = url;
    x.ext = ext;
}

bool fetchExtraMedia(ExtraMediaJob& x, const std::string& tmpTag, const std::function<bool()>& cancelled) {
    if (!x.offered) return false;
    std::string ext = x.ext;
    const int kind = kExtraMedia[x.type].kind;
    // Only the extensions the media lookup knows, so a saved file is always found again.
    static const char* kImg[] = {"png", "jpg", "jpeg", "webp", nullptr};
    static const char* kVid[] = {"mp4", "mkv", "webm", "avi", nullptr};
    static const char* kPdf[] = {"pdf", nullptr};
    const char** known = kind == EXTRA_PDF ? kPdf : kind == EXTRA_VIDEO ? kVid : kImg;
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    bool extOk = false;
    for (const char** e = known; *e; e++) if (ext == *e) extOk = true;
    if (!extOk) ext = known[0];
    if (ext == "jpeg") ext = "jpg";
    mkdirsFor(x.stem);
    const std::string dest = x.stem + "." + ext, tmp = dest + ".tmp-" + tmpTag;
    std::vector<std::pair<std::string, std::string>> none;
    Downloading what(kExtraMedia[x.type].one);
    const curl_off_t cap = kind == EXTRA_VIDEO ? kMaxVideoBytes : kind == EXTRA_PDF ? kMaxManualBytes : kMaxApiBytes;
    bool ok = curlToFile(x.url, none, tmp, 60, kind != EXTRA_IMAGE, cancelled, cap);
    if (ok && !(kind == EXTRA_PDF ? looksLikePdf(tmp) : kind == EXTRA_VIDEO ? looksLikeVideo(tmp) : looksLikeImage(tmp))) {
        x.offered = false;   // what the service has is not a usable file: treat it as not offered
        ok = false;
    }
    if (!ok || rename(tmp.c_str(), dest.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    for (const char** e = known; *e; e++)
        if (ext != *e) unlink((x.stem + "." + *e).c_str());
    x.written = dest;
    return true;
}

// ---------------------------------------------------------------------------
// Region preference + media selection
// ---------------------------------------------------------------------------
// Region preference, mirroring es-de's ScreenScraper fallback chain
// ({region, wor, us, ss, eu, jp, cus}). The requested region is tried first, then
// the universal fallbacks; "ss" picks up media for unofficial titles.
static std::vector<std::string> regionOrder(const std::string& region) {
    std::vector<std::string> base = {"wor", "us", "ss", "eu", "jp", "cus"};
    std::string r = region.empty() ? "us" : region;
    std::vector<std::string> out;
    out.push_back(r);
    for (const std::string& b : base) if (b != r) out.push_back(b);
    return out;
}

// es-de hashes only files up to a size cap (default 32 MB, extended 800 MB) and
// matches larger files (CD/DVD images) by name. 128 MB covers every cartridge
// system while skipping disc images, keeping the worker responsive on eMMC.
static const off_t kHashMaxBytes = 128LL * 1024 * 1024;

// ---------------------------------------------------------------------------
// ScreenScraper.fr
// ---------------------------------------------------------------------------
// ScreenScraper's plain-text errors are in French: the known ones in the user's language, others as
// sent. *gameOnly: about this game's request alone (not the service or the account).
static std::string ssErrorText(const std::string& body, bool* gameOnly) {
    std::string low;
    for (unsigned char c : body) low += (char)tolower(c);
    static const struct { const char* match; const char* text; bool game; } kKnown[] = {
        {"systemid obligatoire", "ScreenScraper needs this game's system or a CRC to find it.", true},
        {"crc, md5 ou sha1", "ScreenScraper rejected this game's checksum.", true},
        {"nom du fichier", "ScreenScraper rejected this game's file name.", true},
        {"identifiants d\xc3\xa9veloppeur", "ScreenScraper did not accept the developer login.", false},
        {"identifiants developpeur", "ScreenScraper did not accept the developer login.", false},
        {"erreur de login", "ScreenScraper did not accept the login. Check the user name and password.", false},
        {"faite du tri", "ScreenScraper has had too many unknown games from this account today. Try again tomorrow.", false},
        {"quota de scrape", "ScreenScraper's daily scrape limit for this account is used up. Try again tomorrow.", false},
        {"threads", "ScreenScraper is busy for this account (too many scrapes at once). Try again shortly.", false},
        {"blacklist", "ScreenScraper has blocked this app.", false},
        {"api ferm", "ScreenScraper's API is closed right now. Try again later.", false},
        {"api closed", "ScreenScraper's API is closed right now. Try again later.", false},
    };
    *gameOnly = false;
    for (const auto& k : kKnown)
        if (low.find(k.match) != std::string::npos) { *gameOnly = k.game; return trDyn(k.text); }
    std::string t = body.size() > 120 ? body.substr(0, 120) : body;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    return std::string(trDyn("ScreenScraper: ")) + t;
}

static ScrapeOutcome scrapeScreenScraper(const Credentials& cred,
                                         const std::string& romPath,
                                         const std::string& displayName,
                                         const PlatformIds& plat,
                                         bool wantBox, bool wantFan,
                                         const std::string& cacheDir,
                                         const std::string& tmpTag,
                                         const std::string& queryName,
                                         std::vector<ExtraMediaJob>* extras,
                                         const std::function<bool()>& cancelled,
                                         const ArtDest& dest) {
    ScrapeOutcome r;
    if (cred.ssDevId.empty() || cred.ssDevPw.empty()) {
        r.credMissing = true;
        r.error = "ScreenScraper developer credentials not set";
        return r;
    }
    const std::string key   = cacheKey(romPath);
    const std::string resp  = respDir() + "/.resp-" + tmpTag + ".json";
    // A user title override drives romnom (crc + size still sent); otherwise the raw
    // filename as before. A game of ScummVM's list goes by its game ID, the name ScreenScraper's
    // ScummVM games carry (sky for Beneath a Steel Sky).
    std::string romnom = queryName.empty() ? baseName(romPath) : queryName;
    if (queryName.empty()) {
        const std::string gameId = nanoscummvm::mirrorGameId(romPath);
        if (!gameId.empty()) romnom = gameId;
    }
    // An Android Games file only names the game's package, so there is nothing to hash or match
    // as a ROM: search by the game's name instead. So is a file with no CRC (one too big to hash)
    // of a system ScreenScraper has no id for: jeuInfos.php needs one of the two.
    bool byName = romPath.size() > 4 && romPath.compare(romPath.size() - 4, 4, ".app") == 0;
    const std::string crc = byName ? std::string() : crc32File(romPath, kHashMaxBytes);
    if (!byName && plat.ss <= 0 && crc.empty()) byName = true;

    std::vector<std::pair<std::string, std::string>> q = {
        {"devid", cred.ssDevId}, {"devpassword", cred.ssDevPw},
        {"softname", cred.ssSoftname.empty() ? std::string("gammaos-nano") : cred.ssSoftname},
        {"output", "json"},
    };
    if (!cred.ssUser.empty()) { q.push_back({"ssid", cred.ssUser}); q.push_back({"sspassword", cred.ssPass}); }
    if (plat.ss > 0) q.push_back({"systemeid", std::to_string(plat.ss)});
    const std::string recherche = cleanQueryName(queryName.empty() ? stripExt(baseName(romPath)) : queryName);
    if (byName) {
        q.push_back({"recherche", recherche});
    } else {
        q.push_back({"romnom", romnom});
        if (!crc.empty()) q.push_back({"crc", crc});
        off_t fsz = 0; if (fileExistsNonEmpty(romPath, &fsz)) q.push_back({"romtaille", std::to_string((long long)fsz)});
    }

    const char* api = byName ? "https://api.screenscraper.fr/api2/jeuRecherche.php"
                             : "https://api.screenscraper.fr/api2/jeuInfos.php";
    if (!curlToFile(api, q, resp, 30, false, cancelled)) {
        r.networkFail = true; r.error = trDyn("ScreenScraper request failed"); return r;
    }
    std::string body = readFile(resp, 2 * 1024 * 1024);
    unlink(resp.c_str());
    // ScreenScraper returns a plain-text error (not JSON) on auth / quota / no-game. Only "not found"
    // says the game is unknown; anything else (bad login, quota or thread limits, a server error
    // page, a cut-off reply) is a temporary failure, tried again next time.
    if (body.empty() || (body[0] != '{' && body[0] != '[')) {
        if (body.find("non trouv") != std::string::npos) {
            r.notFound = true;
            r.error = "No match";
        } else {
            bool gameOnly = false;
            r.error = ssErrorText(body, &gameOnly);
            r.networkFail = !gameOnly;   // a request this game alone could not make is no outage
        }
        return r;
    }
    njson::Value root;
    if (!njson::parse(body, &root)) { r.networkFail = true; r.error = trDyn("ScreenScraper: bad JSON"); return r; }
    const njson::Value* jeu = nullptr;
    if (const njson::Value* rp = root.find("response")) {
        if (!byName) {
            jeu = rp->find("jeu");
        } else if (const njson::Value* jeux = rp->find("jeux")) {
            // Best match first, but a search also returns games that only share a word: take the
            // first whose name is the searched one, or holds it or is held in it. No match is a
            // list holding one empty game.
            auto key = [](const std::string& t) {
                std::string k;
                for (unsigned char c : t) if (isalnum(c)) k += (char)tolower(c);
                return k;
            };
            const std::string want = key(recherche);
            if (jeux->isArray() && !want.empty())
                for (const auto& g : jeux->arr) {
                    const njson::Value* noms = g.isObject() ? g.find("noms") : nullptr;
                    if (!noms || !noms->isArray()) continue;
                    for (const auto& nm : noms->arr) {
                        const std::string k = key(nm.getString("text"));
                        if (k == want || (std::min(k.size(), want.size()) >= 4 &&
                                          (k.find(want) != std::string::npos || want.find(k) != std::string::npos))) {
                            jeu = &g;
                            break;
                        }
                    }
                    if (jeu) break;
                }
        }
    }
    if (!jeu || !jeu->isObject()) { r.notFound = true; r.error = "No match"; return r; }

    std::vector<std::string> regs = regionOrder(cred.region);

    // Title: noms[] are keyed by REGION only (ScreenScraper has no per-name
    // language field, unlike synopsis/genre which use "langue"). Prefer
    // English-speaking regions in a fixed order (independent of the media `regs`
    // chain) so we never default to the French community ("ss"/"cus"/"fr") title.
    // An English Scrape Region (us/eu/wor) is honoured first.
    {
        std::vector<std::string> titleRegs = {"us", "wor", "eu", "uk", "au", "ca"};
        if (cred.region == "eu" || cred.region == "us" || cred.region == "wor") {
            std::string r0 = cred.region;
            titleRegs.erase(std::remove(titleRegs.begin(), titleRegs.end(), r0),
                            titleRegs.end());
            titleRegs.insert(titleRegs.begin(), r0);
        }
        std::string picked;
        if (const njson::Value* noms = jeu->find("noms")) {
            if (noms->isArray() && !noms->arr.empty()) {
                // Pass 1: strict English-region preference.
                for (const std::string& want : titleRegs) {
                    for (const auto& nm : noms->arr) {
                        if (nm.getString("region") != want) continue;
                        std::string tx = nm.getString("text");
                        if (!tx.empty()) { picked = tx; break; }
                    }
                    if (!picked.empty()) break;
                }
                // Pass 2: world / Japanese (romanised) before any French entry.
                if (picked.empty()) {
                    static const char* kFallback[] = {"wor", "us", "jp"};
                    for (const char* want : kFallback) {
                        for (const auto& nm : noms->arr) {
                            if (nm.getString("region") != want) continue;
                            std::string tx = nm.getString("text");
                            if (!tx.empty()) { picked = tx; break; }
                        }
                        if (!picked.empty()) break;
                    }
                }
            }
        }
        r.title = picked;
    }
    // Do NOT fall back to jeu["nom"] (account-region/language default, French for
    // the shipped dev account); use the local filename instead.
    if (r.title.empty()) r.title = stripExt(displayName);

    // Game metadata for the Information screen (best effort). language-/region-preferred.
    auto langText = [](const njson::Value* arr) -> std::string {
        if (!arr || !arr->isArray() || arr->arr.empty()) return std::string();
        std::string first, picked;
        for (const auto& o : arr->arr) {
            std::string lg = o.getString("langue"), tx = o.getString("text");
            if (first.empty()) first = tx;
            if (lg == "en" && picked.empty()) picked = tx;
        }
        return picked.empty() ? first : picked;
    };
    r.synopsis = langText(jeu->find("synopsis"));
    if (const njson::Value* gs = jeu->find("genres")) {
        if (gs->isArray() && !gs->arr.empty()) {
            const njson::Value* gp = &gs->arr[0];
            for (const auto& g : gs->arr) if (g.getString("principale") == "1") { gp = &g; break; }
            r.genre = langText(gp->find("noms"));
        }
    }
    if (const njson::Value* jo = jeu->find("joueurs")) r.players = jo->getString("text");
    if (const njson::Value* nt = jeu->find("note")) { std::string v = nt->getString("text"); if (!v.empty()) r.rating = v + "/20"; }
    if (const njson::Value* ds = jeu->find("dates")) {
        if (ds->isArray() && !ds->arr.empty()) {
            std::string first, picked;
            for (const auto& d : ds->arr) {
                std::string rg = d.getString("region"), tx = d.getString("text");
                if (first.empty()) first = tx;
                for (const std::string& want : regs) if (rg == want && picked.empty()) picked = tx;
            }
            r.releaseDate = picked.empty() ? first : picked;
        }
    }
    if (const njson::Value* dv = jeu->find("developpeur")) r.developer = dv->getString("text");
    if (const njson::Value* ed = jeu->find("editeur")) r.publisher = ed->getString("text");
    htmlUnescape(r.synopsis); htmlUnescape(r.genre); htmlUnescape(r.players);
    htmlUnescape(r.developer); htmlUnescape(r.publisher); htmlUnescape(r.releaseDate);

    r.matched = true;
    const njson::Value* medias = jeu->find("medias");
    if (!medias || !medias->isArray()) { r.error = "No media"; return r; }

    // Pick the best medium among a list of acceptable types, region-preferred.
    auto pickMedia = [&](const std::vector<std::string>& types) -> const njson::Value* {
        // First pass: exact type + region preference. Second pass: type, any region.
        for (const std::string& want : regs) {
            for (const std::string& ty : types)
                for (const auto& m : medias->arr) {
                    if (m.getString("type") != ty) continue;
                    if (m.getString("region") != want) continue;
                    if (!m.getString("url").empty()) return &m;
                }
        }
        for (const std::string& ty : types)
            for (const auto& m : medias->arr) {
                if (m.getString("type") != ty) continue;
                if (!m.getString("url").empty()) return &m;
            }
        return nullptr;
    };
    auto pick = [&](const std::vector<std::string>& types) -> std::string {
        const njson::Value* m = pickMedia(types);
        return m ? m->getString("url") : std::string();
    };

    auto download = [&](const std::string& url, const std::string& destFinal, const char* what) -> bool {
        if (url.empty()) return false;
        std::string tmp = destFinal + ".tmp-" + tmpTag;
        std::vector<std::pair<std::string, std::string>> none;
        Downloading d(what);
        if (!curlToFile(url, none, tmp, 60, false, cancelled)) return false;
        if (!looksLikeImage(tmp)) { unlink(tmp.c_str()); return false; }
        if (rename(tmp.c_str(), destFinal.c_str()) != 0) { unlink(tmp.c_str()); return false; }
        return true;
    };

    if (wantBox) {
        // Cover = box-2D (es-de media_cover); 3D box is the fallback.
        std::string u = pick({"box-2D", "box-3D"});
        const std::string to = dest.box.empty() ? cacheDir + "/" + key + ".box.png" : dest.box;
        const std::string have = existingArt(dest.box, dest.keep);
        if (!have.empty()) { r.boxFile = have; r.kept |= 1; }
        else {
            if (!dest.box.empty()) mkdirsFor(to);
            if (download(u, to, "Cover")) { r.boxFile = to; dropOtherArt(dest.box); }
        }
    }
    if (wantFan) {
        // Background = fanart (es-de media_fanart); fall back to an in-game
        // screenshot then the title screen when no dedicated fanart exists.
        std::string u = pick({"fanart", "ss", "sstitle"});
        const std::string to = dest.fan.empty() ? cacheDir + "/" + key + ".fan.jpg" : dest.fan;
        const std::string have = existingArt(dest.fan, dest.keep);
        if (!have.empty()) { r.fanFile = have; r.kept |= 2; }
        else {
            if (!dest.fan.empty()) mkdirsFor(to);
            if (download(u, to, "Fan Art")) { r.fanFile = to; dropOtherArt(dest.fan); }
        }
    }
    if (extras) {
        for (auto& x : *extras) {
            std::vector<std::string> types;
            std::string list = kExtraMedia[x.type].ssTypes;
            for (size_t s = 0; s <= list.size();) {
                size_t c = list.find(',', s);
                if (c == std::string::npos) c = list.size();
                types.push_back(list.substr(s, c - s));
                s = c + 1;
            }
            if (const njson::Value* m = pickMedia(types)) offerExtraMedia(x, m->getString("url"), m->getString("format"));
        }
    }
    r.ok = !r.boxFile.empty() || !r.fanFile.empty();
    return r;
}

// ---------------------------------------------------------------------------
// TheGamesDB resource maps (genre / developer / publisher id -> name). The game
// records reference these by numeric id; the names live in three separate static
// lists. We fetch them ONCE per scrape (the first TheGamesDB game), cache them in
// a file-static, and free them when the worker finishes (freeTgdbResources, called
// from the scrape worker). TheGamesDB jobs run on one thread and only one scrape
// runs at a time, so no locking is needed. Nothing is held at idle.
// ---------------------------------------------------------------------------
namespace {
struct TgdbResMaps {
    std::map<int, std::string> genre, developer, publisher;
    bool loaded = false;
};
TgdbResMaps gTgdbRes;

// Fetch one resource endpoint (Genres/Developers/Publishers) and fold its
// {idstr:{id,name}} object into id->name.
void loadTgdbResMap(const std::string& key, const char* endpoint,
                    const char* member, const std::string& resp,
                    std::map<int, std::string>& out, const std::function<bool()>& cancelled) {
    std::vector<std::pair<std::string, std::string>> q = {{"apikey", key}};
    std::string url = std::string("https://api.thegamesdb.net/v1/") + endpoint;
    if (!curlToFile(url, q, resp, 30, false, cancelled)) return;
    std::string body = readFile(resp, 4 * 1024 * 1024);
    unlink(resp.c_str());
    njson::Value root;
    if (body.empty() || !njson::parse(body, &root)) return;
    const njson::Value* data = root.find("data");
    const njson::Value* m = data ? data->find(member) : nullptr;
    if (!m || !m->isObject()) return;
    for (const auto& kv : m->obj) {
        int id = kv.second.getInt("id", -1);
        std::string nm = kv.second.getString("name");
        if (id >= 0 && !nm.empty()) out[id] = nm;
    }
}

void ensureTgdbResMaps(const std::string& key, const std::string& resp, const std::function<bool()>& cancelled) {
    if (gTgdbRes.loaded) return;
    gTgdbRes.loaded = true;   // set first: a failed fetch is not retried per ROM
    loadTgdbResMap(key, "Genres",     "genres",     resp, gTgdbRes.genre, cancelled);
    loadTgdbResMap(key, "Developers", "developers", resp, gTgdbRes.developer, cancelled);
    loadTgdbResMap(key, "Publishers", "publishers", resp, gTgdbRes.publisher, cancelled);
}

// Map the first known id in a game's int-array field to its resource name.
std::string firstResName(const njson::Value* game, const char* field,
                         const std::map<int, std::string>& m) {
    const njson::Value* a = game ? game->find(field) : nullptr;
    if (!a || !a->isArray()) return "";
    for (const auto& e : a->arr) {
        if (!e.isNumber()) continue;
        auto it = m.find(e.asInt());
        if (it != m.end()) return it->second;
    }
    return "";
}
}  // namespace

// Free the cached TheGamesDB resource maps (called by the scrape worker on finish so
// nothing lingers at idle). Idempotent.
void freeTgdbResources() {
    gTgdbRes.genre.clear();
    gTgdbRes.developer.clear();
    gTgdbRes.publisher.clear();
    gTgdbRes.loaded = false;
}

// ---------------------------------------------------------------------------
// TheGamesDB
// ---------------------------------------------------------------------------
static ScrapeOutcome scrapeTheGamesDb(const Credentials& cred,
                                      const std::string& romPath,
                                      const std::string& displayName,
                                      const PlatformIds& plat,
                                      bool wantBox, bool wantFan,
                                      const std::string& cacheDir,
                                      const std::string& tmpTag,
                                      const std::string& queryName,
                                      std::vector<ExtraMediaJob>* extras,
                                      const std::function<bool()>& cancelled,
                                      const ArtDest& dest) {
    ScrapeOutcome r;
    if (cred.tgdbKey.empty()) {
        r.credMissing = true;
        r.error = "TheGamesDB API key not set";
        return r;
    }
    const std::string key   = cacheKey(romPath);
    const std::string resp  = respDir() + "/.resp-" + tmpTag + ".json";
    // A user title override drives the name query (still cleaned of region/version
    // tags); otherwise the filename stem as before.
    const std::string qname = queryName.empty()
        ? cleanQueryName(stripExt(baseName(romPath)))
        : cleanQueryName(queryName);

    // 1) ByGameName -> game id. fields includes "platform" so we can prefer a
    // result on the requested platform; filter[platform] also narrows server-side.
    std::vector<std::pair<std::string, std::string>> q = {
        {"apikey", cred.tgdbKey},
        {"fields", "players,publishers,developers,genres,overview,platform,rating,release_date"},
        {"name", qname},
    };
    if (plat.tgdb > 0) q.push_back({"filter[platform]", std::to_string(plat.tgdb)});
    if (!curlToFile("https://api.thegamesdb.net/v1/Games/ByGameName", q, resp, 30, false, cancelled)) {
        r.networkFail = true; r.error = trDyn("TheGamesDB request failed"); return r;
    }
    std::string body = readFile(resp, 2 * 1024 * 1024);
    unlink(resp.c_str());
    njson::Value root;
    if (body.empty() || !njson::parse(body, &root)) { r.networkFail = true; r.error = trDyn("TheGamesDB: bad response"); return r; }
    int code = root.getInt("code", 0);
    if (code == 403 || code == 401) { r.networkFail = true; r.error = trDyn("TheGamesDB: invalid API key"); return r; }

    const njson::Value* data = root.find("data");
    const njson::Value* games = data ? data->find("games") : nullptr;
    if (!games || !games->isArray() || games->arr.empty()) { r.notFound = true; r.error = "No match"; return r; }

    int gameId = -1;
    // Prefer a game on the requested platform; otherwise the first result.
    const njson::Value* gsel = nullptr;
    if (plat.tgdb > 0)
        for (const auto& g : games->arr)
            if (g.getInt("platform", -1) == plat.tgdb) { gsel = &g; break; }
    if (!gsel) gsel = &games->arr[0];
    gameId = gsel->getInt("id", -1);
    r.title = gsel->getString("game_title");
    if (gameId < 0) { r.error = "No match"; return r; }
    if (r.title.empty()) r.title = stripExt(displayName);
    // Metadata. overview/players/rating/release_date are directly readable; genre,
    // developer and publisher are numeric ids resolved through the resource maps
    // (fetched once per scrape, freed on finish).
    r.synopsis = gsel->getString("overview");
    { int pl = gsel->getInt("players", 0); if (pl > 0) r.players = std::to_string(pl); }
    r.rating = gsel->getString("rating");
    r.releaseDate = gsel->getString("release_date");
    if (r.releaseDate.size() > 10) r.releaseDate = r.releaseDate.substr(0, 10);
    ensureTgdbResMaps(cred.tgdbKey, resp, cancelled);
    r.genre     = firstResName(gsel, "genres",     gTgdbRes.genre);
    r.developer = firstResName(gsel, "developers", gTgdbRes.developer);
    r.publisher = firstResName(gsel, "publishers", gTgdbRes.publisher);
    htmlUnescape(r.synopsis); htmlUnescape(r.rating);
    htmlUnescape(r.genre); htmlUnescape(r.developer); htmlUnescape(r.publisher);

    // 2) Images -> boxart + fanart filenames. es-de fetches all image types and
    // filters client-side (no filter[type]) so screenshot/titlescreen remain as
    // background fallbacks.
    std::vector<std::pair<std::string, std::string>> q2 = {
        {"apikey", cred.tgdbKey}, {"games_id", std::to_string(gameId)},
    };
    if (!curlToFile("https://api.thegamesdb.net/v1/Games/Images", q2, resp, 30, false, cancelled)) {
        r.networkFail = true; r.error = trDyn("TheGamesDB images request failed"); return r;
    }
    std::string body2 = readFile(resp, 2 * 1024 * 1024);
    unlink(resp.c_str());
    njson::Value root2;
    if (body2.empty() || !njson::parse(body2, &root2)) { r.networkFail = true; r.error = trDyn("TheGamesDB: bad images response"); return r; }
    const njson::Value* d2 = root2.find("data");
    if (!d2) { r.error = "No media"; return r; }
    std::string base;
    if (const njson::Value* bu = d2->find("base_url")) {
        base = bu->getString("original");
        if (base.empty()) base = bu->getString("large");
        if (base.empty()) base = bu->getString("medium");
    }
    const njson::Value* images = d2->find("images");
    const njson::Value* arr = nullptr;
    if (images && images->isObject()) {
        // Keyed by the game id as a string; there is only the one we asked for.
        const njson::Value* byId = images->find(std::to_string(gameId));
        if (byId && byId->isArray()) arr = byId;
        else if (!images->obj.empty() && images->obj[0].second.isArray()) arr = &images->obj[0].second;
    }
    r.matched = true;
    if (base.empty() || !arr) { r.error = "No media"; return r; }

    auto findImg = [&](const char* type, const char* sidePref) -> std::string {
        std::string anyType;
        for (const auto& im : arr->arr) {
            if (im.getString("type") != type) continue;
            std::string fn = im.getString("filename");
            if (fn.empty()) continue;
            if (sidePref && im.getString("side") == sidePref) return base + fn;
            if (anyType.empty()) anyType = base + fn;
        }
        return anyType;
    };

    auto download = [&](const std::string& url, const std::string& destFinal, const char* what) -> bool {
        if (url.empty()) return false;
        std::string tmp = destFinal + ".tmp-" + tmpTag;
        std::vector<std::pair<std::string, std::string>> none;
        Downloading d(what);
        if (!curlToFile(url, none, tmp, 60, false, cancelled)) return false;
        if (!looksLikeImage(tmp)) { unlink(tmp.c_str()); return false; }
        if (rename(tmp.c_str(), destFinal.c_str()) != 0) { unlink(tmp.c_str()); return false; }
        return true;
    };

    if (wantBox) {
        std::string u = findImg("boxart", "front");
        const std::string to = dest.box.empty() ? cacheDir + "/" + key + ".box.png" : dest.box;
        const std::string have = existingArt(dest.box, dest.keep);
        if (!have.empty()) { r.boxFile = have; r.kept |= 1; }
        else {
            if (!dest.box.empty()) mkdirsFor(to);
            if (download(u, to, "Cover")) { r.boxFile = to; dropOtherArt(dest.box); }
        }
    }
    if (wantFan) {
        std::string u = findImg("fanart", nullptr);
        const std::string to = dest.fan.empty() ? cacheDir + "/" + key + ".fan.jpg" : dest.fan;
        const std::string have = existingArt(dest.fan, dest.keep);
        if (!have.empty()) { r.fanFile = have; r.kept |= 2; }
        else {
            if (!dest.fan.empty()) mkdirsFor(to);
            if (download(u, to, "Fan Art")) { r.fanFile = to; dropOtherArt(dest.fan); }
        }
    }
    if (extras) {
        for (auto& x : *extras) {
            const ExtraMediaType& t = kExtraMedia[x.type];
            if (!t.tgdbType) continue;
            std::string fn;
            for (const auto& im : arr->arr) {
                if (im.getString("type") != t.tgdbType) continue;
                if (t.tgdbSide && im.getString("side") != t.tgdbSide) continue;
                fn = im.getString("filename");
                if (!fn.empty()) break;
            }
            if (fn.empty()) continue;
            size_t dot = fn.find_last_of('.');
            std::string ext = (dot == std::string::npos) ? std::string() : fn.substr(dot + 1);
            offerExtraMedia(x, base + fn, ext);
        }
    }
    r.ok = !r.boxFile.empty() || !r.fanFile.empty();
    return r;
}

// ---------------------------------------------------------------------------
int screenScraperMaxThreads(const Credentials& cred, const std::string& /*cacheDir*/,
                            const std::function<bool()>& cancelled) {
    // Without a user account the limit cannot be read, and an unclear answer is not trusted.
    if (cred.ssDevId.empty() || cred.ssDevPw.empty() || cred.ssUser.empty()) return 1;
    const std::string resp = respDir() + "/.resp-limits.json";
    std::vector<std::pair<std::string, std::string>> q = {
        {"devid", cred.ssDevId}, {"devpassword", cred.ssDevPw},
        {"softname", cred.ssSoftname.empty() ? std::string("gammaos-nano") : cred.ssSoftname},
        {"output", "json"}, {"ssid", cred.ssUser}, {"sspassword", cred.ssPass},
    };
    if (!curlToFile("https://api.screenscraper.fr/api2/ssuserInfos.php", q, resp, 30, false, cancelled)) return 1;
    const std::string body = readFile(resp, 256 * 1024);
    unlink(resp.c_str());
    njson::Value root;
    if (body.empty() || body[0] != '{' || !njson::parse(body, &root)) return 1;
    const njson::Value* rp = root.find("response");
    const njson::Value* user = rp ? rp->find("ssuser") : nullptr;
    const njson::Value* mt = user ? user->find("maxthreads") : nullptr;
    long n = 0;
    if (mt && mt->isNumber() && mt->num == (double)(long)mt->num) {
        n = (long)mt->num;
    } else if (mt && mt->isString() && !mt->str.empty() &&
               mt->str.find_first_not_of("0123456789") == std::string::npos && mt->str.size() < 4) {
        n = strtol(mt->str.c_str(), nullptr, 10);
    }
    if (n < 1) return 1;
    return (int)std::min<long>(n, kMaxScrapeThreads);
}

ScrapeOutcome scrapeRom(Engine engine, const Credentials& cred,
                        const std::string& romPath, const std::string& displayName,
                        const PlatformIds& plat, bool wantBox, bool wantFan,
                        const std::string& cacheDir, const std::string& tmpTag,
                        const std::string& queryName,
                        std::vector<ExtraMediaJob>* extras,
                        const std::function<bool()>& cancelled,
                        const ArtDest& dest) {
    if (engine == ENGINE_OFF || (!wantBox && !wantFan && (!extras || extras->empty()))) {
        ScrapeOutcome r; r.error = "Scraping disabled"; return r;
    }
    if (engine == ENGINE_THEGAMESDB)
        return scrapeTheGamesDb(cred, romPath, displayName, plat, wantBox, wantFan, cacheDir, tmpTag, queryName, extras,
                                cancelled, dest);
    return scrapeScreenScraper(cred, romPath, displayName, plat, wantBox, wantFan, cacheDir, tmpTag, queryName, extras,
                               cancelled, dest);
}

} // namespace nanoscraper
} // namespace android
