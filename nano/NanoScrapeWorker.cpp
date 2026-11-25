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

// gammaos-nano-scrape: the background scrape service (see NanoScrapeQueue.h). init starts it when a
// home sets sys.gammaos.nano.scrape_run; it scrapes the queued requests and exits. It links only the
// scraper, not the rest of Nano, so it costs little memory next to a running game.

#define LOG_TAG "GammaOSNanoScrape"

#include "NanoScrapeQueue.h"
#include "NanoScraper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>

using namespace android::nanoscraper;

namespace {

struct Run {
    std::string token;                // the run: the first request's
    Queue q;                          // the request scraping now
    std::vector<Credentials> creds;   // per job
    std::unordered_set<size_t> seen; // the jobs the run had and did not fail (scrapeKey)
    std::vector<size_t> keys;         // per job
    int before = 0;                   // the games of the earlier requests

    std::mutex m;
    Progress p;                       // under m
    bool changed = true;              // p changed since it was last written
    std::vector<std::pair<std::string, double>> slots;   // each thread's download and speed
    int resultsFd = -1;
    std::atomic<bool> cancel{false};
    std::condition_variable wake;
    bool finished = false;

    void append(const Result& r) {   // under m
        const std::string line = resultLine(r) + "\n";
        const ssize_t n = resultsFd >= 0 ? write(resultsFd, line.data(), line.size()) : -1;
        if (n != (ssize_t)line.size()) {
            ALOGW("results: write failed: %s", strerror(errno));
            if (n > 0) (void)!write(resultsFd, "\n", 1);   // end the torn line so the next one parses
        }
    }

    void setStatus(const QueueJob& j) {
        std::lock_guard<std::mutex> lk(m);
        p.status = j.sysName + " / " + j.displayName;
        changed = true;
    }

    // A game asked for the same way again (the same system queued twice) is scraped once; one
    // that failed, or is asked for another way (another service or account), is scraped again.
    size_t scrapeKey(const QueueJob& j, const Credentials& c) const { return jobKey(q, j, c); }

    // Takes the request's games (under m), without the ones the run already had, and works out
    // each game's account.
    void prepare() {
        const Credentials base = globalCredentials();
        std::vector<QueueJob> jobs;
        creds.clear();
        keys.clear();
        for (auto& j : q.jobs) {
            Credentials c = jobCredentials(j, base);   // the system's own account, else the shared one
            const size_t key = scrapeKey(j, c);
            if (q.jobs.size() > 1 && seen.count(key)) continue;   // one game is asked for on purpose
            jobs.push_back(std::move(j));
            creds.push_back(std::move(c));
            keys.push_back(key);
        }
        // Only against earlier requests: one game in two systems (one folder) is looked up for both.
        for (const auto& k : keys) seen.insert(k);
        if (jobs.size() != q.jobs.size()) ALOGI("%zu games already in this run", q.jobs.size() - jobs.size());
        q.jobs = std::move(jobs);
        mkdir(q.cacheDir.c_str(), 0700);
        updateTotal();
    }

    void updateTotal() {   // under m
        const int total = before + (int)q.jobs.size() + queuedJobCount(token);
        if (total != p.total) { p.total = total; changed = true; }
    }

    // Writes the progress when it changed, and notices a cancel and requests added, a few times a
    // second.
    void reporter() {
        std::unique_lock<std::mutex> lk(m);
        while (!finished) {
            wake.wait_for(lk, std::chrono::milliseconds(250));
            if (finished) break;
            char c[PROPERTY_VALUE_MAX] = {};
            property_get(kCancelProp, c, "");
            if (token == c) cancel = true;
            updateTotal();
            if (changed) {
                Progress copy = p;
                changed = false;
                lk.unlock();
                writeProgress(copy);
                lk.lock();
            }
        }
    }

