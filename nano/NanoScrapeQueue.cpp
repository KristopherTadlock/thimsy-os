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

#define LOG_TAG "GammaOSNanoScrape"

#include "NanoScrapeQueue.h"
#include "NanoJson.h"
#include "NanoScraperDevCreds.h"   // compiled-in (obfuscated) ScreenScraper dev creds

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>

namespace android {
namespace nanoscraper {

// Replaces path with text through a temporary file, so a reader never sees half of it.
static bool writeFileAtomic(const char* path, const std::string& text) {
    const std::string tmp = std::string(path) + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    close(fd);
    if (!ok || rename(tmp.c_str(), path) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

static bool readFile(const char* path, std::string* out, size_t maxBytes) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size <= maxBytes;
    if (ok) {
        out->resize(st.st_size);
        ok = read(fd, &(*out)[0], st.st_size) == st.st_size;
    }
    close(fd);
    return ok;
}

static njson::Value stringArray(const std::vector<std::string>& v) {
    njson::Value a = njson::Value::makeArray();
    for (const auto& s : v) a.arr.push_back(njson::Value::makeString(s));
    return a;
}

static std::vector<std::string> readStringArray(const njson::Value& o, const char* key) {
    std::vector<std::string> v;
    const njson::Value* a = o.find(key);
    if (a && a->isArray())
        for (const auto& s : a->arr) v.push_back(s.asString());
    return v;
}

QueueLock::QueueLock() {
    mkdir(kQueueDir, 0700);
    mFd = open(kQueueLock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (mFd >= 0) {
        int r;
        while ((r = flock(mFd, LOCK_EX)) != 0 && errno == EINTR) {}
        mHeld = r == 0;
    }
    if (!mHeld) ALOGW("queue lock: %s", strerror(errno));
}

QueueLock::~QueueLock() {
    if (mFd >= 0) close(mFd);   // releases the lock
}

// A request file is "<boot time ns, 20 digits>-<run>-<games>.json"; claimed, ".taken" for ".json".
struct RequestName {
    std::string name, run;
    int games = 0;
    bool claimed = false;
};

static bool parseRequestName(const char* name, RequestName* r) {
    const size_t len = strlen(name);
    if (len > 5 && strcmp(name + len - 5, ".json") == 0) r->claimed = false;
    else if (len > 6 && strcmp(name + len - 6, ".taken") == 0) r->claimed = true;
    else return false;
    const char* runStart = strchr(name, '-');
    const char* gamesStart = runStart ? strchr(runStart + 1, '-') : nullptr;
    if (!gamesStart) return false;
    r->name = name;
    r->run.assign(runStart + 1, gamesStart);
    r->games = atoi(gamesStart + 1);
    return true;
}

// The run's requests, waiting and claimed, oldest first.
static std::vector<RequestName> requestFiles(const std::string& run) {
    std::vector<RequestName> out;
    if (DIR* d = opendir(kQueueDir)) {
        while (struct dirent* e = readdir(d)) {
            RequestName r;
            if (parseRequestName(e->d_name, &r) && r.run == run) out.push_back(std::move(r));
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const RequestName& a, const RequestName& b) { return a.name < b.name; });
    return out;
}

static std::string serializeQueue(const Queue& q);
static bool parseQueue(const std::string& text, Queue* q);

Credentials jobCredentials(const QueueJob& j, const Credentials& base) {
    Credentials c = base;
    if (!j.ssUser.empty()) c.ssUser = j.ssUser;
    if (!j.ssPass.empty()) c.ssPass = j.ssPass;
    if (!j.tgdbKey.empty()) c.tgdbKey = j.tgdbKey;
    return c;
}

size_t jobKey(const Queue& q, const QueueJob& j, const Credentials& c) {
    const size_t account = std::hash<std::string>{}(c.ssUser + '\x1f' + c.ssPass + '\x1f' + c.tgdbKey);
    std::string k = j.romPath + '\x1f' + j.queryName + '\x1f' + std::to_string(j.engine) + '\x1f' +
                    std::to_string(account) + '\x1f' +
                    char('0' + (q.wantBox && j.wantBox)) + char('0' + (q.wantFan && j.wantFan));
    for (const auto& x : j.extras) k += ',' + std::to_string(x.type);
    return std::hash<std::string>{}(k);   // a large library keeps 8 bytes a game
}

bool validRunToken(const std::string& run) {
    if (run == "none") return true;
    if (run.empty() || run.size() > 20) return false;
    for (char c : run) if (c < '0' || c > '9') return false;
    return true;
}

// "<run>.roms": the jobs a run was given ("<key>") and the ones that failed since ("-<key>"),
// one per line (keys only: no account).
static std::string ledgerPath(const std::string& run) {
    return std::string(kQueueDir) + "/" + run + ".roms";
}

static void ledgerAppend(const std::string& run, const std::string& lines) {
    if (lines.empty() || !validRunToken(run)) return;
    const int fd = open(ledgerPath(run).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (write(fd, lines.data(), lines.size()) != (ssize_t)lines.size()) ALOGW("queue: ledger write failed");
    close(fd);
}

void ledgerForget(const std::string& run, size_t key) {
    ledgerAppend(run, "-" + std::to_string(key) + "\n");
}

bool addRequest(Queue& q) {
    if (!validRunToken(q.run)) { ALOGW("queue: refusing run token '%s'", q.run.c_str()); return false; }
    std::set<size_t> have;
    {
        std::string text;
        if (readFile(ledgerPath(q.run).c_str(), &text, 64 * 1024 * 1024)) {
            for (size_t p = 0; p < text.size();) {
                size_t nl = text.find('\n', p);
                if (nl == std::string::npos) nl = text.size();
                if (nl > p) {
                    if (text[p] == '-') have.erase(strtoull(text.c_str() + p + 1, nullptr, 10));
                    else have.insert(strtoull(text.c_str() + p, nullptr, 10));
                }
                p = nl + 1;
            }
        }
    }
    const Credentials base = globalCredentials();
    std::string added;
    std::vector<QueueJob> fresh;
    for (auto& j : q.jobs) {
        const size_t key = jobKey(q, j, jobCredentials(j, base));
        // Only against what the run had before: one game in two systems (one folder) is a job for each.
        if (q.jobs.size() > 1 && have.count(key)) continue;
        added += std::to_string(key) + "\n";
        fresh.push_back(std::move(j));
    }
    q.jobs = std::move(fresh);
    if (q.jobs.empty()) return true;
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    char name[96];
    snprintf(name, sizeof(name), "%020lld-%s-%zu.json",
             (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec, q.run.c_str(), q.jobs.size());
    const std::string path = std::string(kQueueDir) + "/" + name;
    if (!writeFileAtomic(path.c_str(), serializeQueue(q))) return false;
    ledgerAppend(q.run, added);
    return true;
}

std::string claimRequest(const std::string& run) {
    for (const auto& r : requestFiles(run)) {
        if (r.claimed) continue;
        const std::string path = std::string(kQueueDir) + "/" + r.name;
        const std::string claimed = path.substr(0, path.size() - 5) + ".taken";
        if (rename(path.c_str(), claimed.c_str()) == 0) return claimed;
        unlink(path.c_str());
    }
    return std::string();
}

bool readClaimed(const std::string& path, Queue* q) {
    std::string text;
    if (readFile(path.c_str(), &text, 64 * 1024 * 1024) && parseQueue(text, q)) return true;
    ALOGW("queue: dropped unreadable request %s", path.c_str());
    return false;
}

// Removes the requests of run (keep false) or of every other run (keep true). The latter takes a
// temporary file a crash left too: it can hold an account.
static void removeRequests(const std::string& run, bool keep) {
    if (DIR* d = opendir(kQueueDir)) {
        while (struct dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            RequestName r;
            const bool ofRun = (parseRequestName(e->d_name, &r) && r.run == run) || e->d_name == run + ".roms";
            if (ofRun != keep) unlinkat(dirfd(d), e->d_name, 0);
        }
        closedir(d);
    }
}

void clearRequests(const std::string& run) {
    removeRequests(run, false);
}

void clearOtherRequests(const std::string& keep) {
    unlink("/data/system/nano_scrape/queue.json");   // the single request of earlier versions
    removeRequests(keep, true);
}

std::string newestWaitingRun() {
    std::string newest;
    if (DIR* d = opendir(kQueueDir)) {
        while (struct dirent* e = readdir(d)) {
            RequestName r;
            if (parseRequestName(e->d_name, &r) && !r.claimed &&
                strtoll(r.run.c_str(), nullptr, 10) > strtoll(newest.c_str(), nullptr, 10))
                newest = r.run;
        }
        closedir(d);
    }
    return newest;
}

void endDeadRun(const std::string& run) {
    char svc[PROPERTY_VALUE_MAX] = {};
    property_get(kServiceProp, svc, "");
    Progress p;
    if (!strcmp(svc, "running") || !readProgress(&p) || p.run != run || !p.running) return;
    p.running = false;
    p.what.clear();
    p.bps = 0.0;
    p.error = "The scrape stopped before it finished.";
    writeProgress(p);
}

int queuedJobCount(const std::string& run) {
    int n = 0;
    for (const auto& r : requestFiles(run)) n += r.games;
    return n;
}

static std::string serializeQueue(const Queue& q) {
    njson::Value root = njson::Value::makeObject();
    root.set("run") = njson::Value::makeString(q.run);
    root.set("dir") = njson::Value::makeString(q.cacheDir);
    root.set("box") = njson::Value::makeBool(q.wantBox);
    root.set("fan") = njson::Value::makeBool(q.wantFan);
    njson::Value jobs = njson::Value::makeArray();
    for (const auto& j : q.jobs) {
        njson::Value o = njson::Value::makeObject();
        o.set("rom") = njson::Value::makeString(j.romPath);
        o.set("name") = njson::Value::makeString(j.displayName);
        o.set("sys") = njson::Value::makeString(j.sysName);
        if (!j.queryName.empty()) o.set("query") = njson::Value::makeString(j.queryName);
        o.set("engine") = njson::Value::makeNumber(j.engine);
        if (!j.ssUser.empty()) o.set("ssUser") = njson::Value::makeString(j.ssUser);
        if (!j.ssPass.empty()) o.set("ssPass") = njson::Value::makeString(j.ssPass);
        if (!j.tgdbKey.empty()) o.set("tgdbKey") = njson::Value::makeString(j.tgdbKey);
        o.set("ss") = njson::Value::makeNumber(j.plat.ss);
        o.set("tgdb") = njson::Value::makeNumber(j.plat.tgdb);
        o.set("box") = njson::Value::makeBool(j.wantBox);
        o.set("fan") = njson::Value::makeBool(j.wantFan);
        njson::Value extras = njson::Value::makeArray();
        for (const auto& x : j.extras) {
            njson::Value e = njson::Value::makeObject();
            e.set("type") = njson::Value::makeNumber(x.type);
            e.set("stem") = njson::Value::makeString(x.stem);
            extras.arr.push_back(std::move(e));
        }
        o.set("extras") = std::move(extras);
        o.set("haveDirs") = stringArray(j.haveDirs);
        o.set("haveNames") = stringArray(j.haveNames);
        if (!j.art.box.empty()) o.set("boxDest") = njson::Value::makeString(j.art.box);
        if (!j.art.fan.empty()) o.set("fanDest") = njson::Value::makeString(j.art.fan);
        if (j.art.keep) o.set("artKeep") = njson::Value::makeBool(true);
        jobs.arr.push_back(std::move(o));
    }
    root.set("jobs") = std::move(jobs);
    return njson::serialize(root, false);
}

static bool parseQueue(const std::string& text, Queue* q) {
    njson::Value root;
    if (!njson::parse(text, &root) || !root.isObject()) return false;
    q->run = root.getString("run");
    q->cacheDir = root.getString("dir");
    q->wantBox = root.getBool("box", true);
    q->wantFan = root.getBool("fan", true);
    q->jobs.clear();
    const njson::Value* jobs = root.find("jobs");
    if (!jobs || !jobs->isArray()) return false;
    for (const auto& o : jobs->arr) {
        if (!o.isObject()) continue;
        QueueJob j;
        j.romPath = o.getString("rom");
        if (j.romPath.empty()) continue;
        j.displayName = o.getString("name");
        j.sysName = o.getString("sys");
        j.queryName = o.getString("query");
        j.engine = o.getInt("engine");
        j.ssUser = o.getString("ssUser");
        j.ssPass = o.getString("ssPass");
        j.tgdbKey = o.getString("tgdbKey");
        j.plat.ss = o.getInt("ss", -1);
        j.plat.tgdb = o.getInt("tgdb", -1);
        j.wantBox = o.getBool("box", true);
        j.wantFan = o.getBool("fan", true);
        if (const njson::Value* extras = o.find("extras"); extras && extras->isArray()) {
            for (const auto& e : extras->arr) {
                const int type = e.getInt("type", -1);
                if (type < 0 || type >= kExtraMediaCount) continue;
                ExtraMediaJob x;
                x.type = type;
                x.stem = e.getString("stem");
                j.extras.push_back(std::move(x));
            }
        }
        j.haveDirs = readStringArray(o, "haveDirs");
        j.haveNames = readStringArray(o, "haveNames");
        j.art.box = o.getString("boxDest");
        j.art.fan = o.getString("fanDest");
        j.art.keep = o.getBool("artKeep", false);
        q->jobs.push_back(std::move(j));
    }
    return !q->run.empty();
}

// One line per value: a name, '=', the value with its newlines flattened.
static void putLine(std::string& out, const char* key, const std::string& value) {
    out += key;
    out += '=';
    for (char c : value) out += (c == '\n' || c == '\r') ? ' ' : c;
    out += '\n';
}

bool writeProgress(const Progress& p) {
    std::string out;
    char num[64];
    putLine(out, "run", p.run);
    putLine(out, "running", p.running ? "1" : "0");
    putLine(out, "cancelled", p.cancelled ? "1" : "0");
    snprintf(num, sizeof(num), "%d %d %d %d %d", p.done, p.total, p.hits, p.fail, p.skip);
    putLine(out, "counts", num);
    snprintf(num, sizeof(num), "%.0f", p.bps);
    putLine(out, "bps", num);
    putLine(out, "what", p.what);
    putLine(out, "status", p.status);
    putLine(out, "error", p.error);
    return writeFileAtomic(kProgressPath, out);
}

bool readProgress(Progress* p) {
    std::string text;
    if (!readFile(kProgressPath, &text, 64 * 1024)) return false;
    *p = Progress();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "run") p->run = value;
        else if (key == "running") p->running = value == "1";
        else if (key == "cancelled") p->cancelled = value == "1";
        else if (key == "counts")
            sscanf(value.c_str(), "%d %d %d %d %d", &p->done, &p->total, &p->hits, &p->fail, &p->skip);
        else if (key == "bps") p->bps = strtod(value.c_str(), nullptr);
        else if (key == "what") p->what = value;
        else if (key == "status") p->status = value;
        else if (key == "error") p->error = value;
    }
    return !p->run.empty();
}

std::string resultLine(const Result& r) {
    njson::Value o = njson::Value::makeObject();
    o.set("rom") = njson::Value::makeString(r.rom);
    auto put = [&](const char* key, const std::string& v) {
        if (!v.empty()) o.set(key) = njson::Value::makeString(v);
    };
    put("box", r.box);
    put("fan", r.fan);
    put("title", r.title);
    put("scraper", r.scraper);
    put("desc", r.synopsis);
    put("genre", r.genre);
    put("players", r.players);
    put("rating", r.rating);
    put("date", r.releaseDate);
    put("dev", r.developer);
    put("pub", r.publisher);
    o.set("when") = njson::Value::makeNumber((double)r.when);
    if (r.extrasOnly) o.set("xonly") = njson::Value::makeBool(true);
    if (r.extrasTried) o.set("xtried") = njson::Value::makeNumber(r.extrasTried);
    if (r.kept) o.set("kept") = njson::Value::makeNumber(r.kept);
    if (r.force) o.set("force") = njson::Value::makeBool(true);
    if (r.extrasMissing) o.set("xmiss") = njson::Value::makeNumber(r.extrasMissing);
    if (!r.extrasWritten.empty()) o.set("xwritten") = stringArray(r.extrasWritten);
    return njson::serialize(o, false);
}

bool parseResultLine(const std::string& line, Result* r) {
    njson::Value o;
    if (!njson::parse(line, &o) || !o.isObject()) return false;
    *r = Result();
    r->rom = o.getString("rom");
    r->box = o.getString("box");
    r->fan = o.getString("fan");
    r->title = o.getString("title");
    r->scraper = o.getString("scraper");
    r->synopsis = o.getString("desc");
    r->genre = o.getString("genre");
    r->players = o.getString("players");
    r->rating = o.getString("rating");
    r->releaseDate = o.getString("date");
    r->developer = o.getString("dev");
    r->publisher = o.getString("pub");
    r->when = (long long)o.getInt("when");
    r->extrasOnly = o.getBool("xonly");
    r->extrasTried = o.getInt("xtried");
    r->kept = o.getInt("kept");
    r->force = o.getBool("force");
    r->extrasMissing = o.getInt("xmiss");
    r->extrasWritten = readStringArray(o, "xwritten");
    return !r->rom.empty();
}

#ifdef NANO_SS_HAVE_DEV_CREDS
// Reassemble an XOR-obfuscated, two-chunk credential from NanoScraperDevCreds.gen.h.
static std::string nanoSsDecode(const unsigned char* h1, size_t n1,
                                const unsigned char* h2, size_t n2) {
    std::string s; s.reserve(n1 + n2);
    size_t i = 0;
    for (size_t j = 0; j < n1; j++, i++) s.push_back((char)(h1[j] ^ kNanoSsKey[i % sizeof(kNanoSsKey)]));
    for (size_t j = 0; j < n2; j++, i++) s.push_back((char)(h2[j] ^ kNanoSsKey[i % sizeof(kNanoSsKey)]));
    return s;
}
#endif

Credentials globalCredentials() {
    Credentials c;
    char buf[PROPERTY_VALUE_MAX];
    property_get("persist.gammaos.scraper.ssdevid", buf, ""); c.ssDevId = buf;
    property_get("persist.gammaos.scraper.ssdevpw", buf, ""); c.ssDevPw = buf;
    property_get("persist.gammaos.scraper.softname", buf, NANO_SS_SOFTNAME); c.ssSoftname = buf;
    property_get("persist.gammaos.scraper.ssuser",  buf, ""); c.ssUser  = buf;
    property_get("persist.gammaos.scraper.sspass",  buf, ""); c.ssPass  = buf;
    property_get("persist.gammaos.scraper.tgdbkey", buf, ""); c.tgdbKey = buf;
    property_get("persist.gammaos.scraper.region",  buf, "us"); c.region = buf;
#ifdef NANO_SS_HAVE_DEV_CREDS
    // Fall back to the built-in developer credentials when the props are unset, so
    // scraping works out of the box without exposing the raw secret (see
    // NanoScraperDevCreds.h). A user-set prop still overrides.
    if (c.ssDevId.empty()) c.ssDevId = nanoSsDecode(kNanoSsID1, sizeof(kNanoSsID1), kNanoSsID2, sizeof(kNanoSsID2));
    if (c.ssDevPw.empty()) c.ssDevPw = nanoSsDecode(kNanoSsPW1, sizeof(kNanoSsPW1), kNanoSsPW2, sizeof(kNanoSsPW2));
#endif
    return c;
}

bool extraMediaPresent(const QueueJob& j, int type) {
    static const char* const kImg[] = {".png", ".jpg", ".jpeg", ".webp", nullptr};
    static const char* const kPdf[] = {".pdf", ".cbz", ".cbr", ".cb7", nullptr};
    static const char* const kVid[] = {".mp4", ".mkv", ".webm", ".avi", nullptr};
    const ExtraMediaType& mt = kExtraMedia[type];
    const char* const* exts = mt.kind == EXTRA_PDF ? kPdf : mt.kind == EXTRA_VIDEO ? kVid : kImg;
    for (const auto& d : j.haveDirs)
        for (const auto& n : j.haveNames)
            for (const char* const* e = exts; *e; e++) {
                const std::string p = d + mt.subdir + "/" + n + *e;
                if (access(p.c_str(), F_OK) == 0 && !isAndroidGamesIcon(p)) return true;
            }
    return false;
}

// The mark is a tEXt chunk ("Software" = its package) right after the PNG signature and IHDR
// (8 + 25 bytes).
bool isAndroidGamesIcon(const std::string& path) {
    static const char kMark[] = "tEXtSoftware\0org.thimsy.androidgames";
    const size_t at = 8 + 25 + 4, len = sizeof(kMark) - 1;
    if (path.size() < 4 || strcasecmp(path.c_str() + path.size() - 4, ".png") != 0) return false;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char head[8 + 25 + 4 + sizeof(kMark)];
    const ssize_t n = pread(fd, head, at + len, 0);
    close(fd);
    return n == (ssize_t)(at + len) && memcmp(head + at, kMark, len) == 0;
}

int scrapeThreadCap() {
    FILE* f = fopen("/proc/meminfo", "r");
    long kb = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
        fclose(f);
    }
    // The same line Nano draws for its 1 GB defaults (a 1 GB panel reports about 0.95-1.0 GB).
    return kb > 0 && kb <= 1300000 ? 1 : kMaxScrapeThreads;
}

} // namespace nanoscraper
} // namespace android
