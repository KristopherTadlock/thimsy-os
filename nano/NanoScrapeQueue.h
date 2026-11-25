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

// How a Nano home and the background scrape service (gammaos-nano-scrape) talk. A scrape runs in
// the service so it outlives the home (which exits when it starts a game) and is the same scrape
// whichever home (the DRM home or the overlay) is on screen. A home puts the games in a request file
// in kQueueDir, named with a new run token, and sets kRunProp to the token, which starts the
// service; the service takes only that run's requests. While it runs, more
// requests can be added; it takes them one at a time, oldest first, and exits when none is left. It
// writes its progress to kProgressPath a few times a second and appends each finished game to
// kResultsPath as a line of JSON; every home applies the lines it has not seen to its manifest.
// Setting kCancelProp to the run token stops the run and drops the waiting requests. Taking the
// last request and the final progress happen under QueueLock, as does a home adding a request, so
// a request added as the run ends is never lost: the home sees it ended and starts a new one.

#ifndef GAMMAOS_NANO_SCRAPE_QUEUE_H
#define GAMMAOS_NANO_SCRAPE_QUEUE_H

#include "NanoScraper.h"

#include <string>
#include <vector>

namespace android {
namespace nanoscraper {

constexpr const char* kQueueDir     = "/data/system/nano_scrape/queue.d";
constexpr const char* kQueueLock    = "/data/system/nano_scrape/queue.lock";
constexpr const char* kProgressPath = "/data/system/nano_scrape/progress";
constexpr const char* kResultsPath  = "/data/system/nano_scrape/results.jsonl";
constexpr const char* kRunProp      = "sys.gammaos.nano.scrape_run";
constexpr const char* kCancelProp   = "sys.gammaos.nano.scrape_cancel";
constexpr const char* kServiceProp  = "init.svc.gammaos-nano-scrape";

struct QueueJob {
    std::string romPath;
    std::string displayName;
    std::string sysName;
    std::string queryName;             // user title override as the search query ("" = filename)
    int engine = 0;                    // Engine
    // The system's own account, over the global one ("" = the global one). The rest of the
    // credentials the service reads itself (globalCredentials).
    std::string ssUser, ssPass, tgdbKey;
    PlatformIds plat;
    bool wantBox = true, wantFan = true;   // false: the game has art; fetch only missing extras
    std::vector<ExtraMediaJob> extras;     // type and stem
    // A copy of an extra already counts under haveDirs[i] + <subdir>/ + haveNames[k] (none: overwrite).
    std::vector<std::string> haveDirs, haveNames;
    ArtDest art;                           // where the cover and fan art go ("" = the cache folder)
};

struct Queue {
    std::string run;
    std::string cacheDir;              // where covers and fan art go
    bool wantBox = true, wantFan = true;
    std::vector<QueueJob> jobs;
};

// Holds the queue lock (flock) while it lives.
class QueueLock {
public:
    QueueLock();
    ~QueueLock();
    QueueLock(const QueueLock&) = delete;
    QueueLock& operator=(const QueueLock&) = delete;
    bool held() const { return mHeld; }
private:
    int mFd;
    bool mHeld = false;
};

// With the lock held: add a request (to run q.run), claim the run's oldest (set aside so nothing
// else takes it; "" when none is left), drop the run's, drop every other run's.
// addRequest leaves out the jobs the run already has (waiting, being scraped or done and not
// failed: the same game, search name, service, account and media), so q.jobs is what was added
// (none: no request). A request of one game always goes in: one game is asked for on purpose.
bool addRequest(Queue& q);
// A run token as the home makes it (a decimal sequence number, or "none"): it names files in
// kQueueDir, so anything else (a property any root process can set) is refused.
bool validRunToken(const std::string& run);
// A job's account (the system's own, else the shared one) and what makes two jobs the same scrape.
Credentials jobCredentials(const QueueJob& j, const Credentials& base);
size_t jobKey(const Queue& q, const QueueJob& j, const Credentials& c);
// The worker, for a job that failed: queued again, it is tried again.
void ledgerForget(const std::string& run, size_t key);
std::string claimRequest(const std::string& run);
void clearRequests(const std::string& run);
void clearOtherRequests(const std::string& keep);
// Without the lock (a large one takes a while to parse): read a claimed request. The caller
// removes it (it can hold an account).
bool readClaimed(const std::string& path, Queue* q);
// The games in the run's requests still waiting or claimed (from the file names; no lock needed).
int queuedJobCount(const std::string& run);
// The newest run with a request waiting ("" = none).
std::string newestWaitingRun();
// With the lock held: when the progress still says run runs but the service is gone, says it
// stopped, so no home reports it again.
void endDeadRun(const std::string& run);

struct Progress {
    std::string run;
    bool running = false;
    bool cancelled = false;
    int done = 0, total = 0, hits = 0, fail = 0, skip = 0;   // total counts the waiting requests too
    double bps = 0.0;
    std::string what;                  // the art downloading now ("Manual"), "" between files
    std::string status;                // "System / Game"
    std::string error;                 // English; the home translates it
};

bool writeProgress(const Progress& p);
bool readProgress(Progress* p);

// One finished game. extrasOnly: the job fetched only extra media, keeping the entry's art.
struct Result {
    std::string rom;
    std::string box, fan, title, scraper;
    std::string synopsis, genre, players, rating, releaseDate, developer, publisher;
    long long when = 0;
    bool extrasOnly = false;
    int extrasTried = 0, extrasMissing = 0;
    std::vector<std::string> extrasWritten;
    int kept = 0;          // ScrapeOutcome::kept
    bool force = false;    // the job replaced what was there (Overwrite, or one game on request)
};

std::string resultLine(const Result& r);   // no newline
bool parseResultLine(const std::string& line, Result* r);

// The credentials every system shares: the persist.gammaos.scraper.* props, with the built-in
// developer credentials when those are unset.
Credentials globalCredentials();

// True when a copy of extra medium type is already under one of the job's folders.
bool extraMediaPresent(const QueueJob& j, int type);

// tools/android-games marks its icons, which are not scraped media.
bool isAndroidGamesIcon(const std::string& path);

// 1 on a device with about 1 GB of RAM or less, else kMaxScrapeThreads.
int scrapeThreadCap();

} // namespace nanoscraper
} // namespace android

#endif // GAMMAOS_NANO_SCRAPE_QUEUE_H
