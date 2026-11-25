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

// The BIOS installer's worker: every file of a chosen folder whose size is a catalogue entry's is
// hashed, recognised by its SHA-1 (whatever its name), and copied where each emulator reads it,
// under the name it needs (see NanoBiosInstall.cpp). Nothing is overwritten.

#define LOG_TAG "NanoMenu"

#include "NanoBiosWorker.h"
#include "NanoAppFiles.h"
#include "NanoI18n.h"
#include "NanoJson.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <log/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace android {
namespace nanobios {

namespace {

const char kManifestDir[] = "/data/system/nano_bios";
const char kRaDefSystem[] = "/storage/emulated/0/RetroArch/system";
const char kArmsx2Bios[] = "/data/media/0/Android/data/com.armsx2/files/bios";
const char kDolphinGc[] = "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files/GC";
const char kDrasticNanoSys[] = "/data/system/drastic-nano/DraStic/system";
const char kMupenFiles[] = "/data/media/0/Android/data/org.mupen64plusae.v3.fzurita/files";
const char kScummvmExtra[] = "/data/data/org.scummvm.scummvm/files/extra";
const char kHakuxConsole[] = "/data/data/com.rfandango.haku_x/files/console";
// The core reads the BIOS and firmware from <its data folder>/system, the data folder being
// files/SeedlessDS (EmuAssets.DATA_DIR, PathCache.systemDir).
const char kSeedlessSys[] = "/data/data/com.seedlessds.app/files/SeedlessDS/system";
const char kMame4droidRoms[] = "/data/media/0/Android/data/com.seleuco.mame4d2024/files/roms";
const char kVita3kPkg[] = "org.vita3k.emulator";
const char kVita3kAppPrefs[] = "/data/data/org.vita3k.emulator/shared_prefs/vita3k_app.xml";
const char kVita3kStage[] = "/data/media/0/Android/data/org.vita3k.emulator/files/nano-install";
const uint64_t kMaxFile = 1ull << 30;        // larger files are never looked at
const uint64_t kSpare = 8ull << 20;          // left free on a drive after copying
const int kUnknownNames = 5;                 // unrecognised files named in the report

// An English format string, translated, filled in.
std::string tfmt(const char* english, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, english);
    vsnprintf(buf, sizeof(buf), trDyn(english), ap);
    va_end(ap);
    return buf;
}

std::string humanSize(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else snprintf(buf, sizeof(buf), "%llu KB", (unsigned long long)((b + 1023) / 1024));
    return buf;
}

uint64_t fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0; }

int64_t monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct Drive { std::string name; uint64_t bsize = 4096, avail = 0, need = 0; bool full = false; };

// What a folder holds (files, and bytes), links not followed: a measure of an extraction's progress.
uint64_t treeMeasure(const std::string& dir, int depth = 0) {
    if (depth > 16) return 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    uint64_t total = 0;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (lstat(p.c_str(), &st) != 0) continue;
        total += 1 + (uint64_t)st.st_size;
        if (S_ISDIR(st.st_mode)) total += treeMeasure(p, depth + 1);
    }
    closedir(d);
    return total;
}

// Lines of the log since an epoch second that hold any of the needles: the index of the first found, or -1.
// pid: only that process's lines ("" = all).
int logcatFind(const RunFn& run, time_t since, const std::vector<const char*>& needles, const std::string& pid) {
    std::string out;
    std::vector<std::string> args = {"/system/bin/logcat", "-d", "-T", std::to_string((long long)since) + ".000"};
    if (!pid.empty()) args.push_back("--pid=" + pid);
    run(args, &out, 10000);
    for (size_t pos = 0; pos < out.size();) {
        size_t end = out.find('\n', pos);
        if (end == std::string::npos) end = out.size();
        const std::string line = out.substr(pos, end - pos);
        for (size_t i = 0; i < needles.size(); i++) if (line.find(needles[i]) != std::string::npos) return (int)i;
        pos = end + 1;
    }
    return -1;
}


// A recognised file (or set).
struct BiosMatch {
    int file = -1;           // nanobios::files() index
    std::string src;         // the file (a folder for a folder set)
    std::string sha1;        // of src ("" for a folder set or a PUP)
    uint64_t size = 0;
    std::string name;        // the target name where a slot has none
    std::string label;       // for the report
    int region = -1;         // PS2: its region (REGION_*), for ARMSX2's selection
    bool font = false;       // PUP: the font package
    std::vector<std::string> targets;   // where it is now (installed or already there)
    int installed = 0, already = 0, conflicts = 0, failed = 0;
};

struct BiosOp {
    size_t match;
    int kind, flags;
    std::string src, dst;
    std::string rel;         // dst under the slot's folder
    uint64_t size = 0;
    dev_t dev = 0;
    bool inSet = false;      // a file of a folder set (a different one there is kept quietly)
    int state = 0;           // 0 to copy, 1 copied, 2 already there, 3 conflict, 4 no room, 5 failed, 6 kept (set)
};

}  // namespace