    void go();
};

// Each game's lookup, cover and fan art first (each result written as soon as it is saved), then
// its extra media, images before manuals and videos. Games that share an account run together on
// as many threads as the account allows.
void Run::go() {
    const bool wantBox = q.wantBox, wantFan = q.wantFan;
    const std::vector<QueueJob>& jobs = q.jobs;
    std::function<bool()> cancelled = [this] { return cancel.load(); };

    // Runs fn(0..n-1) on up to `threads` threads, this one included.
    auto pool = [&](int threads, size_t n, const std::function<void(size_t)>& fn) {
        std::atomic<size_t> next{0};
        auto runSlot = [&](int slot) {
            setDownloadReporter([this, slot](const std::string& what, double bps) {
                std::lock_guard<std::mutex> lk(m);
                if ((int)slots.size() <= slot) slots.resize(slot + 1);
                auto& s = slots[slot];
                s.second = (what != s.first || bps <= 0.0 || s.second <= 0.0) ? bps : s.second * 0.5 + bps * 0.5;
                s.first = what;
                if (!what.empty()) p.what = what;
                double total = 0.0;
                bool busy = false;
                for (const auto& o : slots) { busy |= !o.first.empty(); total += o.second > 0.0 ? o.second : 0.0; }
                if (!busy) p.what.clear();
                p.bps = total;
                changed = true;
            });
            for (size_t i; !cancelled() && (i = next++) < n;) fn(i);
            setDownloadReporter(nullptr);
        };
        std::vector<std::thread> helpers;
        for (int s = 1; s < threads && (size_t)s < n; s++) helpers.emplace_back(runSlot, s);
        runSlot(0);
        for (auto& t : helpers) t.join();
    };

    struct Work {
        std::vector<ExtraMediaJob> extras;   // the job's extra media not already there
        ScrapeOutcome r;
        bool box = false, fan = false, extrasOnly = false;
        int tried = 0, missing = 0;    // extra media asked about / the service did not have
        bool published = false;        // its cover / fan art went out after the lookup
        int downloads = 0;             // extra media still to fetch (under m)
    };
    std::vector<Work> work(jobs.size());

    auto resultFor = [&](size_t i, const Work& w) {
        Result e;
        e.rom = jobs[i].romPath;
        e.box = w.r.boxFile; e.fan = w.r.fanFile; e.title = w.r.title;
        e.synopsis = w.r.synopsis; e.genre = w.r.genre; e.players = w.r.players;
        e.rating = w.r.rating; e.releaseDate = w.r.releaseDate;
        e.developer = w.r.developer; e.publisher = w.r.publisher;
        e.scraper = (jobs[i].engine == (int)ENGINE_THEGAMESDB) ? "thegamesdb" : "screenscraper";
        e.when = (long long)time(nullptr);
        e.extrasOnly = w.extrasOnly; e.extrasTried = w.tried; e.extrasMissing = w.missing;
        e.kept = w.r.kept;
        e.force = !jobs[i].art.keep;
        return e;
    };

    // Counts a game once all its work is done and writes what it got.
    auto finish = [&](size_t i) {
        Work& w = work[i];
        const ScrapeOutcome& r = w.r;
        bool extraFailed = false;   // the service had it, but the download or save failed (retried next time)
        int unusable = 0;           // what the service had was not a usable file: as good as missing
        std::vector<std::string> written;
        for (const auto& x : w.extras) {
            if (!x.written.empty()) written.push_back(x.written);
            else if (x.offered) extraFailed = true;
            else if (!x.url.empty()) unusable |= 1 << x.type;
        }
        if (w.tried) { w.tried |= unusable; w.missing |= unusable; }
        const bool anyExtra = !written.empty();
        const bool ok = r.ok || anyExtra;
        const bool stopped = cancelled();   // a lookup or download the cancel cut short is no failure
        if (stopped) extraFailed = false;
        std::lock_guard<std::mutex> lk(m);
        const int failedBefore = p.fail;
        p.done++;
        changed = true;
        if (w.published) {
            // Its cover is already in: only what the extra media added.
            if (extraFailed) p.fail++; else p.hits++;
            if (anyExtra || unusable) {
                Result e;
                e.rom = jobs[i].romPath;
                e.when = (long long)time(nullptr);
                e.extrasOnly = true; e.extrasTried = unusable; e.extrasMissing = unusable;
                e.extrasWritten = std::move(written);
                append(e);
            }
        } else if (w.extrasOnly && !r.networkFail && !stopped) {
            // A game that has its art: failed when the service did not know it or media it should
            // get did not save, a hit when new media came, else up to date.
            if (!r.matched || extraFailed) p.fail++;
            else if (anyExtra) p.hits++;
            else p.skip++;
            Result e = resultFor(i, w);
            e.extrasWritten = std::move(written);
            append(e);
        } else if (ok) {
            // The art saved; media it should have got and did not still counts as a failure.
            if (extraFailed) p.fail++; else p.hits++;
            Result e = resultFor(i, w);
            e.extrasWritten = std::move(written);
            append(e);
        } else if (!stopped) {
            p.fail++;
        }
        if (!ok && r.networkFail && !stopped && p.error.empty()) p.error = r.error;
        if (p.fail != failedBefore) {   // queued again, it is tried again
            seen.erase(keys[i]);
            ledgerForget(token, keys[i]);
        }
        // Only the counts are needed from here on: a large library keeps nothing per game.
        w.r = ScrapeOutcome();
        std::vector<ExtraMediaJob>().swap(w.extras);
    };

    // Pass 1: identify the game and fetch its cover and fan art.
    auto identify = [&](size_t i) {
        const QueueJob& j = jobs[i];
        Work& w = work[i];
        setStatus(j);
        for (const auto& x : j.extras)
            if (!extraMediaPresent(j, x.type)) w.extras.push_back(x);
        w.box = wantBox && j.wantBox; w.fan = wantFan && j.wantFan;
        w.extrasOnly = !w.box && !w.fan && !j.extras.empty();
        if (w.extrasOnly && w.extras.empty()) {   // has its art and every extra already
            std::lock_guard<std::mutex> lk(m);
            p.done++; p.skip++;
            changed = true;
            return;
        }
        w.r = scrapeRom((Engine)j.engine, creds[i], j.romPath, j.displayName, j.plat, w.box, w.fan,
                        q.cacheDir, std::to_string(i), j.queryName, &w.extras, cancelled, j.art);
        // What the service did not have, to not ask again for a while (a quota or server error
        // page tells nothing, nor does a cancelled lookup).
        if ((w.r.matched || w.r.notFound) && !w.r.networkFail && !cancelled())
            for (const auto& x : w.extras) {
                w.tried |= 1 << x.type;
                if (!x.offered) w.missing |= 1 << x.type;
            }
        for (const auto& x : w.extras) w.downloads += x.offered ? 1 : 0;
        if (w.downloads == 0) { finish(i); return; }
        if (w.r.ok) {   // show the cover now; the extra media follow
            std::lock_guard<std::mutex> lk(m);
            append(resultFor(i, w));
            w.published = true;
        }
    };

    // Groups of jobs sharing an account, in the order they were listed.
    std::vector<std::vector<size_t>> groups;
    {
        std::vector<std::string> keys;
        for (size_t i = 0; i < jobs.size(); i++) {
            const Credentials& c = creds[i];
            const std::string key = std::to_string(jobs[i].engine) + '\x1f' + c.ssDevId + '\x1f' +
                                    c.ssUser + '\x1f' + c.ssPass + '\x1f' + c.tgdbKey;
            const size_t g = std::find(keys.begin(), keys.end(), key) - keys.begin();
            if (g == keys.size()) { keys.push_back(key); groups.emplace_back(); }
            groups[g].push_back(i);
        }
    }
    const int cap = scrapeThreadCap();
    for (const auto& group : groups) {
        if (cancelled()) break;
        // ScreenScraper limits how many requests an account makes at once; TheGamesDB states no
        // limit, so it gets one thread, as does an account whose limit cannot be read. A device
        // with about 1 GB of RAM always uses one.
        const size_t first = group[0];
        const int threads = cap > 1 && jobs[first].engine == (int)ENGINE_SCREENSCRAPER
                ? std::min(screenScraperMaxThreads(creds[first], q.cacheDir, cancelled), cap) : 1;
        ALOGI("%zu games on %d thread(s)", group.size(), threads);
        pool(threads, group.size(), [&](size_t k) { identify(group[k]); });

        // Pass 2: each extra medium separately, the lighter kinds first.
        std::vector<std::pair<size_t, size_t>> items;   // (job, extra)
        for (size_t i : group)
            if (work[i].downloads > 0)
                for (size_t x = 0; x < work[i].extras.size(); x++)
                    if (work[i].extras[x].offered) items.emplace_back(i, x);
        std::stable_sort(items.begin(), items.end(), [&](const auto& a, const auto& b) {
            return kExtraMedia[work[a.first].extras[a.second].type].kind <
                   kExtraMedia[work[b.first].extras[b.second].type].kind;
        });
        pool(threads, items.size(), [&](size_t k) {
            const size_t i = items[k].first;
            ExtraMediaJob& x = work[i].extras[items[k].second];
            setStatus(jobs[i]);
            fetchExtraMedia(x, std::to_string(i) + "-" + std::to_string(x.type), cancelled);
            bool last;
            {
                std::lock_guard<std::mutex> lk(m);
                last = --work[i].downloads == 0;
            }
            if (last) finish(i);
        });
    }
    freeTgdbResources();
}

} // namespace

