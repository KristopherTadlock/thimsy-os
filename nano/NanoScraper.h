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

// Boxart / cover scraper engine for GammaOS Nano. Self-contained (no GL): fetches
// game cover + background art from ScreenScraper.fr or TheGamesDB with libcurl and
// writes them to a disk cache. Modeled on EmulationStation-DE.
//
// Both services require user-supplied credentials (a ScreenScraper developer
// account, or a TheGamesDB API key); the engine reports a clear error when they
// are missing. The caller (NanoMenuScraper.cpp) runs scrapeRom() on worker
// threads and consumes the written files on the render thread.

#ifndef GAMMAOS_NANO_SCRAPER_H
#define GAMMAOS_NANO_SCRAPER_H

#include <functional>
#include <string>
#include <vector>

namespace android {
namespace nanoscraper {

enum Engine {
    ENGINE_SCREENSCRAPER = 0,
    ENGINE_THEGAMESDB    = 1,
    ENGINE_OFF           = 2,
};

// Parse an engine name ("screenscraper" / "thegamesdb" / "off"); default
// ScreenScraper for unknown/empty.
Engine engineFromName(const std::string& name);

struct Credentials {
    // ScreenScraper.fr
    std::string ssDevId;
    std::string ssDevPw;
    std::string ssSoftname;   // registered application name for the dev account
    std::string ssUser;
    std::string ssPass;
    // TheGamesDB
    std::string tgdbKey;
    // Preferred media region: us | eu | jp | wor
    std::string region;
};

struct PlatformIds {
    int ss   = -1;   // ScreenScraper systemeid
    int tgdb = -1;   // TheGamesDB platform id
};

// Resolve a system to scraper platform ids from its romDir / shortname / name.
// manualSsId (if a positive integer string) forces the ScreenScraper systemeid.
PlatformIds platformForSystem(const std::string& romDir,
                              const std::string& shortname,
                              const std::string& name,
                              const std::string& manualSsId);

// Extra media the user can turn on (Boxart Scraper > Content Settings), saved in ES-DE's media layout.
enum ExtraMediaKind { EXTRA_IMAGE = 0, EXTRA_PDF = 1, EXTRA_VIDEO = 2 };
struct ExtraMediaType {
    const char* key;       // ES-DE media type, and persist.gammaos.scraper.media.<key>
    const char* label;     // settings row
    const char* subdir;    // ES-DE downloaded_media/<system>/<subdir>
    const char* ssTypes;   // ScreenScraper media types, best first, comma separated
    const char* tgdbType;  // TheGamesDB image type (nullptr = not offered)
    const char* tgdbSide;  // TheGamesDB boxart side, or nullptr
    int kind;              // ExtraMediaKind
    const char* one;       // one file of it, for the progress line ("Manual")
};
extern const ExtraMediaType kExtraMedia[];
extern const int kExtraMediaCount;

// One extra medium to fetch for a game: the kExtraMedia index and the destination path without its
// extension (the service's file format picks it). scrapeRom sets offered, with the url and format,
// when the service has the medium; fetchExtraMedia sets written when the file is saved.
struct ExtraMediaJob {
    int type = 0;
    std::string stem;
    bool offered = false;
    std::string url, ext;
    std::string written;
};

struct ScrapeOutcome {
    bool ok = false;            // the cover or fan art was written
    int kept = 0;               // 1 cover, 2 fan art: one already there was used (ArtDest::keep), not downloaded
    bool matched = false;       // the service identified the game (its media list was read)
    bool notFound = false;      // the service answered that it does not know the game
    bool networkFail = false;   // could not reach / authenticate the API
    bool credMissing = false;   // required credentials are not configured
    std::string title;          // matched game title (best effort)
    std::string boxFile;        // written cover path, or empty
    std::string fanFile;        // written fanart path, or empty
    std::string error;          // human-readable failure reason
    // Game metadata for the Information screen (best effort; empty = no data).
    std::string synopsis;       // description / overview
    std::string genre;
    std::string players;        // e.g. "1" or "1-2"
    std::string rating;         // ScreenScraper "16/20"; TheGamesDB e.g. "E - Everyone"
    std::string releaseDate;    // "YYYY-MM-DD" or "YYYY"
    std::string developer;
    std::string publisher;
};

// Scrape one ROM. Writes "<key>.box.png" / "<key>.fan.jpg" into cacheDir (key =
// cacheKey(romPath)) and marks which of extras (may be null) the service offers; fetchExtraMedia
// downloads those afterwards, so covers are not held up by manuals and videos. cancelled (may be
// empty) stops a transfer in progress. tmpTag is a unique per-call scratch suffix so concurrent
// callers do not clobber each other's response/temp files. queryName, when non-empty,
// is used as the search query (a user title override) instead of the ROM filename;
// empty = query by filename as before.
// Where the cover and fan art go ("" = cacheDir/<cacheKey>.box.png / .fan.jpg): the ES-DE folder's
// covers/ and fanart/ when Nano keeps its data there.
struct ArtDest {
    std::string box, fan;
    bool keep = false;   // a picture already there (any image type) is the game's: not downloaded again
};

// /storage/<volume> or /mnt/media_rw/<volume>: never made by a mkdir (with the card out that would
// put a folder where the card mounts).
inline bool isVolumeRoot(const std::string& p) {
    for (const char* pre : {"/storage/", "/mnt/media_rw/"}) {
        const size_t n = strlen(pre);
        if (p.compare(0, n, pre) == 0 && p.find('/', n) == std::string::npos) return true;
    }
    return p == "/storage" || p == "/mnt/media_rw";
}
ScrapeOutcome scrapeRom(Engine engine, const Credentials& cred,
                        const std::string& romPath, const std::string& displayName,
                        const PlatformIds& plat, bool wantBox, bool wantFan,
                        const std::string& cacheDir, const std::string& tmpTag,
                        const std::string& queryName = std::string(),
                        std::vector<ExtraMediaJob>* extras = nullptr,
                        const std::function<bool()>& cancelled = nullptr,
                        const ArtDest& dest = ArtDest());

// Download an extra medium scrapeRom found to its stem + extension, checking it is the kind of file
// it should be (if not, offered is cleared). A copy saved earlier under another extension is removed.
bool fetchExtraMedia(ExtraMediaJob& x, const std::string& tmpTag, const std::function<bool()>& cancelled);

// Most requests a scrape makes at once, whatever an account allows.
constexpr int kMaxScrapeThreads = 4;

// How many requests the ScreenScraper account in cred may make at once (its maxthreads), capped at
// kMaxScrapeThreads. 1 when there is no user account, the request fails or is cancelled, or the
// answer is unclear.
int screenScraperMaxThreads(const Credentials& cred, const std::string& cacheDir,
                            const std::function<bool()>& cancelled);

// Stable 64-bit FNV-1a hex key for a rom path (cache filename stem).
std::string cacheKey(const std::string& romPath);

// Release the cached TheGamesDB resource maps (genre/developer/publisher id->name).
// They are fetched lazily on the first TheGamesDB game of a scrape; the scrape worker
// calls this when it finishes so nothing is held at idle. Idempotent.
void freeTgdbResources();

// Progress of the art download running on this thread: what it is ("Cover", "Manual"; "" once it
// finishes) and its speed in bytes per second, reported about twice a second.
using DownloadReporter = std::function<void(const std::string& what, double bytesPerSec)>;
void setDownloadReporter(DownloadReporter reporter);

} // namespace nanoscraper
} // namespace android

#endif // GAMMAOS_NANO_SCRAPER_H