bool runInstall(const Env& env, InstallShared& shared, const std::string& folder, bool fromManifest,
                const std::string& raConfig, std::string* nextRaConfig) {
    InstallShared* sh = &shared;
    const std::string& R = env.root;
    auto P = [&](const char* fixed) { return R + fixed; };
    // A path from an app's settings (spelt as the app writes it): its raw folder, under root.
    auto at = [&](const std::string& p) -> std::string {
        if (p.empty()) return p;
        const std::string r = rawView(p);
        return R.empty() || r == R || under(r, R) ? r : R + r;
    };
    // A path as the app reads it in its settings.
    auto shown = [&](const std::string& p) { return appView(!R.empty() && under(p, R) ? p.substr(R.size()) : p); };
    auto run = [&](const std::vector<std::string>& args, std::string* out, int timeoutMs) {
        return env.run ? env.run(args, out, timeoutMs) : -1;
    };
    auto forceStop = [&](const char* pkg) { run({"/system/bin/am", "force-stop", pkg}, nullptr, 15000); };
    auto freeSpace = [&](const std::string& path, uint64_t* bsize, uint64_t* avail) {
        if (env.freeSpace) return env.freeSpace(path, bsize, avail);
        struct statvfs vs;
        if (statvfs(path.c_str(), &vs) != 0) return false;
        *bsize = vs.f_bsize ? vs.f_bsize : 4096;
        *avail = (uint64_t)vs.f_bavail * vs.f_frsize;
        return true;
    };
    // A drive by the device of a path's deepest existing folder.
    auto driveOf = [&](const std::string& path, std::map<dev_t, Drive>* drives) -> dev_t {
        std::string d = path;
        while (d.size() > 1 && !pathExists(d)) d = dirName(d);
        struct stat st;
        if (stat(d.c_str(), &st) != 0) return 0;
        auto it = drives->find(st.st_dev);
        if (it == drives->end()) {
            Drive dr;
            freeSpace(d, &dr.bsize, &dr.avail);
            dr.name = under(canonical(d), R + "/mnt/media_rw") ? trDyn("microSD card") : trDyn("Internal storage");
            drives->emplace(st.st_dev, dr);
        }
        return st.st_dev;
    };
    const std::string manifestPath = P(kManifestPath);
    auto cancelled = [&]() { return sh->cancel.load(); };
    const auto& cat = files();
    const Region region = env.region;
    Packages pk;
    pk.load(P("/data/system/packages.list"));
    std::vector<BiosMatch> matches;
    std::vector<std::string> lines, unknownNames;
    int unknown = 0;
    bool noneFound = false;

    auto progress = [&](const std::function<void(InstallProgress&)>& fn) {
        if (fromManifest) return;   // a reinstall has no screen
        { std::lock_guard<std::mutex> lk(sh->mu); fn(sh->progress); }
        if (env.wakeThrottled) env.wakeThrottled();
    };
    auto rowSystem = [&](int fi) { const Row* r = findRow(cat[fi].row); return std::string(trDyn(r ? r->system : "")); };

    // ---- 1. what there is ----
    njson::Value manifest;
    {
        // Any failure but a missing file: never built on, set aside, started afresh.
        bool bad = false;
        std::string text;
        const int fd = openRegular(manifestPath, true);
        if (fd < 0) {
            bad = access(manifestPath.c_str(), F_OK) == 0 || errno != ENOENT;
        } else {
            char buf[8192];
            for (;;) {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 || text.size() > (8u << 20)) { bad = true; break; }
                if (n == 0) break;
                text.append(buf, (size_t)n);
            }
            close(fd);
            if (!bad && (!njson::parse(text, &manifest) || !manifest.isObject())) bad = true;
        }
        if (bad) {
            ALOGW("bios: %s cannot be read; kept as installed.json.bad", manifestPath.c_str());
            rename(manifestPath.c_str(), (manifestPath + ".bad").c_str());
            manifest = njson::Value();
        }
    }

    if (!fromManifest) {
        char rbuf[PATH_MAX];
        const std::string root = realpath(rawView(folder).c_str(), rbuf) ? std::string(rbuf) : std::string();
        std::set<uint64_t> sizes;
        std::set<std::string> hashNames;   // hashed whatever their size
        for (const auto& f : cat) {
            if ((f.match == MATCH_SHA1 || f.match == MATCH_MD5 || f.match == MATCH_FOLDERSET) && f.size) sizes.insert(f.size);
            if (f.match == MATCH_FOLDERSET || (f.match == MATCH_SHA1 && !f.size)) hashNames.insert(lowerStr(baseName(f.name)));
        }
        struct Found { std::string path; uint64_t size; };
        std::vector<Found> all;
        std::set<std::pair<dev_t, ino_t>> seen;
        std::function<void(const std::string&, int)> walk = [&](const std::string& dir, int depth) {
            if (depth > 24 || cancelled()) return;
            DIR* d = opendir(dir.c_str());
            if (!d) return;
            std::vector<std::string> names;
            while (dirent* e = readdir(d)) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
            closedir(d);
            std::sort(names.begin(), names.end());
            for (const auto& n : names) {
                std::string p = dir + "/" + n;
                struct stat st;
                if (lstat(p.c_str(), &st) != 0) continue;
                if (S_ISLNK(st.st_mode)) {   // followed only to somewhere inside the chosen folder, then used as that
                    char t[PATH_MAX];
                    if (!realpath(p.c_str(), t) || !under(t, root) || inSystemFs(t) || lstat(t, &st) != 0) continue;
                    p = t;
                }
                if (S_ISDIR(st.st_mode)) {
                    if (seen.insert({st.st_dev, st.st_ino}).second) walk(p, depth + 1);
                } else if (S_ISREG(st.st_mode) && (uint64_t)st.st_size <= kMaxFile) {
                    all.push_back({p, (uint64_t)st.st_size});
                }
            }
        };
        struct stat rst;
        if (!root.empty() && !inSystemFs(root) && root != "/" && stat(root.c_str(), &rst) == 0) {
            seen.insert({rst.st_dev, rst.st_ino});
            walk(root, 0);
        }
        progress([&](InstallProgress& p) { p.total = (int)all.size(); });
        ALOGI("bios: %zu files under %s", all.size(), root.c_str());

        std::map<std::string, int> bySha, byMd5;
        for (size_t i = 0; i < cat.size(); i++) {
            if (*cat[i].sha1 && cat[i].match != MATCH_NAME) bySha.emplace(cat[i].sha1, (int)i);
            if (cat[i].match == MATCH_MD5) byMd5.emplace(cat[i].md5, (int)i);
        }
        int romdirIdx = -1, pupMain = -1, pupFont = -1;
        for (size_t i = 0; i < cat.size(); i++) {
            if (cat[i].match == MATCH_PS2ROMDIR) romdirIdx = (int)i;
            if (cat[i].match == MATCH_PUP) (strcmp(cat[i].desc, "Font package") ? pupMain : pupFont) = (int)i;
        }
        for (size_t k = 0; k < all.size() && !cancelled(); k++) {
            const Found& f = all[k];
            const std::string name = baseName(f.path), lname = lowerStr(name);
            progress([&](InstallProgress& p) { p.scanned = (int)k + 1; p.current = name; });
            BiosMatch m;
            m.src = f.path;
            m.size = f.size;
            bool hashed = false;
            if (sizes.count(f.size) || hashNames.count(lname)) {
                std::string md5;
                if (hashFile(f.path, &m.sha1, &md5, true)) {
                    hashed = true;
                    auto it = bySha.find(m.sha1);
                    if (it != bySha.end()) m.file = it->second;
                    else if ((it = byMd5.find(md5)) != byMd5.end()) m.file = it->second;
                }
            }
            if (m.file < 0 && lname.size() > 4 && lname.compare(lname.size() - 4, 4, ".zip") == 0) {
                std::vector<std::string> members;
                if (const ZipSet* zs = zipMatchSet(f.path, &members)) {
                    for (size_t i = 0; i < cat.size(); i++)
                        if (cat[i].match == MATCH_ZIPSET && zs->name == std::string(cat[i].name)) m.file = (int)i;
                    if (m.file >= 0) hashFile(f.path, &m.sha1, nullptr, true);
                }
            }
            if (m.file < 0 && f.size == 4194304 && romdirIdx >= 0) {
                const std::string rv = ps2Romver(f.path);
                if (!rv.empty()) {
                    m.file = romdirIdx;
                    m.name = name;
                    m.label = ps2RomverLabel(rv, &m.region);
                    if (m.sha1.empty()) hashFile(f.path, &m.sha1, nullptr, true);
                }
            }
            if (m.file < 0 && f.size >= (1u << 20) && pupMain >= 0) {
                uint32_t fw = 0;
                if (isVitaPup(f.path, &fw)) {
                    // Sony's font package is far smaller than the system software.
                    m.font = f.size < (64u << 20);
                    m.file = m.font ? pupFont : pupMain;
                    char v[32];
                    snprintf(v, sizeof(v), "%x.%02x", (fw >> 24) & 0xff, (fw >> 16) & 0xff);
                    m.label = fw ? std::string(v) : std::string();
                }
            }
            if (m.file < 0) {
                for (size_t i = 0; i < cat.size(); i++) {
                    const File& c = cat[i];
                    if (!nameMatches(c, name, f.size)) continue;
                    if (!strcmp(c.row, "ndsfw")) {   // a DS firmware: "MAC" at 0x08
                        char mac[3] = {};
                        const int fd = openRegular(f.path, true);   // never a FIFO's open that blocks
                        const bool ok = fd >= 0 && pread(fd, mac, 3, 8) == 3 && !memcmp(mac, "MAC", 3);
                        if (fd >= 0) close(fd);
                        if (!ok) continue;
                    }
                    m.file = (int)i;
                    break;
                }
            }
            if (m.file < 0) {
                if (hashed) {
                    unknown++;
                    if ((int)unknownNames.size() < kUnknownNames) unknownNames.push_back(name);
                }
                continue;
            }
            if (m.name.empty()) m.name = cat[m.file].name;
            if (m.sha1.empty() && cat[m.file].match != MATCH_PUP) hashFile(f.path, &m.sha1, nullptr, true);
            matches.push_back(std::move(m));
        }

        // A folder set: both sentinels under one folder, at their places in it. Of several such
        // folders the first found is used.
        {
            std::vector<std::pair<size_t, std::string>> db, mach;   // match, its root
            for (size_t i = 0; i < matches.size(); i++) {
                const File& c = cat[matches[i].file];
                if (c.match != MATCH_FOLDERSET) continue;
                const std::string want = "/" + lowerStr(c.name);
                const std::string lp = lowerStr(matches[i].src);
                std::string r;
                if (lp.size() > want.size() && lp.compare(lp.size() - want.size(), want.size(), want) == 0)
                    r = matches[i].src.substr(0, matches[i].src.size() - want.size());
                (strstr(c.name, "Databases") ? db : mach).push_back({i, r});
            }
            std::string root;
            size_t dbi = 0, mi = 0;
            for (const auto& d : db) {
                for (const auto& m : mach)
                    if (!d.second.empty() && d.second == m.second) { root = d.second; dbi = d.first; mi = m.first; break; }
                if (!root.empty()) break;
            }
            std::vector<size_t> drop;
            for (const auto& v : {db, mach})
                for (const auto& x : v) if (root.empty() || (x.first != dbi && x.first != mi)) drop.push_back(x.first);
            if (!root.empty() && (db.size() > 1 || mach.size() > 1)) lines.push_back(trDyn("MSX: another copy of the blueMSX folders was ignored."));
            for (size_t k : {dbi, mi}) {
                if (root.empty()) break;
                BiosMatch& m = matches[k];
                const std::string sub = strstr(cat[m.file].name, "Databases") ? "Databases" : "Machines";
                std::string dir;
                if (DIR* d = opendir(root.c_str())) {
                    while (dirent* e = readdir(d)) {
                        struct stat lst;
                        const std::string cand = root + "/" + e->d_name;
                        if (!strcasecmp(e->d_name, sub.c_str()) && lstat(cand.c_str(), &lst) == 0 && S_ISDIR(lst.st_mode)) dir = cand;
                    }
                    closedir(d);
                }
                if (dir.empty()) { drop.push_back(k); continue; }
                m.src = dir;
                m.sha1.clear();
                m.name = sub;
            }
            std::sort(drop.begin(), drop.end());
            drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
            for (auto it = drop.rbegin(); it != drop.rend(); ++it) matches.erase(matches.begin() + (long)*it);
        }

        // A PS2 BIOS's companions (.nvm and the extra ROMs dumped with it) beside it.
        int compIdx = -1;
        for (size_t i = 0; i < cat.size(); i++) if (cat[i].match == MATCH_COMPANION) compIdx = (int)i;
        const size_t n = matches.size();
        for (size_t i = 0; i < n && compIdx >= 0; i++) {
            const File& c = cat[matches[i].file];
            if (c.row != std::string("ps2")) continue;
            const std::string srcDir = dirName(matches[i].src), srcBase = baseName(matches[i].src);
            const std::string stem = lowerStr(srcBase.substr(0, srcBase.find_last_of('.')));
            const std::string tgtStem = matches[i].name.substr(0, matches[i].name.find_last_of('.'));
            DIR* d = opendir(srcDir.c_str());
            if (!d) continue;
            while (dirent* e = readdir(d)) {
                const std::string en = e->d_name, le = lowerStr(en);
                for (const char* ext : {".nvm", ".mec", ".rom1", ".rom2", ".erom"}) {
                    if (le != stem + ext) continue;
                    BiosMatch cm;
                    cm.file = compIdx;
                    cm.src = srcDir + "/" + en;
                    cm.size = fileSize(cm.src);
                    cm.name = tgtStem + en.substr(en.find_last_of('.'));
                    struct stat cst;
                    if (lstat(cm.src.c_str(), &cst) != 0 || !S_ISREG(cst.st_mode) || (uint64_t)cst.st_size > kMaxFile) continue;
                    hashFile(cm.src, &cm.sha1, nullptr, true);
                    matches.push_back(cm);
                }
            }
            closedir(d);
        }
        noneFound = matches.empty() && !cancelled();
    } else {
        // Reinstall: the manifest's entries, each from a copy that is still there.
        const njson::Value* list = manifest.find("files");
        for (size_t k = 0; list && list->isArray() && k < list->arr.size(); k++) {
            const njson::Value& e = list->arr[k];
            const std::string row = e.getString("row"), cname = e.getString("file"), sha = e.getString("sha1");
            BiosMatch m;
            for (size_t i = 0; i < cat.size() && m.file < 0; i++)
                if (row == cat[i].row && cname == cat[i].name && (!*cat[i].sha1 || cat[i].match == MATCH_FOLDERSET || sha == cat[i].sha1))
                    m.file = (int)i;
            if (m.file < 0 || cat[m.file].match == MATCH_PUP) continue;   // Vita3K only from the dialog
            if (sha.empty() && cat[m.file].match != MATCH_FOLDERSET) continue;
            m.name = e.getString("name");
            m.sha1 = sha;
            m.font = e.getBool("font");
            m.region = e.getInt("region", -1);
            std::vector<std::string> cands;
            if (const njson::Value* t = e.find("targets"))
                for (const auto& v : t->arr) { cands.push_back(v.asString()); m.targets.push_back(v.asString()); }
            cands.push_back(e.getString("source"));
            for (const auto& c : cands) {
                if (c.empty()) continue;
                if (cat[m.file].match == MATCH_FOLDERSET) {   // still holding its sentinel
                    const bool dbs = m.name == "Databases";
                    std::string have;
                    struct stat lst;
                    hashFile(c + (dbs ? "/msxromdb.xml" : "/Shared Roms/MSX.rom"), &have, nullptr, true);
                    if (lstat(c.c_str(), &lst) == 0 && S_ISDIR(lst.st_mode) && have == cat[m.file].sha1) { m.src = c; break; }
                    continue;
                }
                struct stat st;
                if (lstat(c.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
                std::string s;
                if (!hashFile(c, &s, nullptr, true) || s != sha) continue;
                m.src = c;
                m.size = (uint64_t)st.st_size;
                break;
            }
            if (!m.src.empty()) matches.push_back(std::move(m));
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const BiosMatch& a, const BiosMatch& b) { return a.file < b.file; });

    // ---- 2. where each goes ----
    std::string raSys, scummExtra;
    bool raSysDone = false, scummSetPath = false;
    std::map<int, std::string> baseCache;
    auto base = [&](int kind) -> std::string {
        auto it = baseCache.find(kind);
        if (it != baseCache.end()) return it->second;
        std::string b;
        switch (kind) {
            case SLOT_RA: case SLOT_RA_FOLDER: {
                if (!raSysDone) {
                    raSysDone = true;
                    std::string text, v;
                    appFileRead(at(raConfig), &text, R);
                    for (size_t pos = 0; pos < text.size();) {
                        size_t end = text.find('\n', pos);
                        if (end == std::string::npos) end = text.size();
                        const std::string line = text.substr(pos, end - pos);
                        pos = end + 1;
                        const size_t eq = line.find('=');
                        if (eq == std::string::npos || sdTrimDir(line.substr(0, eq)) != "system_directory") continue;
                        v = sdTrimDir(line.substr(eq + 1));
                    }
                    if (v.empty() || v == "default" || v[0] != '/') v = kRaDefSystem;
                    raSys = at(v);
                }
                b = raSys;
                break;
            }
            case SLOT_ARMSX2: b = P(kArmsx2Bios); break;
            case SLOT_DOLPHIN_GC: b = P(kDolphinGc); break;
            case SLOT_FLYCAST: { std::string prefs; const std::string h = flycastHomeAt(R, &prefs); if (!h.empty()) b = at(h) + "/data"; break; }
            case SLOT_DRASTIC: {
                std::string d = sdTrimDir(drasticSystemDirAt(R));
                if (!d.empty()) b = at(d) + "/system";
                break;
            }
            case SLOT_DRASTIC_NANO: b = P(kDrasticNanoSys); break;
            case SLOT_MUPEN: b = P(kMupenFiles); break;
            case SLOT_PPSSPP_FONT: {
                const std::string ms = ppssppMemstickAt(R);
                if (!ms.empty()) b = at(ms) + (strcasecmp(baseName(ms).c_str(), "PSP") == 0 ? "" : "/PSP") + "/flash0/font";
                break;
            }
            case SLOT_SCUMMVM: {
                std::string ini;
                appFileRead(P(appdef::kScummvmIni), &ini, R);
                const std::string x = iniMainGet(ini, "extrapath");
                if (x.empty()) { b = P(kScummvmExtra); scummSetPath = true; }   // unset: its app folder, set as it
                else if (x[0] == '/' && x.compare(0, 5, "/saf/") != 0 && isDir(at(x))) b = at(x);
                // else set to something unusable: no folder
                scummExtra = b;
                break;
            }
            case SLOT_AZAHAR: { std::string prefs, uri; appPrefsRead(P(appdef::kAzaharPrefs), &prefs, R); const std::string a = azaharDir(prefs, &uri); if (!a.empty()) b = at(a); break; }
            case SLOT_HAKUX: b = P(kHakuxConsole); break;
            case SLOT_SEEDLESSDS: b = P(kSeedlessSys); break;
            case SLOT_MAME4DROID: b = P(kMame4droidRoms); break;
            default: break;
        }
        while (b.size() > 1 && b.back() == '/') b.pop_back();
        // Only where emulators keep files: shared storage, a card that is in, the app's own folder.
        if (!b.empty()) {
            const std::string c = safePath(b) ? canonical(b) : std::string();
            const std::string pkg = slotPackage(kind);
            const std::string media = R + "/data/media/0", card = R + "/mnt/media_rw";
            const size_t vol = c.find('/', card.size() + 1);
            bool ok = !c.empty() && safePath(c)
                && (under(c, media)
                    || (under(c, card) && vol != std::string::npos && isDir(c.substr(0, vol)))
                    || (!pkg.empty() && under(c, R + "/data/data/" + pkg))
                    || (kind == SLOT_DRASTIC_NANO && under(c, R + "/data/system/drastic-nano")));
            // ... and never another app's Android/data or Android/obb folder
            const bool otherApp = ok && (c == media + "/Android/data" || c == media + "/Android/obb"
                                         || under(c, media + "/Android/obb")
                                         || (under(c, media + "/Android/data")
                                             && (pkg.empty() || pkgAfter(c, media + "/Android/data") != pkg)));
            if (otherApp) ok = false;
            if (!ok) ALOGW("bios: %s's folder %s is not one files are installed to", slotAppName(kind), b.c_str());
            b = ok ? c : std::string();
            if (kind == SLOT_SCUMMVM) { scummExtra = b; if (b.empty()) scummSetPath = false; }
        }
        baseCache[kind] = b;
        return b;
    };

    std::vector<BiosOp> ops;
    std::vector<size_t> pups;                 // Vita3K's, done last
    std::set<std::string> notInstalled, noFolder, claimed;
    for (size_t mi = 0; mi < matches.size(); mi++) {
        BiosMatch& m = matches[mi];
        const File& c = cat[m.file];
        for (const Slot& s : c.slots) {
            if (s.kind == SLOT_VITA3K) {
                if (fromManifest) continue;   // Vita3K runs only from the dialog
                if (!pk.installed(kVita3kPkg)) { notInstalled.insert(slotAppName(s.kind)); continue; }
                pups.push_back(mi);
                continue;
            }
            if (!pk.installed(slotPackage(s.kind))) { notInstalled.insert(slotAppName(s.kind)); continue; }
            const std::string b = base(s.kind);
            if (b.empty()) { noFolder.insert(slotAppName(s.kind)); continue; }
            if (s.flags & SLOTF_SATURN_SEL) {   // YabaSanshiro's own file: Japan, or no other Saturn BIOS there
                bool other = pathExists(b + "/mpr-17933.bin") || pathExists(b + "/sega_101.bin");
                for (const auto& o : matches) if (o.name == "mpr-17933.bin" || o.name == "sega_101.bin") other = true;
                if (region != REGION_JPN && other) continue;
            }
            if (c.match == MATCH_FOLDERSET) {   // every file of the folder, around what is there
                const std::string top = b + "/" + m.name;
                std::function<void(const std::string&, const std::string&, int)> add =
                    [&](const std::string& from, const std::string& to, int depth) {
                    if (depth > 8) return;
                    DIR* d = opendir(from.c_str());
                    if (!d) return;
                    std::vector<std::string> names;
                    while (dirent* e = readdir(d)) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
                    closedir(d);
                    std::sort(names.begin(), names.end());
                    for (const auto& n : names) {
                        const std::string sp = from + "/" + n, dp = to + "/" + n;
                        struct stat st;
                        if (lstat(sp.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) continue;   // links are not followed
                        if (S_ISDIR(st.st_mode)) { add(sp, dp, depth + 1); continue; }
                        if (!S_ISREG(st.st_mode) || (uint64_t)st.st_size > kMaxFile || !safePath(dp)) continue;
                        BiosOp op{mi, s.kind, s.flags, sp, dp};
                        op.rel = dp.substr(b.size() + 1);
                        op.size = (uint64_t)st.st_size;
                        op.inSet = true;
                        ops.push_back(op);
                    }
                };
                add(m.src, top, 0);
                m.targets.push_back(top);
                continue;
            }
            const std::string rel = *s.path ? std::string(s.path) : m.name;
            const std::string dst = b + "/" + rel;
            if (!safePath(dst)) { ALOGW("bios: %s is not a path files are installed to", dst.c_str()); continue; }
            if (!claimed.insert(dst).second) continue;   // a better version (earlier in the catalogue) has it
            BiosOp op{mi, s.kind, s.flags, m.src, dst};
            op.rel = rel;
            op.size = m.size;
            ops.push_back(op);
        }
    }

    // What is there already: the same file; another version of it where versions share one place
    // (GameCube's IPL.bin); or something else, kept.
    auto existing = [&](BiosOp& op) {
        const BiosMatch& m = matches[op.match];
        std::string have;
        if (hashFile(op.dst, &have, nullptr, true) && !m.sha1.empty() && have == m.sha1) { op.state = 2; return; }
        if (op.inSet) { op.state = 6; return; }   // a set goes around what is there
        bool alternate = false;
        for (size_t i = 0; i < cat.size() && !alternate && !have.empty(); i++) {
            if ((int)i == m.file || have != cat[i].sha1) continue;
            for (const Slot& sl : cat[i].slots)
                if (sl.kind == op.kind && *sl.path && op.rel == sl.path) alternate = true;
        }
        op.state = alternate ? 2 : 3;
    };
    for (auto& op : ops) if (pathExists(op.dst)) existing(op);

    // ---- 3. room ----
    // Taken in catalogue order while the drive has room for them (8 MB kept free); the rest skipped.
    std::map<dev_t, Drive> drives;
    std::map<dev_t, uint64_t> admitted;
    for (auto& op : ops) {
        if (op.state != 0) continue;
        op.dev = driveOf(op.dst, &drives);
        Drive& d = drives[op.dev];
        const uint64_t n = roundUp(op.size, d.bsize);
        d.need += n;
        if (!admit(&admitted[op.dev], n, kSpare, d.avail)) { op.state = 4; d.full = true; }
    }
    for (auto& kv : drives) {
        if (!kv.second.full) continue;
        lines.push_back(tfmt("Not enough space on %s: %s needed, %s free", kv.second.name.c_str(),
                             humanSize(kv.second.need + kSpare).c_str(), humanSize(kv.second.avail).c_str()));
        kv.second.full = false;   // what was taken still goes
    }

    // ---- 4. copy ----
    std::set<std::string> restore;   // under /data/data: the topmost paths made
    std::map<int, int> newPerKind;
    for (auto& op : ops) {
        if (op.state != 0) continue;
        if (cancelled()) { op.state = 7; continue; }
        Drive& d = drives[op.dev];
        if (d.full) { op.state = 4; continue; }
        progress([&](InstallProgress& p) { p.current = baseName(op.dst); });
        std::string top;
        const std::string pkg = slotPackage(op.kind);
        int err = copyNew(op.src, op.dst, pkg, pk, &sh->cancel, &top, R);
        if (err == ECANCELED) { op.state = 7; continue; }
        if (err == EEXIST) { existing(op); continue; }   // one appeared meanwhile: kept
        if (err == ENOSPC) {
            d.full = true;
            uint64_t left = 0;
            for (const auto& o : ops) if (o.dev == op.dev && (o.state == 0)) left += roundUp(o.size, d.bsize);
            uint64_t bsize = 0, avail = 0;
            if (!freeSpace(dirName(op.dst), &bsize, &avail)) avail = 0;
            lines.push_back(tfmt("Not enough space on %s: %s needed, %s free", d.name.c_str(), humanSize(left + kSpare).c_str(),
                                 humanSize(avail).c_str()));
            op.state = 4;
            continue;
        }
        if (err) {
            ALOGW("bios: %s -> %s failed: %s", op.src.c_str(), op.dst.c_str(), strerror(err));
            op.state = 5;
            continue;
        }
        op.state = 1;
        newPerKind[op.kind]++;
        const Owner o = ownerFor(op.dst, pkg, R);
        if (o.kind == OWN_APPDATA) restore.insert(top.empty() ? op.dst : top);
        ALOGI("bios: installed %s", op.dst.c_str());
    }
    for (auto it = restore.begin(); it != restore.end(); ++it) {
        bool inner = false;   // inside one made above it: that one's restorecon covers it
        for (const auto& o : restore) if (o != *it && it->compare(0, o.size() + 1, o + "/") == 0) inner = true;
        if (!inner && safePath(*it)) run({"/system/bin/restorecon", "-R", *it}, nullptr, 60000);
    }

    // Results per file.
    for (const auto& op : ops) {
        BiosMatch& m = matches[op.match];
        if (op.state == 1) m.installed++;
        else if (op.state == 2 || op.state == 6) m.already++;
        else if (op.state == 3) m.conflicts++;
        else if (op.state == 4 || op.state == 5) m.failed++;
        if ((op.state == 1 || op.state == 2) && !op.inSet
            && std::find(m.targets.begin(), m.targets.end(), op.dst) == m.targets.end())
            m.targets.push_back(op.dst);
    }

    // ---- 5. settings, only for emulators that got a new file ----
    if (newPerKind[SLOT_ARMSX2] > 0) {   // ARMSX2's one BIOS: kept unless unset or gone; recommended region first
        std::string prefs;
        appPrefsRead(P(appdef::kArmsx2Prefs), &prefs, R);
        const std::string cur = sdPrefsGet(prefs, "bios");
        // Kept when set: to a file that is there, or to anything not a plain path (content://, say).
        if (cur.empty() || (cur[0] == '/' && !pathExists(at(cur)))) {
            int best = -1, bestScore = -1;
            for (size_t i = 0; i < ops.size(); i++) {
                const BiosOp& op = ops[i];
                if (op.kind != SLOT_ARMSX2 || !(op.flags & SLOTF_SELECT) || (op.state != 1 && op.state != 2)) continue;
                const BiosMatch& m = matches[op.match];
                const File& c = cat[m.file];
                int r = m.region;
                if (r < 0) r = !strncmp(c.desc, "USA", 3) ? REGION_USA : !strncmp(c.desc, "Europe", 6) ? REGION_EUR
                             : !strncmp(c.desc, "Japan", 5) ? REGION_JPN : -1;
                const int score = (c.rec & regionBit(region)) ? 3 : r == (int)region ? 2 : 1;
                if (score > bestScore) { bestScore = score; best = (int)i; }
            }
            if (best >= 0) {
                const std::string app = shown(ops[best].dst);
                forceStop("com.armsx2");
                appPrefsRead(P(appdef::kArmsx2Prefs), &prefs, R);
                if (sdPrefsSet(&prefs, "bios", app) && appFileWrite(P(appdef::kArmsx2Prefs), prefs, env.run, R)) {
                    lines.push_back(tfmt("ARMSX2 uses %s", baseName(app).c_str()));
                } else {
                    ALOGW("bios: ARMSX2's BIOS setting could not be written");
                }
            }
        }
    }
    if (newPerKind[SLOT_MUPEN] > 0) {   // M64Plus FZ's 64DD IPL setting, when unset
        std::string prefs;
        appPrefsRead(P(appdef::kMupenPrefs), &prefs, R);
        if (sdPrefsGet(prefs, "japanIdlPath64dd").empty()) {
            for (const auto& op : ops) {
                if (op.kind != SLOT_MUPEN || !(op.flags & SLOTF_PREF_64DD) || op.state != 1) continue;
                forceStop("org.mupen64plusae.v3.fzurita");
                appPrefsRead(P(appdef::kMupenPrefs), &prefs, R);
                if (!sdPrefsSet(&prefs, "japanIdlPath64dd", sdFileUri(shown(op.dst))) || !appFileWrite(P(appdef::kMupenPrefs), prefs, env.run, R))
                    ALOGW("bios: M64Plus FZ's 64DD setting could not be written");
                break;
            }
        }
    }
    if (newPerKind[SLOT_SCUMMVM] > 0 && scummSetPath) {   // ScummVM's extra path, when unset
        forceStop("org.scummvm.scummvm");
        std::string ini;
        appFileRead(P(appdef::kScummvmIni), &ini, R);
        if (iniMainGet(ini, "extrapath").empty()
            && !appFileWrite(P(appdef::kScummvmIni), iniMainSet(ini, "extrapath", shown(scummExtra)), env.run, R))
            ALOGW("bios: ScummVM's extra path could not be written");
    }

    // ---- 6. Vita3K, last: it installs its firmware itself, its window up meanwhile ----
    if (!pups.empty()) {
        std::string cfg;
        appFileRead(P(appdef::kVita3kConfig), &cfg, R);
        std::string pref = vita3kPrefPath(cfg);
        if (pref.empty()) pref = appdef::kVita3kDefPref;
        pref = at(pref);
        const std::string cpref = safePath(pref) ? canonical(pref) : std::string();
        const size_t vol = cpref.find('/', R.size() + 14);
        const bool prefOk = under(cpref, R + "/data/media/0")
                            || (under(cpref, R + "/mnt/media_rw") && vol != std::string::npos && isDir(cpref.substr(0, vol)));
        if (!prefOk) { noFolder.insert("Vita3K"); for (size_t mi : pups) matches[mi].failed++; pups.clear(); }
        // A file every complete install has: the system software's shell (vs0:vsh/shell/shell.self, the
        // home screen every firmware boots into) and the font package's Latin font (sa0:data/font/pvf/ltn0.pvf).
        auto present = [&](bool font) {
            return pathExists(pref + (font ? "/sa0/data/font/pvf/ltn0.pvf" : "/vs0/vsh/shell/shell.self"));
        };
        std::vector<size_t> todo;
        std::set<bool> kinds;
        for (size_t mi : pups) {
            BiosMatch& m = matches[mi];
            if (present(m.font) || kinds.count(m.font)) { m.already++; continue; }
            kinds.insert(m.font);
            todo.push_back(mi);
        }
        std::stable_sort(todo.begin(), todo.end(), [&](size_t a, size_t b) { return !matches[a].font && matches[b].font; });
        // Room: the staged copy, and about three times its size for what Vita3K extracts.
        if (!todo.empty()) {
            std::map<dev_t, Drive> vd;
            uint64_t total = 0;
            for (size_t mi : todo) total += matches[mi].size;
            const dev_t sd = driveOf(P(kVita3kStage), &vd), pd = driveOf(pref, &vd);
            vd[sd].need += roundUp(total, vd[sd].bsize);
            vd[pd].need += 3 * total;
            for (auto& kv : vd) {
                if (kv.second.need + kSpare <= kv.second.avail) continue;
                lines.push_back(tfmt("Not enough space on %s: %s needed, %s free", kv.second.name.c_str(),
                                     humanSize(kv.second.need + kSpare).c_str(), humanSize(kv.second.avail).c_str()));
                for (size_t mi : todo) matches[mi].failed++;
                todo.clear();
                break;
            }
        }
        if (!todo.empty() && !cancelled()) {
            progress([&](InstallProgress& p) { p.current = "Vita3K"; });
            forceStop(kVita3kPkg);
            std::string prefs;
            appPrefsRead(P(kVita3kAppPrefs), &prefs, R);
            const int oldFlag = prefsGetBool(prefs, "initial_setup_completed");   // put back if nothing installs
            if (!prefsSetBool(&prefs, "initial_setup_completed", true) || !appFileWrite(P(kVita3kAppPrefs), prefs, env.run, R))
                ALOGW("bios: Vita3K's setup flag could not be written");
            bool vitaInstalled = false;
            std::vector<std::pair<size_t, std::string>> staged;
            std::string stageTop;
            for (size_t mi : todo) {
                BiosMatch& m = matches[mi];
                const std::string dst = P(kVita3kStage) + "/" + cat[m.file].name;
                removeAt(dst, 0);
                const int err = copyNew(m.src, dst, kVita3kPkg, pk, &sh->cancel, &stageTop, R);
                if (err) { ALOGW("bios: staging %s failed: %s", m.src.c_str(), strerror(err)); m.failed++; continue; }
                staged.push_back({mi, dst});
            }
            auto unstage = [&]() {
                for (const auto& st : staged) removeAt(st.second, 0);
                removeAt(P(kVita3kStage), AT_REMOVEDIR);
            };
            size_t tried = 0;   // staged ones Vita3K was asked to install; the rest failed (skipped)
            if (!staged.empty() && !cancelled()) {
                // Each log window starts in a second after everything before it (bounded: the
                // wall clock may jump).
                auto nextSecond = []() {
                    const time_t t = time(nullptr) + 1;
                    for (int i = 0; i < 40 && time(nullptr) < t; i++) usleep(50000);
                    return t;
                };
                auto vitaPid = [&]() {
                    std::string out;
                    run({"/system/bin/pidof", kVita3kPkg}, &out, 5000);
                    const size_t e = out.find_first_not_of("0123456789");
                    return out.substr(0, e);
                };
                const time_t t0 = nextSecond();
                run({"/system/bin/am", "start", "-W", "-n", "org.vita3k.emulator/.MainActivity"}, nullptr, 30000);
                std::string pid = vitaPid();
                bool ready = false, aborted = false;
                const int64_t initEnd = monoMs() + 60000;
                for (int i = 0; i < 120 && !ready && monoMs() < initEnd; i++) {
                    if (cancelled()) { aborted = true; break; }
                    if (pid.empty()) pid = vitaPid();
                    if (!pid.empty()) ready = logcatFind(run, t0, {"Vita3K Android initialised"}, pid) >= 0;
                    if (!ready) sleep(1);
                }
                if (!ready && !aborted) {
                    lines.push_back(trDyn("Vita3K did not start, so its firmware was not installed"));
                    aborted = true;
                }
                int opId = (int)(time(nullptr) % 100000);
                for (const auto& st : staged) {
                    // Not after a cancel; one whose extraction has started finishes (never cut off).
                    if (aborted || cancelled()) { aborted = true; break; }
                    BiosMatch& m = matches[st.first];
                    const bool font = m.font;
                    progress([&](InstallProgress& p) { p.current = "Vita3K: " + std::string(cat[m.file].name); });
                    const time_t t1 = nextSecond();
                    run({"/system/bin/am", "start-foreground-service", "-n", "org.vita3k.emulator/.InstallForegroundService",
                             "--el", "operation_id", std::to_string(++opId), "--es", "operation_type", "FIRMWARE",
                             "--es", "path", shown(st.second)}, nullptr, 15000);
                    int result = -1;   // 0 done, 1 failed; a timeout stays -1 (a failure)
                    bool sawDec = false;   // PUP_DEC made by this install (a leftover is older than t1)
                    // 5 minutes; longer while the extraction still grows (checked every 5 s), up to 15
                    // minutes, and given up after 60 s without growth.
                    const int64_t start = monoMs(), soft = start + 300000, hard = start + 900000;
                    int64_t lastGrowth = start;
                    uint64_t measure = 0;
                    bool grew = false;   // since the first look (a partial vs0/sa0 may be there before)
                    for (int s = 0; s < 1200 && result < 0; s++) {
                        const int64_t now = monoMs();
                        if (now >= hard || (now >= soft && now - lastGrowth > 60000)) break;
                        if (s % 5 == 0) {
                            const uint64_t m2 = treeMeasure(pref + "/PUP_DEC") + treeMeasure(pref + (font ? "/sa0" : "/vs0"));
                            if (m2 > measure) { grew = s > 0; measure = m2; lastGrowth = now; }
                        }
                        if (!sawDec && !grew && cancelled()) { aborted = true; break; }   // nothing extracted yet
                        sleep(1);
                        if (pid.empty()) pid = vitaPid();
                        const int f = logcatFind(run, t1, {"installed successfully", "installation failed"}, pid);
                        if (f >= 0) { result = f; break; }
                        // Vita3K removes PUP_DEC when it has extracted everything.
                        struct stat ds;
                        if (lstat((pref + "/PUP_DEC").c_str(), &ds) == 0) { if (ds.st_mtime >= t1) sawDec = true; }
                        else if (sawDec) result = present(font) ? 0 : 1;
                    }
                    if (aborted) break;
                    tried++;
                    if (result == 0) {
                        vitaInstalled = true;
                        m.installed++;
                        m.targets.push_back(pref + (font ? "/sa0" : "/vs0"));
                        lines.push_back(font ? trDyn("PS Vita font package installed in Vita3K") : trDyn("PS Vita system software installed in Vita3K"));
                    } else {
                        m.failed++;
                        lines.push_back(result == 1 ? tfmt("Vita3K could not install %s", baseName(m.src).c_str())
                                                    : tfmt("Vita3K did not finish installing %s", baseName(m.src).c_str()));
                    }
                    removeAt(st.second, 0);
                    if (result == -1 && !font) break;   // the system software timed out: nothing more this run
                }
                unstage();
                forceStop(kVita3kPkg);
                // Nano's home kept the panel meanwhile (DRM) or is the overlay above the apps: nothing
                // to bring back.
            } else {
                unstage();
            }
            for (size_t k = tried; k < staged.size(); k++) matches[staged[k].first].failed++;
            if (!vitaInstalled) {   // Vita3K's own setup again, as before
                std::string p2;
                appPrefsRead(P(kVita3kAppPrefs), &p2, R);
                if (oldFlag < 0) prefsRemoveBool(&p2, "initial_setup_completed");
                else prefsSetBool(&p2, "initial_setup_completed", oldFlag == 1);
                if (!appFileWrite(P(kVita3kAppPrefs), p2, env.run, R)) ALOGW("bios: Vita3K's setup flag could not be put back");
            }
        }
    }

    // ---- 7. report ----
    int nInstalled = 0, nAlready = 0, nConflicts = 0;
    std::vector<std::string> lInst, lAlready, lConf, lFail;
    std::set<std::string> said;
    bool mame2003 = false;
    for (const auto& m : matches) {
        const File& c = cat[m.file];
        std::string what = c.match == MATCH_FOLDERSET ? std::string("blueMSX ") + m.name
                         : c.match == MATCH_PS2ROMDIR ? baseName(m.src) + (m.label.empty() ? "" : " (" + m.label + ")")
                         : c.match == MATCH_PUP ? baseName(m.src) + (m.label.empty() ? "" : " (" + m.label + ")")
                         : m.name;
        const std::string sys = rowSystem(m.file);
        const std::string key = sys + "\n" + what;
        if (!said.insert(key + std::to_string(m.installed > 0)).second) continue;
        if (m.installed > 0) {
            nInstalled++;
            if (c.match != MATCH_PUP) lInst.push_back(tfmt("%s: %s installed", sys.c_str(), what.c_str()));
            if (c.match == MATCH_ZIPSET && strcmp(c.row, "qsound")
                && std::any_of(c.slots.begin(), c.slots.end(), [](const Slot& s) { return s.kind == SLOT_RA; }))
                mame2003 = true;
        } else if (m.conflicts > 0) {
            nConflicts++;
            lConf.push_back(tfmt("%s: a different %s is already there (kept)", sys.c_str(), what.c_str()));
        } else if (m.already > 0) {
            nAlready++;
            lAlready.push_back(tfmt("%s: %s already installed", sys.c_str(), what.c_str()));
        } else if (m.failed > 0) {
            lFail.push_back(tfmt("%s: %s could not be installed", sys.c_str(), what.c_str()));
        }
    }
    std::vector<std::string> report;
    for (auto* l : {&lInst, &lAlready, &lConf, &lFail}) report.insert(report.end(), l->begin(), l->end());
    for (const auto& a : notInstalled) report.push_back(tfmt("%s not installed", trDyn(a.c_str())));
    for (const auto& a : noFolder) report.push_back(tfmt("%s has no folder set up yet; its files were skipped", trDyn(a.c_str())));
    if (mame2003) report.push_back(trDyn("MAME 2003-Plus reads BIOS zips from the game's folder: copy them there for it."));
    if (cancelled()) report.insert(report.begin(), trDyn("Install cancelled."));
    report.insert(report.end(), lines.begin(), lines.end());
    if (unknown > 0) {
        std::string names;
        for (const auto& n : unknownNames) names += (names.empty() ? "" : ", ") + n;
        if (unknown > (int)unknownNames.size()) names += ", ...";
        report.push_back(tfmt("%d other files were not recognised: %s", unknown, names.c_str()));
    }

    // ---- 8. the manifest: what is where now (written only when it changed) ----
    {
        njson::Value root = manifest.isObject() ? manifest : njson::Value::makeObject();
        root.set("version") = njson::Value::makeNumber(1);
        njson::Value& list = root.set("files");
        if (!list.isArray()) list = njson::Value::makeArray();
        for (const auto& m : matches) {
            if (m.targets.empty()) continue;
            const File& c = cat[m.file];
            njson::Value* e = nullptr;
            for (auto& v : list.arr)
                if (v.getString("row") == c.row && v.getString("file") == c.name && v.getString("name") == m.name
                    && v.getString("sha1") == m.sha1) { e = &v; break; }
            if (!e) { list.arr.push_back(njson::Value::makeObject()); e = &list.arr.back(); }
            e->set("row") = njson::Value::makeString(c.row);
            e->set("file") = njson::Value::makeString(c.name);
            e->set("name") = njson::Value::makeString(m.name);
            e->set("sha1") = njson::Value::makeString(m.sha1);
            if (!fromManifest) e->set("source") = njson::Value::makeString(m.src);
            if (m.font) e->set("font") = njson::Value::makeBool(true);
            if (m.region >= 0) e->set("region") = njson::Value::makeNumber(m.region);
            njson::Value& t = e->set("targets");
            if (!t.isArray()) t = njson::Value::makeArray();
            for (const auto& p : m.targets) {
                bool have = false;
                for (const auto& v : t.arr) if (v.asString() == p) have = true;
                if (!have) t.arr.push_back(njson::Value::makeString(p));
            }
        }
        const std::string text = njson::serialize(root, true);
        if (!list.arr.empty() && text != njson::serialize(manifest, true)) {
            mkdir(P(kManifestDir).c_str(), 0770);
            const std::string tmp = manifestPath + ".tmp";
            const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
            bool ok = fd >= 0 && write(fd, text.data(), text.size()) == (ssize_t)text.size() && fsync(fd) == 0;
            if (fd >= 0) ok = close(fd) == 0 && ok;
            if (!ok || rename(tmp.c_str(), manifestPath.c_str()) != 0) { unlink(tmp.c_str()); ALOGW("bios: the manifest could not be written"); }
        }
    }

    for (const auto& l : report) ALOGI("bios: %s", l.c_str());
    // A reinstall that found every emulator's folder is done; one that did not (a card not in yet)
    // stays pending for the next start.
    if (fromManifest && noFolder.empty() && !cancelled()) unlink(P(kReinstallMarker).c_str());
    bool again = false;
    {
        std::lock_guard<std::mutex> lk(sh->mu);
        if (!fromManifest) {
            InstallProgress& p = sh->progress;
            p.running = false;
            p.done = true;
            p.current.clear();
            p.installed = nInstalled;
            p.already = nAlready;
            p.conflicts = nConflicts;
            p.unknown = unknown;
            p.noneFound = noneFound;
            p.lines = report;
        }
        sh->busy = false;
        // A queued reinstall still runs after a user's cancel, never after ~NanoMenu's.
        std::string ra;
        if (sh->reinstallQueued && !sh->closing) {
            sh->reinstallQueued = false;
            sh->busy = again = true;
            sh->cancel = false;
            ra = sh->queuedRaConfig;
        }
        if (!again) sh->running = false;
        if (again && nextRaConfig) *nextRaConfig = ra;
    }
    if (env.wake) env.wake();   // the report, or the waiting dialog's next step
    return again;
}

}  // namespace nanobios
}  // namespace android