// Under the queue lock: the cancel as it stands now, not as the reporter last saw it.
static bool cancelRequested(Run& run) {
    char c[PROPERTY_VALUE_MAX] = {};
    property_get(kCancelProp, c, "");
    if (run.token == c) run.cancel = true;
    return run.cancel;
}

int main() {
    Run run;
    char token[PROPERTY_VALUE_MAX] = {};
    property_get(kRunProp, token, "");
    run.token = token[0] && validRunToken(token) ? token : "none";
    {
        // Said to run from the start, under the lock: a home starting a scrape while the first
        // request is read adds to this run rather than clearing its queue. The results go first:
        // a home reads them only once the progress names this run.
        QueueLock lock;
        run.resultsFd = open(kResultsPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0600);
        if (run.resultsFd < 0) ALOGE("cannot write %s: %s", kResultsPath, strerror(errno));
        run.p.run = run.token;
        run.p.running = true;
        run.p.total = queuedJobCount(run.token);   // from the names: the parse takes a while
        writeProgress(run.p);
    }
    for (;;) {
        std::string claimed;
        {
            QueueLock lock;
            claimed = claimRequest(run.token);
            if (claimed.empty()) {
                ALOGE("no queue to scrape");
                if (run.resultsFd >= 0) close(run.resultsFd);
                run.p.running = false;
                run.p.error = "The scrape could not be started.";
                writeProgress(run.p);
                return 1;
            }
        }
        const bool read = readClaimed(claimed, &run.q);
        unlink(claimed.c_str());   // before the total counts the waiting requests
        if (read) break;
        run.p.error = "A scrape request could not be read.";
    }
    run.prepare();
    cancelRequested(run);   // a cancel while it started
    writeProgress(run.p);

    std::thread reporter(&Run::reporter, &run);
    for (bool more = true; more;) {
        ALOGI("run %s: %zu games", run.token.c_str(), run.q.jobs.size());
        run.go();
        // The next request, or the end. The end is written before the lock is let go: a home
        // adding a request after that sees the run ended and starts a new one.
        for (;;) {
            std::string claimed;
            {
                QueueLock lock;
                // Under m too: the reporter's count never sees a request between its two names.
                std::unique_lock<std::mutex> lk(run.m);
                claimed = cancelRequested(run) ? std::string() : claimRequest(run.token);
                if (claimed.empty()) {
                    run.updateTotal();
                    run.p.cancelled = run.cancel && run.p.done < run.p.total;   // not when it was all done
                    if (run.cancel) clearRequests(run.token);
                    run.updateTotal();
                    run.finished = true;
                    run.p.running = false;
                    run.p.what.clear();
                    run.p.bps = 0.0;
                    lk.unlock();
                    run.wake.notify_all();
                    reporter.join();
                    if (run.resultsFd >= 0) close(run.resultsFd);
                    writeProgress(run.p);
                    more = false;
                    break;
                }
            }
            Queue next;
            if (!readClaimed(claimed, &next)) {
                std::lock_guard<std::mutex> lk(run.m);
                unlink(claimed.c_str());
                run.p.error = "A scrape request could not be read.";
                run.changed = true;
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(run.m);   // the total never counts it twice, or not at all
                unlink(claimed.c_str());
                run.before += (int)run.q.jobs.size();
                run.q = std::move(next);
                run.prepare();
                break;
            }
        }
    }
    ALOGI("run %s done (%d/%d hits, %d fail)", run.token.c_str(), run.p.hits, run.p.total, run.p.fail);
    return 0;
}
