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

// The ES-DE folder as Nano's store. Once Move ES-DE to microSD has run, persist.gammaos.nano.esde.dir
// names the ES-DE folder on the card and it is the one place Nano keeps what ES-DE also keeps, in
// ES-DE's own files, so mainline ES-DE pointed at the same folder sees the same library:
//   gamelists/<system>/gamelist.xml   favourites, hidden games, titles, metadata, play stats, the
//                                     emulator of a game (<altemulator>) and of the system
//                                     (<alternativeEmulator>); Recently Played is the <lastplayed> games
//   collections/custom-<name>.cfg     collections (listed in es_settings.xml CollectionSystemsCustom)
//   settings/es_settings.xml          the ROM folder and the scraper settings ES-DE has too
//   custom_systems/es_systems.xml     Nano's systems in folders ES-DE has no system for
//   downloaded_media/<system>/...     covers, fan art and the other media
//   nano/                             what only Nano has (its systems, the scrape index, title
//                                     overrides, game emulators ES-DE has no label for)
// Nano keeps its in-memory lists as before; in store mode they are read from those files, and a
// save writes only what changed back to them (a gamelist is edited in place, keeping everything
// Nano does not know). Without the card the lists are empty and nothing is written. The files are
// looked at every few seconds (esdeStoreSweep): what ES-DE or a computer changed is read again,
// and a card swapped for another with its own ES-DE folder becomes the store.

#define LOG_TAG "NanoMenu"

#include "NanoMenu.h"
#include "NanoEsdeSystemName.h"
#include "NanoI18n.h"
#include "NanoJson.h"
#include "NanoPlatform.h"

#include <cutils/properties.h>
#include <dirent.h>
#include <fcntl.h>
#include <log/log.h>
#include <sys/stat.h>
#include <tinyxml2.h>
#include <unistd.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <cerrno>
#include <ctime>

namespace android {

static const char* const kEsdeDirProp = "persist.gammaos.nano.esde.dir";
static const char* const kEsdeDefaultDir = "/storage/emulated/0/ES-DE";
static const char* const kGamelistInc = "\x01+1";   // esdeGamelistSet: add one to the number ("\x01+N": N)
static const char kGamelistIfEmpty = '\x02';         // esdeGamelistSet: a value set only where there is none
static const char kGamelistIfLater = '\x03';         // esdeGamelistSet: a time set only over an earlier one
static const char* const kGamelistRemove = "\x01remove";   // esdeGamelistSet field: the whole entry goes
static const char* const kGamelistSystem = "\x01system";   // mEsdeGlPending game: the gamelist's own fields

static void esdeMkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if ((i == dir.size() || dir[i] == '/') && !nanoscraper::isVolumeRoot(dir.substr(0, i)))
            ::mkdir(dir.substr(0, i).c_str(), 0775);
}

static bool esdeWriteAtomic(const std::string& path, const std::string& text) {
    esdeMkdirs(path.substr(0, path.find_last_of('/')));
    const std::string tmp = path + ".nanotmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

static bool esdeReadFile(const std::string& path, std::string* out) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size < 64 * 1024 * 1024;
    if (ok) {
        out->resize(st.st_size);
        size_t got = 0;
        while (got < out->size()) {   // FUSE and network-backed storage return short reads
            const ssize_t n = read(fd, &(*out)[got], out->size() - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        ok = got == out->size();
    }
    close(fd);
    return ok;
}

// ES-DE opens files through /storage; Nano scans some through /mnt/media_rw and /data/media/0.
static std::string esdeAppPath(const std::string& p) {
    if (p.compare(0, 14, "/mnt/media_rw/") == 0) return "/storage/" + p.substr(14);
    if (p.compare(0, 14, "/data/media/0/") == 0) return "/storage/emulated/0/" + p.substr(14);
    if (p.compare(0, 8, "/sdcard/") == 0) return "/storage/emulated/0/" + p.substr(8);
    return p;
}

static std::string esdeLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string NanoMenu::esdeDataDir() {
    char d[PROPERTY_VALUE_MAX] = {};
    property_get(kEsdeDirProp, d, "");
    std::string dir = d;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    return dir.size() > 1 && dir[0] == '/' ? dir : std::string(kEsdeDefaultDir);
}

bool NanoMenu::esdeStoreMode() {
    char d[PROPERTY_VALUE_MAX] = {};
    property_get(kEsdeDirProp, d, "");
    return d[0] == '/';
}

// The path Nano can stat without the app storage layer (a card's raw mount; Nano runs as root).
static std::string esdeRawPath(const std::string& p) {
    if (p.compare(0, 20, "/storage/emulated/0/") == 0) return "/data/media/0/" + p.substr(20);
    if (p.compare(0, 9, "/storage/") == 0 && p.compare(0, 14, "/storage/self/") != 0) return "/mnt/media_rw/" + p.substr(9);
    return p;
}

// Store mode with the card in (checked at most once a second).
bool NanoMenu::esdeStoreReady() {
    if (!esdeStoreMode()) return false;
    const int64_t now = (int64_t)uptimeMillis();
    if (mEsdeStoreCheckedMs == 0 || now - mEsdeStoreCheckedMs > 1000) {
        mEsdeStoreCheckedMs = now;
        mEsdeStoreOk = access(esdeDataDir().c_str(), W_OK) == 0;
        if (mEsdeStoreOk) mEsdeCardGoneMs = 0;
        else mEsdeStoreOk = esdeStoreFollowCard() && access(esdeDataDir().c_str(), W_OK) == 0;
    }
    return mEsdeStoreOk && !mEsdeFollowPending;   // the other card's lists are not saved on this one
}

// The card the ES-DE folder was on is out: the same folder on a card that is in (one swapped for
// another with its own ES-DE folder) becomes the store, as a move there would have made it. A card
// that is in without the folder is left alone.
bool NanoMenu::esdeStoreFollowCard() {
    const std::string dir = esdeDataDir();
    if (dir.compare(0, 9, "/storage/") != 0) return false;
    const size_t sl = dir.find('/', 9);
    const std::string vol = dir.substr(9, sl == std::string::npos ? std::string::npos : sl - 9);
    const std::string rel = sl == std::string::npos ? std::string() : dir.substr(sl);
    if (vol.empty() || vol == "emulated" || vol == "self" || nanoplat::get().volumes->present(vol)) {
        mEsdeCardGoneMs = 0;
        return false;
    }
    // Only once the card has stayed out a while, and not at boot: a card being mounted (or two cards
    // coming up in either order) is not another card.
    const int64_t now = (int64_t)uptimeMillis();
    if (mEsdeCardGoneMs == 0) mEsdeCardGoneMs = now;
    if (now < 30000 || now - mEsdeCardGoneMs < 10000) return false;
    std::vector<std::string> found = nanoplat::get().volumes->writableFolders(vol, rel);
    if (found.empty()) return false;
    std::sort(found.begin(), found.end());
    char sc[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.scraper.dir", sc, "");
    if (std::string(sc) == dir + "/nano") property_set("persist.gammaos.scraper.dir", (found[0] + "/nano").c_str());
    property_set(kEsdeDirProp, found[0].c_str());
    mEsdeGlPending.clear();   // edits for the card that went
    mEsdeGlDueMs = 0;
    mEsdeFollowPending = true;   // esdeStoreTick reads the new one
    ALOGI("esde: %s is out; using the ES-DE folder on the card that is in, %s", dir.c_str(), found[0].c_str());
    return true;
}

// The folder and the device it is on: another card changes it (a card's folder is named after it).
std::string NanoMenu::esdeStoreIdentity() {
    const std::string dir = esdeDataDir();
    struct stat st;
    if (stat(esdeRawPath(dir).c_str(), &st) != 0 && stat(dir.c_str(), &st) != 0) return std::string();
    return dir + "\n" + std::to_string((unsigned long long)st.st_dev);
}

// A file's modification time and size in one number, -1 when it is not there.
static long long esdeStamp(const std::string& path) {
    struct stat st;
    if (stat(esdeRawPath(path).c_str(), &st) != 0 && stat(path.c_str(), &st) != 0) return -1;
    return ((long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec) * 31 + (long long)st.st_size;
}

// A game's key whatever path or case it is named with (the card and Android's storage ignore case).
std::string NanoMenu::esdeKey(const std::string& path) {
    std::string k = hiddenKey(path);
    for (auto& c : k) c = (char)tolower((unsigned char)c);
    return k;
}

// The scanned games by esdeKey, built again when they changed (each load that uses them asks).
void NanoMenu::esdeRomCaseRefresh() {
    size_t sig = mXmbSystems.size() + 1;
    for (const auto& s : mXmbSystems)
        for (const auto& r : s.roms) sig += std::hash<std::string>()(r) * 2654435761u + 1;
    if (sig == mEsdeRomCaseSig && !mEsdeRomCase.empty()) return;
    mEsdeRomCaseSig = sig;
    mEsdeRomCase.clear();
    for (const auto& s : mXmbSystems)
        for (const auto& r : s.roms) mEsdeRomCase.emplace(esdeKey(r), r);
}

// The spelling the scan found a game under, "" when the scan has no such game.
std::string NanoMenu::esdeCanonRom(const std::string& path) {
    if (mEsdeRomCase.empty()) esdeRomCaseRefresh();
    auto it = mEsdeRomCase.find(esdeKey(path));
    return it == mEsdeRomCase.end() ? std::string() : it->second;
}

// root/rel as its files are spelt, "" when there is none: a gamelist or collection written elsewhere
// can name a game in another case than its file has. A game the scan has is taken from it; another
// (a hidden one) from its folder's listing.
std::string NanoMenu::esdeFindGame(const std::string& root, const std::string& rel) {
    const std::string whole = root + "/" + rel;
    const std::string scanned = esdeCanonRom(whole);
    if (!scanned.empty()) return scanned;
    if (access(esdeRawPath(whole).c_str(), F_OK) != 0 && access(whole.c_str(), F_OK) != 0) return std::string();
    std::string cur = root;
    for (size_t p = 0; p < rel.size();) {
        size_t q = rel.find('/', p);
        if (q == std::string::npos) q = rel.size();
        const std::string part = rel.substr(p, q - p);
        p = q + 1;
        if (part.empty() || part == ".") continue;
        // Each folder listed once per load (mEsdeDirCase): lowercase name -> its spelling.
        auto dc = mEsdeDirCase.find(cur);
        if (dc == mEsdeDirCase.end()) {
            dc = mEsdeDirCase.emplace(cur, std::unordered_map<std::string, std::string>()).first;
            if (DIR* d = opendir(esdeRawPath(cur).c_str())) {
                while (dirent* e = readdir(d)) dc->second.emplace(esdeLower(e->d_name), e->d_name);
                closedir(d);
            }
        }
        auto n = dc->second.find(esdeLower(part));
        cur += "/" + (n == dc->second.end() ? part : n->second);
    }
    return cur;
}

std::string NanoMenu::romNamesPath() {
    return esdeStoreMode() ? esdeDataDir() + "/nano/names.json" : std::string("/data/system/nano_scrape/names.json");
}

// The folders a system's games are in: the scanned ones, else (not scanned yet) the candidate
// folders that exist. Cached for a few seconds: the lookups run per game.
const std::vector<std::string>& NanoMenu::esdeSystemRoots(int sysIdx) {
    static const std::vector<std::string> kNone;
    if (sysIdx < 0 || sysIdx >= (int)mXmbSystems.size()) return kNone;
    const int64_t now = (int64_t)uptimeMillis();
    const XmbSystem& s = mXmbSystems[sysIdx];
    // By id: the list is reordered and systems removed. Kept while the scanned folders stay the
    // same (the guessed ones, of games listed from the cache before a scan, for a minute).
    const bool guess = s.activePaths.empty() && s.enabled && !s.roms.empty();
    std::string sig = guess ? "g" : "s";
    for (const auto& p : s.activePaths) sig += "\n" + p;
    auto& c = mEsdeRootsCache[s.id];
    if (c.first != 0 && c.sig == sig && (!guess || now - c.first < 60000)) return c.second;
    c.first = now;
    c.sig = sig;
    c.second.clear();
    std::vector<std::string> from = s.activePaths;
    if (guess) {
        for (const auto& p : buildScanCandidates(s)) {
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) from.push_back(p);
        }
    }
    for (std::string r : from) {
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        if (!r.empty() && std::find(c.second.begin(), c.second.end(), r) == c.second.end()) c.second.push_back(r);
    }
    return c.second;
}

// A game's gamelist and its <path> there (relative to its system folder, without "./").
// The ES-DE folder of the system folder a game is in: ES-DE keeps a gamelist per ROM folder (a
// Nano system scanning megadrive/ and genesis/ is two systems to ES-DE).
bool NanoMenu::esdeRomRef(const std::string& rom, std::string* gamelist, std::string* rel, std::string* folder) {
    size_t best = 0;
    int bestSys = -1;
    std::string bestRoot;
    const std::string rk = hiddenKey(rom);
    for (int s = 0; s < (int)mXmbSystems.size(); s++) {
        for (const auto& root : esdeSystemRoots(s)) {
            const std::string k = hiddenKey(root);
            if (k.size() > best && rk.size() > k.size() + 1 && rk.compare(0, k.size(), k) == 0 && rk[k.size()] == '/') {
                best = k.size();
                bestSys = s;
                bestRoot = root;
                *rel = rk.substr(k.size() + 1);
            }
        }
    }
    if (bestSys < 0) return false;
    const std::string f = esdeRootFolder(bestRoot, bestSys);
    if (f.empty()) return false;
    if (gamelist) *gamelist = esdeDataDir() + "/gamelists/" + f + "/gamelist.xml";
    if (folder) *folder = f;
    return true;
}

// Where a game's cover and fan art go in store mode: covers/ and fanart/ beside its other media.
bool NanoMenu::esdeArtDest(const std::string& rom, int sysIdx, nanoscraper::ArtDest* d) {
    if (!esdeStoreMode()) return false;
    std::string writeDir;
    std::vector<std::string> names, haveDirs;
    if (!esdeMediaStems(rom, sysIdx, &writeDir, &names, &haveDirs) || names.empty()) return false;
    d->box = writeDir + "covers/" + names[0] + ".png";
    d->fan = writeDir + "fanart/" + names[0] + ".jpg";
    return true;
}

// ---------------------------------------------------------------------------
// Gamelist writer: edits collect per file and are written together shortly after.
// ---------------------------------------------------------------------------

void NanoMenu::esdeGamelistSet(const std::string& rom, const char* field, const std::string& value) {
    if (!esdeStoreMode()) return;
    std::string gl, rel;
    if (!esdeRomRef(rom, &gl, &rel, nullptr)) {
        ALOGW("esde: %s is in no system folder; %s not saved", rom.c_str(), field);
        if (!strcmp(field, "favorite") || !strcmp(field, "hidden"))
            photoShowBanner(trDyn("This game is in no system folder, so the ES-DE folder cannot keep that."));
        return;
    }
    std::string& slot = mEsdeGlPending[gl][rel][field];
    if (!value.empty() && value[0] == kGamelistIfEmpty && !slot.empty()) return;   // an edit waiting wins over a scrape's default
    if (!value.empty() && value[0] == kGamelistIfLater && !slot.empty() && slot[0] == kGamelistIfLater && slot >= value)
        return;
    if (value.compare(0, 2, "\x01+") == 0 && slot.compare(0, 2, "\x01+") == 0) {   // played twice before a write
        slot = "\x01+" + std::to_string(atol(slot.c_str() + 2) + atol(value.c_str() + 2));
        return;
    }
    slot = value;
    if (mEsdeGlDueMs == 0) mEsdeGlDueMs = (int64_t)uptimeMillis() + 800;
}

// A field of a gamelist itself (not of a game): field "" removes it.
void NanoMenu::esdeGamelistSetSystem(const std::string& folder, const char* field, const std::string& value) {
    if (!esdeStoreMode() || folder.empty()) return;
    mEsdeGlPending[esdeDataDir() + "/gamelists/" + folder + "/gamelist.xml"][kGamelistSystem][field] = value;
    if (mEsdeGlDueMs == 0) mEsdeGlDueMs = (int64_t)uptimeMillis() + 800;
}

void NanoMenu::esdeGamelistTick() {
    if (mFileJobKind != 0) return;   // a move may be switching Nano to the card
    if (mEsdeGlDueMs != 0 && (int64_t)uptimeMillis() >= mEsdeGlDueMs) esdeGamelistFlush();
    esdeStoreTick();
}

bool NanoMenu::esdeGamelistFlush() {
    mEsdeGlDueMs = 0;
    if (mEsdeGlPending.empty()) return true;
    if (!esdeStoreReady()) {
        photoShowBanner(trDyn("The microSD card with your ES-DE folder is not in. The change was not saved."));
        mEsdeGlPending.clear();
        return false;
    }
    bool allOk = true;
    for (auto& file : mEsdeGlPending) {
        const std::string& path = file.first;
        tinyxml2::XMLDocument doc;
        const tinyxml2::XMLError loaded = doc.LoadFile(path.c_str());
        // tinyxml2 says "not found" for any file it could not open: only a missing one is new.
        struct stat gst;
        bool exists = loaded != tinyxml2::XML_ERROR_FILE_NOT_FOUND || stat(path.c_str(), &gst) == 0 || errno != ENOENT;
        if (loaded == tinyxml2::XML_ERROR_EMPTY_DOCUMENT && stat(path.c_str(), &gst) == 0 && gst.st_size == 0) exists = false;
        if (exists && loaded != tinyxml2::XML_SUCCESS) {
            ALOGW("esde: %s cannot be read; left as it is", path.c_str());   // never replace a gamelist we cannot read
            allOk = false;
            continue;
        }
        if (!exists) doc.Clear();
        tinyxml2::XMLElement* root = doc.FirstChildElement("gameList");
        if (!root) {
            if (exists) { ALOGW("esde: %s has no gameList; left as it is", path.c_str()); allOk = false; continue; }
            doc.InsertEndChild(doc.NewDeclaration());
            root = doc.NewElement("gameList");
            doc.InsertEndChild(root);
        }
        std::unordered_map<std::string, tinyxml2::XMLElement*> games;   // by path, lowercase (the card ignores case)
        for (tinyxml2::XMLElement* g = root->FirstChildElement("game"); g; g = g->NextSiblingElement("game")) {
            const tinyxml2::XMLElement* pe = g->FirstChildElement("path");
            std::string p = pe && pe->GetText() ? pe->GetText() : "";
            while (p.compare(0, 2, "./") == 0) p.erase(0, 2);
            if (!p.empty()) games.emplace(esdeLower(p), g);
        }
        for (auto& edit : file.second) {
            if (edit.first == kGamelistSystem) {   // <alternativeEmulator><label>: ES-DE writes it before <gameList>
                for (const auto& kv : edit.second) {
                    tinyxml2::XMLElement* c = doc.FirstChildElement(kv.first.c_str());
                    if (!c) c = root->FirstChildElement(kv.first.c_str());
                    if (kv.second.empty()) { if (c) c->Parent()->DeleteChild(c); continue; }
                    if (!c) {
                        c = doc.NewElement(kv.first.c_str());
                        tinyxml2::XMLNode* decl = doc.FirstChild() && doc.FirstChild()->ToDeclaration() ? doc.FirstChild() : nullptr;
                        if (decl) doc.InsertAfterChild(decl, c);
                        else doc.InsertFirstChild(c);
                    }
                    tinyxml2::XMLElement* l = c->FirstChildElement("label");
                    if (!l) { l = doc.NewElement("label"); c->InsertEndChild(l); }
                    l->SetText(kv.second.c_str());
                }
                continue;
            }
            tinyxml2::XMLElement* g = nullptr;
            auto gi = games.find(esdeLower(edit.first));
            if (edit.second.count(kGamelistRemove)) {
                if (gi != games.end()) { root->DeleteChild(gi->second); games.erase(gi); }
                continue;
            }
            if (gi != games.end()) {
                g = gi->second;
            } else {
                const bool anySet = std::any_of(edit.second.begin(), edit.second.end(),
                                                [](const auto& kv) { return !kv.second.empty(); });
                if (!anySet) continue;   // clearing a field of a game that has no entry
                g = doc.NewElement("game");
                tinyxml2::XMLElement* pe = doc.NewElement("path");
                pe->SetText(("./" + edit.first).c_str());
                g->InsertEndChild(pe);
                root->InsertEndChild(g);
                games.emplace(esdeLower(edit.first), g);
            }
            for (const auto& kv : edit.second) {
                tinyxml2::XMLElement* c = g->FirstChildElement(kv.first.c_str());
                if (kv.second.empty()) {
                    if (c) g->DeleteChild(c);
                    continue;
                }
                std::string v = kv.second;
                if (!v.empty() && v[0] == kGamelistIfEmpty) {
                    if (c && c->GetText() && *c->GetText()) continue;
                    v.erase(0, 1);
                }
                if (!v.empty() && v[0] == kGamelistIfLater) {
                    if (c && c->GetText() && strcmp(c->GetText(), v.c_str() + 1) >= 0) continue;
                    v.erase(0, 1);
                }
                if (v.compare(0, 2, "\x01+") == 0) {
                    const char* t = c ? c->GetText() : nullptr;
                    v = std::to_string((t ? atol(t) : 0) + atol(v.c_str() + 2));
                }
                if (!c) {
                    c = doc.NewElement(kv.first.c_str());
                    g->InsertEndChild(c);
                }
                c->SetText(v.c_str());
            }
        }
        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        if (!esdeWriteAtomic(path, std::string(printer.CStr(), printer.CStrSize() > 0 ? printer.CStrSize() - 1 : 0))) {
            ALOGW("esde: writing %s failed", path.c_str());
            allOk = false;
            continue;
        }
        esdeNoteWrite(path);
        mEsdeGamelistStamp.clear();   // the theme reads them again
    }
    mEsdeGlPending.clear();
    if (!allOk) photoShowBanner(trDyn("Some ES-DE gamelists could not be updated."));
    return allOk;
}

// ---------------------------------------------------------------------------
// Favourites and hidden games, from the gamelists' <favorite> and <hidden>.
// ---------------------------------------------------------------------------

// Every system's flagged games, each gamelist parsed again only when it changed. Also gathers what
// the gamelists say about the games (titles, emulators, play times) and the systems' emulators.
void NanoMenu::esdeStoreLoadFlags(std::vector<std::string>* favs, std::map<std::string, std::string>* hidden) {
    favs->clear();
    hidden->clear();
    mEsdeGlNames.clear();
    mEsdeGlAltEmu.clear();
    mEsdeGlSysEmu.clear();
    mEsdeGlUnreadable.clear();
    mEsdeGlPlayed.clear();
    mEsdeFolderSys.clear();
    mEsdeFolderRoots.clear();
    mEsdeDirCase.clear();
    esdeRomCaseRefresh();
    mEsdeFlagsLoadedReady = esdeStoreReady();
    if (!mEsdeFlagsLoadedReady) return;
    std::unordered_set<std::string> favKeys;
    // Each ES-DE folder with the system folders that are it.
    std::map<std::string, std::vector<std::string>> byFolder;
    for (int s = 0; s < (int)mXmbSystems.size(); s++)
        for (const auto& r : esdeSystemRoots(s)) {
            const std::string f = esdeRootFolder(r, s);
            if (f.empty()) continue;
            auto& v = byFolder[f];
            if (std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
            mEsdeFolderSys.emplace(esdeKey(r), s);
        }
    mEsdeFolderRoots = byFolder;
    for (const auto& bf : byFolder) {
        const std::string path = esdeDataDir() + "/gamelists/" + bf.first + "/gamelist.xml";
        const auto& roots = bf.second;
        std::string rootsKey;
        for (const auto& r : roots) rootsKey += r + "\n";
        EsdeFlags& f = mEsdeFlagsCache[path + "\x1f" + rootsKey];
        struct stat st;
        const bool have = stat(path.c_str(), &st) == 0;
        const long long mtime = have ? (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : -1;
        const long long size = have ? (long long)st.st_size : -1;
        if (!f.parsed || f.mtime != mtime || f.size != size) {
            f = EsdeFlags();
            f.parsed = true;
            f.mtime = mtime;
            f.size = size;
            tinyxml2::XMLDocument doc;
            const bool loaded = have && doc.LoadFile(path.c_str()) == tinyxml2::XML_SUCCESS;
            f.unreadable = have && st.st_size > 0 && (!loaded || !doc.FirstChildElement("gameList"));
            if (loaded) {
                if (tinyxml2::XMLElement* root = doc.FirstChildElement("gameList")) {
                    auto text = [](tinyxml2::XMLElement* g, const char* k) {
                        const tinyxml2::XMLElement* c = g->FirstChildElement(k);
                        return std::string(c && c->GetText() ? c->GetText() : "");
                    };
                    // Before <gameList>, where ES-DE writes it, or inside it (ES-DE reads both).
                    tinyxml2::XMLElement* ae = doc.FirstChildElement("alternativeEmulator");
                    if (!ae) ae = root->FirstChildElement("alternativeEmulator");
                    if (ae) f.sysEmu = text(ae, "label");
                    for (tinyxml2::XMLElement* g = root->FirstChildElement("game"); g; g = g->NextSiblingElement("game")) {
                        std::string rel = text(g, "path");
                        while (rel.compare(0, 2, "./") == 0) rel.erase(0, 2);
                        if (rel.empty()) continue;
                        if (text(g, "favorite") == "true") f.favs.push_back(rel);
                        if (text(g, "hidden") == "true") f.hidden.push_back(rel);
                        const std::string name = text(g, "name"), emu = text(g, "altemulator"), played = text(g, "lastplayed");
                        if (!name.empty()) f.names.emplace_back(rel, name);
                        if (!emu.empty()) f.altEmu.emplace_back(rel, emu);
                        if (played.size() >= 15 && played.compare(0, 8, "19700101") != 0) f.played.emplace_back(rel, played);
                    }
                }
            }
        }
        // The system folder a flagged game is in (none: the entry stays in the gamelist as it was),
        // found again when the scanned games change.
        if (f.resolvedSig != mEsdeRomCaseSig || !f.resolved) {
            auto find = [&](const std::string& rel) {
                for (const auto& r : roots) {
                    const std::string g = esdeFindGame(r, rel);
                    if (!g.empty()) return g;
                }
                return std::string();
            };
            f.favPaths.clear();
            f.hiddenPaths.clear();
            for (const auto& rel : f.favs) { const std::string p = find(rel); if (!p.empty()) f.favPaths.push_back(p); }
            for (const auto& rel : f.hidden) { const std::string p = find(rel); if (!p.empty()) f.hiddenPaths.push_back(p); }
            f.resolvedSig = mEsdeRomCaseSig;
            f.resolved = true;
        }
        for (const auto& p : f.favPaths)
            if (favKeys.insert(hiddenKey(p)).second) favs->push_back(p);
        for (const auto& p : f.hiddenPaths) (*hidden)[hiddenKey(p)] = p;
        // Titles and emulators by every folder the game can be in: looked up by the game's own path.
        for (const auto& r : roots) {
            const int sys = mEsdeFolderSys[esdeKey(r)];
            for (const auto& n : f.names) mEsdeGlNames[esdeKey(r + "/" + n.first)] = n.second;
            for (const auto& e : f.altEmu) mEsdeGlAltEmu[esdeKey(r + "/" + e.first)] = {e.second, bf.first, sys};
        }
        for (const auto& pl : f.played) {
            EsdePlayed e;
            e.when = pl.second;
            for (const auto& r : roots) e.paths.push_back(r + "/" + pl.first);
            mEsdeGlPlayed.push_back(std::move(e));
        }
        if (!f.sysEmu.empty()) mEsdeGlSysEmu[bf.first] = f.sysEmu;
        if (f.unreadable) mEsdeGlUnreadable.insert(bf.first);
    }
}

// What a save changes against what the files held (mEsdeStoreFavs / mEsdeStoreHidden, by hiddenKey).
void NanoMenu::esdeStoreSaveFlag(const char* field, const std::map<std::string, std::string>& now,
                                 std::map<std::string, std::string>& synced) {
    for (const auto& kv : now)
        if (!synced.count(kv.first)) esdeGamelistSet(kv.second, field, "true");
    for (const auto& kv : synced)
        if (!now.count(kv.first)) esdeGamelistSet(kv.second, field, std::string());
    synced = now;
}

// ---------------------------------------------------------------------------
// Collections: collections/custom-<name>.cfg, one game per line, %ROMPATH%/... or absolute.
// ---------------------------------------------------------------------------

// The storage a path is on (/storage/emulated/0, /storage/XXXX-XXXX), as ES-DE sees it.
static std::string esdeVolume(const std::string& appPath) {
    const std::string emu = "/storage/emulated/0";
    if (appPath.compare(0, emu.size(), emu) == 0) return emu;
    if (appPath.compare(0, 9, "/storage/") != 0) return std::string();
    const size_t sl = appPath.find('/', 9);
    return sl == std::string::npos ? appPath : appPath.substr(0, sl);
}

// ES-DE's ROM folder: es_settings.xml's ROMDirectory (~ is the ES-DE folder on Android), on the ES-DE
// folder's card when the one it names is out (a card swapped for another laid out the same), else
// ROMs beside the ES-DE folder; "" when there is no such folder.
std::string NanoMenu::esdeRomDirectory() {
    const int64_t now = (int64_t)uptimeMillis();   // asked per game: read again every few seconds
    if (mEsdeRomDirMs != 0 && now - mEsdeRomDirMs < 3000 && mEsdeRomDirFor == esdeDataDir()) return mEsdeRomDir;
    mEsdeRomDirMs = now;
    mEsdeRomDirFor = esdeDataDir();
    mEsdeRomDir = esdeRomDirectoryNow();
    return mEsdeRomDir;
}

std::string NanoMenu::esdeAppPathOf(const std::string& p) { return esdeAppPath(p); }

// A path from the card that names user storage (Nano runs as root: never a system folder).
bool NanoMenu::esdeStoragePath(const std::string& p) {
    if (p.find("..") != std::string::npos) return false;
    for (const char* pre : {"/storage/", "/mnt/media_rw/", "/mnt/shares/", "/data/media/0/", "/sdcard/", "content://"})
        if (p.compare(0, strlen(pre), pre) == 0) return true;
    return false;
}

std::string NanoMenu::esdeRomDirectoryNow() {
    std::string roms = esdeSettingsGet("ROMDirectory");
    if (roms.compare(0, 1, "~") == 0) roms = esdeDataDir() + roms.substr(1);
    while (roms.size() > 1 && roms.back() == '/') roms.pop_back();
    if (!esdeStoragePath(roms + "/")) roms.clear();
    struct stat st;
    auto isDir = [&](const std::string& d) { return !d.empty() && d[0] == '/' && stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode); };
    if (isDir(roms)) return roms;
    const std::string app = esdeAppPath(roms), vol = esdeVolume(app), mine = esdeVolume(esdeDataDir());
    if (!vol.empty() && !mine.empty() && vol != mine && isDir(mine + app.substr(vol.size()))) return mine + app.substr(vol.size());
    const std::string dir = esdeDataDir();
    roms = dir.substr(0, dir.find_last_of('/')) + "/ROMs";
    return isDir(roms) ? roms : std::string();
}

// The folders %ROMPATH% can be: ES-DE's ROM folder, then the parents of every system folder (ES-DE's
// ROM folder holds a folder per system), the ones on the ES-DE folder's storage first.
std::vector<std::string> NanoMenu::esdeRomPathCandidates() {
    std::vector<std::string> same, other;
    const std::string romDir = esdeRomDirectory();
    if (!romDir.empty()) same.push_back(romDir);
    const std::string vol = esdeVolume(esdeDataDir());
    for (int s = 0; s < (int)mXmbSystems.size(); s++)
        for (const auto& r : esdeSystemRoots(s)) {
            const size_t sl = r.find_last_of('/');
            if (sl == std::string::npos || sl == 0) continue;
            const std::string parent = r.substr(0, sl);
            if (sameFile(esdeAppPath(parent), esdeAppPath(romDir))) continue;
            auto& v = esdeVolume(esdeAppPath(parent)) == vol ? same : other;
            if (std::find(v.begin(), v.end(), parent) == v.end()) v.push_back(parent);
        }
    same.insert(same.end(), other.begin(), other.end());
    return same;
}

// A collection line for a game: %ROMPATH%/<system>/<game> when it is in ES-DE's ROM folder (without one, a
// ROMs folder on the ES-DE folder's storage is taken to be it), as ES-DE writes it, else the path ES-DE
// sees.
std::string NanoMenu::esdeCollectionLine(const std::string& rom, const std::string& romDir, const std::string& esdeDir) {
    const std::string app = esdeAppPath(rom);
    std::string low = esdeLower(app);
    if (!romDir.empty()) {
        const std::string rd = esdeLower(esdeAppPath(romDir)) + "/";
        if (low.compare(0, rd.size(), rd) == 0) return "%ROMPATH%/" + app.substr(rd.size());
        return app;
    }
    const std::string esdeVol = esdeVolume(esdeAppPath(esdeDir));
    const size_t at = low.find("/roms/", esdeVol.size());
    if (!esdeVol.empty() && at != std::string::npos && esdeVolume(app) == esdeVol) return "%ROMPATH%/" + app.substr(at + 6);
    return app;
}

// A collection's file name: its name without what ES-DE does not accept in one (it skips such a
// file) or a FAT file name cannot hold.
std::string NanoMenu::esdeCollectionFileName(const std::string& name) {
    std::string out;
    for (char c : name) out += strchr("*\",./:;<>\\|\n\t\r?,", c) ? '_' : c;   // ',' separates CollectionSystemsCustom
    return out;
}

// The collections es_settings.xml lists as enabled (CollectionSystemsCustom), as ES-DE shows them;
// another custom-*.cfg stays on the card untouched.
static std::vector<std::string> esdeSplitList(const std::string& v) {
    std::vector<std::string> out;
    for (size_t p = 0; p <= v.size();) {
        size_t q = v.find(',', p);
        if (q == std::string::npos) q = v.size();
        if (q > p) out.push_back(v.substr(p, q - p));
        p = q + 1;
    }
    return out;
}

void NanoMenu::esdeStoreLoadCollections() {
    // Those already shown keep their places (open lists point at them by index), new ones after them.
    std::vector<std::string> before;
    for (const auto& c : mXmbCollections) before.push_back(c.name);
    mXmbCollections.clear();
    mEsdeCollectionFiles.clear();
    mEsdeDirCase.clear();
    esdeRomCaseRefresh();
    if (!esdeStoreReady()) { mEsdeCollCache.clear(); return; }
    const std::string dir = esdeDataDir() + "/collections";
    std::vector<std::string> names;
    for (const auto& n : esdeSplitList(esdeSettingsGet("CollectionSystemsCustom")))   // a name ES-DE takes, nothing else
        if (!n.empty() && esdeCollectionFileName(n) == n && access((dir + "/custom-" + n + ".cfg").c_str(), F_OK) == 0
            && std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    auto rank = [&](const std::string& n) {
        const auto it = std::find(before.begin(), before.end(), n);
        return it == before.end() ? before.size() : (size_t)(it - before.begin());
    };
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
        const size_t ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    // A file is resolved again when it changes, or the folders its games can be in do.
    const std::vector<std::string> romRoots = esdeRomPathCandidates();
    std::string rootsKey;
    for (const auto& r : romRoots) rootsKey += r + "\n";
    for (int s = 0; s < (int)mXmbSystems.size(); s++) rootsKey += std::to_string(mXmbSystems[s].roms.size()) + ",";
    for (const auto& name : names) {
        const std::string path = dir + "/custom-" + name + ".cfg";
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        const long long mtime = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        EsdeCollCache& cc = mEsdeCollCache[name];
        if (!cc.parsed || cc.mtime != mtime || cc.size != (long long)st.st_size || cc.roots != rootsKey) {
            std::string text;
            if (!esdeReadFile(path, &text)) continue;
            cc = EsdeCollCache();
            cc.parsed = true;
            cc.mtime = mtime;
            cc.size = (long long)st.st_size;
            cc.roots = rootsKey;
            cc.c.name = name;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                std::string line = text.substr(pos, nl - pos);
                pos = nl + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                std::string abs;
                if (line.compare(0, 10, "%ROMPATH%/") == 0) {
                    for (const auto& r : romRoots)
                        if (!(abs = esdeFindGame(r, line.substr(10))).empty()) break;
                } else if (line[0] == '/') {
                    const size_t sl = line.find_last_of('/');
                    abs = esdeFindGame(sl == 0 ? std::string() : line.substr(0, sl), line.substr(sl + 1));
                }
                if (abs.empty()) {
                    cc.c.esdeUnresolved.push_back(line);   // kept as it is (a card that is out)
                } else if (cc.c.esdeLines.emplace(abs, line).second) {
                    cc.c.roms.push_back(abs);
                }
            }
        }
        mEsdeCollectionFiles.insert(name);
        cc.c.esdeRead = cc.c.roms;
        mXmbCollections.push_back(cc.c);
    }
}

void NanoMenu::esdeStoreSaveCollections() {
    if (!esdeStoreReady()) {
        photoShowBanner(trDyn("The microSD card with your ES-DE folder is not in. The change was not saved."));
        return;
    }
    const std::string dir = esdeDataDir() + "/collections";
    const std::string romDir = esdeRomDirectory();
    std::set<std::string> now, nowLower;
    bool ok = true, clash = false;
    for (auto& c : mXmbCollections) {
        const std::string file = esdeCollectionFileName(c.name);
        if (file.empty()) continue;
        if (!nowLower.insert(esdeLower(file)).second) { clash = true; continue; }   // the card ignores case
        now.insert(file);
        const std::string path = dir + "/custom-" + file + ".cfg";
        // A rename that only changes case: the file keeps its old name unless it goes first.
        for (const auto& old : mEsdeCollectionFiles)
            if (old != file && esdeLower(old) == esdeLower(file) && rename((dir + "/custom-" + old + ".cfg").c_str(), path.c_str()) != 0)
                ok = false;
        // One Nano did not change stays as ES-DE wrote it; a changed one keeps each line it had.
        if (c.roms == c.esdeRead && access(path.c_str(), F_OK) == 0) continue;
        // A file of that name Nano did not read (a collection disabled in ES-DE): its lines stay.
        if (!mEsdeCollectionFiles.count(file) && c.esdeRead.empty() && c.esdeUnresolved.empty()) {
            std::string old;
            if (esdeReadFile(path, &old)) {
                for (size_t p = 0; p < old.size();) {
                    size_t nl = old.find('\n', p);
                    if (nl == std::string::npos) nl = old.size();
                    std::string l = old.substr(p, nl - p);
                    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                    p = nl + 1;
                    bool have = l.empty();
                    for (const auto& kv : c.esdeLines) have = have || kv.second == l;
                    if (!have) c.esdeUnresolved.push_back(l);
                }
            } else if (access(path.c_str(), F_OK) == 0) {
                ok = false;
                continue;
            }
        }
        std::string text;
        for (const auto& r : c.roms) {
            auto li = c.esdeLines.find(r);
            if (li == c.esdeLines.end()) li = c.esdeLines.emplace(r, esdeCollectionLine(r, romDir, esdeDataDir())).first;
            text += li->second + "\n";
        }
        for (const auto& l : c.esdeUnresolved) text += l + "\n";
        std::string cur;
        if (!(esdeReadFile(path, &cur) && cur == text) && !esdeWriteAtomic(path, text)) { ok = false; continue; }
        esdeNoteWrite(path);
        esdeNoteWrite(dir);   // the folder changed with it
        c.esdeRead = c.roms;
    }
    for (const auto& gone : mEsdeCollectionFiles)
        if (!nowLower.count(esdeLower(gone))) unlink((dir + "/custom-" + gone + ".cfg").c_str());
    esdeNoteWrite(dir);
    // ES-DE shows a custom collection only when it is listed as enabled: a new one is (one the user
    // turned off in ES-DE stays off), a removed or renamed one goes.
    std::string enabled = esdeSettingsGet("CollectionSystemsCustom");
    const std::vector<std::string> list = esdeSplitList(enabled);
    std::vector<std::string> next;
    for (const auto& n : list)
        if (now.count(n) || !mEsdeCollectionFiles.count(n)) next.push_back(n);
    for (const auto& n : now)
        if (!mEsdeCollectionFiles.count(n) && std::find(next.begin(), next.end(), n) == next.end()) next.push_back(n);
    std::string joined;
    for (const auto& n : next) joined += (joined.empty() ? "" : ",") + n;
    if (joined != enabled) ok = esdeSettingsSet("CollectionSystemsCustom", joined) && ok;
    mEsdeCollectionFiles = now;
    if (clash) photoShowBanner(trDyn("Two collections have the same name; one was not saved."));
    else if (!ok) photoShowBanner(trDyn("Some ES-DE collections could not be saved."));
}

// ---------------------------------------------------------------------------
// es_settings.xml: flat <string|bool|int|float name="" value="" /> lines.
// ---------------------------------------------------------------------------

static std::string esdeXmlEscape(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

static std::string esdeXmlUnescape(const std::string& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '&') {
            static const std::pair<const char*, char> kEnt[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool hit = false;
            for (const auto& e : kEnt)
                if (v.compare(i, strlen(e.first), e.first) == 0) { o += e.second; i += strlen(e.first) - 1; hit = true; break; }
            if (hit) continue;
        }
        o += v[i];
    }
    return o;
}

// The line of setting `key` in es_settings.xml text, or npos.
static size_t esdeSettingLine(const std::string& text, const std::string& key, size_t* end) {
    const std::string needle = "name=\"" + key + "\"";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return std::string::npos;
    const size_t start = text.rfind('<', at);
    const size_t close = text.find('>', at);
    if (start == std::string::npos || close == std::string::npos) return std::string::npos;
    *end = close + 1;
    return start;
}

std::string NanoMenu::esdeSettingsGetIn(const std::string& file, const std::string& key) {
    std::string text;
    if (!esdeReadFile(file, &text)) return std::string();
    size_t end;
    const size_t start = esdeSettingLine(text, key, &end);
    if (start == std::string::npos) return std::string();
    const std::string tag = text.substr(start, end - start);
    const size_t v = tag.find("value=\"");
    if (v == std::string::npos) return std::string();
    const size_t q = tag.find('"', v + 7);
    return q == std::string::npos ? std::string() : esdeXmlUnescape(tag.substr(v + 7, q - v - 7));
}

bool NanoMenu::esdeSettingsSetIn(const std::string& file, const std::string& key, const std::string& value,
                                 const char* type) {
    std::string text;
    const bool have = esdeReadFile(file, &text);
    if (!have && access(file.c_str(), F_OK) == 0) return false;
    size_t end;
    const size_t start = esdeSettingLine(text, key, &end);
    std::string tag = type;
    if (start != std::string::npos) {   // ES-DE reads a setting only under its own type
        const size_t sp = text.find_first_of(" \t\n", start + 1);
        if (sp != std::string::npos && sp < end) tag = text.substr(start + 1, sp - start - 1);
    }
    const std::string line = "<" + tag + " name=\"" + key + "\" value=\"" + esdeXmlEscape(value) + "\" />";
    if (start != std::string::npos) {
        if (text.compare(start, end - start, line) == 0) return true;
        text.replace(start, end - start, line);
    } else {
        // Before a closing root when there is one (ES-DE reads a future <settings> root), else last.
        const size_t root = text.rfind("</settings>");
        if (root != std::string::npos) text.insert(root, line + "\n");
        else {
            if (!text.empty() && text.back() != '\n') text += "\n";
            if (text.empty()) text = "<?xml version=\"1.0\"?>\n";
            text += line + "\n";
        }
    }
    return esdeWriteAtomic(file, text);
}

std::string NanoMenu::esdeSettingsGet(const std::string& key) {
    return esdeSettingsGetIn(esdeDataDir() + "/settings/es_settings.xml", key);
}

bool NanoMenu::esdeSettingsSet(const std::string& key, const std::string& value, const char* type) {
    const std::string file = esdeDataDir() + "/settings/es_settings.xml";
    const bool ok = esdeSettingsSetIn(file, key, value, type);
    esdeNoteWrite(file);
    return ok;
}

// ---------------------------------------------------------------------------
// Play stats, scraped metadata and titles.
// ---------------------------------------------------------------------------

static std::string esdeNowIso() {
    const time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    char b[32];
    strftime(b, sizeof(b), "%Y%m%dT%H%M%S", &lt);
    return b;
}

// A game started: a line in kEsdePlayedFile, not a gamelist rewrite before the game can start (the
// home may exit then). esdeStorePlayedTick writes them to the gamelists once the card is there.
static const char* const kEsdePlayedFile = "/data/system/nano_esde_played.txt";

void NanoMenu::esdeGamelistPlayed(const std::string& rom) {
    if (!esdeStoreMode()) return;
    const std::string line = rom + "\t" + esdeNowIso() + "\n";
    const int fd = open(kEsdePlayedFile, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) { ALOGW("esde: %s cannot be written", kEsdePlayedFile); return; }
    if (write(fd, line.data(), line.size()) != (ssize_t)line.size()) ALOGW("esde: %s not written", kEsdePlayedFile);
    close(fd);
}

// The games started since: their play counts and times. Taken by renaming the file, so of the two Nano
// processes only one counts them. A game in no system folder yet (its system is still being read or
// scanned) waits in kEsdePlayedWaitFile and is tried again after the next reload (mEsdePlayedRetry).
static const char* const kEsdePlayedWaitFile = "/data/system/nano_esde_played.wait";

void NanoMenu::esdeStorePlayedTick() {
    const bool retry = mEsdePlayedRetry && access(kEsdePlayedWaitFile, F_OK) == 0;
    if ((!retry && access(kEsdePlayedFile, F_OK) != 0) || !esdeStoreReady()) return;
    mEsdePlayedRetry = false;
    std::string text;
    for (const char* file : {kEsdePlayedWaitFile, kEsdePlayedFile}) {   // the older first
        if (file == kEsdePlayedWaitFile && !retry) continue;
        const std::string mine = std::string(file) + "." + std::to_string(getpid());
        if (rename(file, mine.c_str()) != 0) continue;
        std::string t;
        esdeReadFile(mine, &t);
        unlink(mine.c_str());
        text += t;
        if (!text.empty() && text.back() != '\n') text += '\n';
    }
    const time_t old = time(nullptr) - 30 * 86400;   // a game still in no folder after a month is not kept
    struct tm lt;
    localtime_r(&old, &lt);
    char cutoff[32];
    strftime(cutoff, sizeof(cutoff), "%Y%m%dT%H%M%S", &lt);
    std::string waiting;
    for (size_t p = 0; p < text.size();) {
        size_t nl = text.find('\n', p);
        if (nl == std::string::npos) nl = text.size();
        const std::string l = text.substr(p, nl - p);
        p = nl + 1;
        const size_t tab = l.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const std::string rom = l.substr(0, tab), when = l.substr(tab + 1);
        std::string gl, rel;
        if (!esdeRomRef(rom, &gl, &rel, nullptr)) {
            if (when >= cutoff) waiting += l + "\n";
            continue;
        }
        esdeGamelistSet(rom, "playcount", kGamelistInc);
        esdeGamelistSet(rom, "lastplayed", std::string(1, kGamelistIfLater) + when);
    }
    if (!waiting.empty()) {
        const int fd = open(kEsdePlayedWaitFile, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0 || write(fd, waiting.data(), waiting.size()) != (ssize_t)waiting.size())
            ALOGW("esde: %s not written", kEsdePlayedWaitFile);
        if (fd >= 0) close(fd);
    }
    esdeGamelistFlush();   // before anything reads <lastplayed> (Recently Played)
}

void NanoMenu::esdeGamelistRemove(const std::string& rom) {
    if (!esdeStoreMode()) return;
    esdeGamelistSet(rom, kGamelistRemove, "1");
    esdeGamelistFlush();
}

// ES-DE's rating is 0 to 1; ScreenScraper's is out of 20 ("16/20" or "16").
std::string NanoMenu::esdeGamelistRating(const std::string& r) {
    if (r.empty()) return std::string();
    char* end = nullptr;
    double v = strtod(r.c_str(), &end);
    if (end == r.c_str()) return std::string();
    if (*end == '/') {
        const double of = strtod(end + 1, nullptr);
        if (of <= 0) return std::string();
        v /= of;
    } else if (v > 1.0) {
        v /= 20.0;
    }
    v = std::max(0.0, std::min(1.0, v));
    char b[16];
    snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

// YYYY-MM-DD (or YYYY, YYYY-MM) -> ES-DE's YYYYMMDDT000000.
std::string NanoMenu::esdeGamelistDate(const std::string& d) {
    std::string digits;
    for (char c : d) if (isdigit((unsigned char)c)) digits += c;
    if (digits.size() < 4) return std::string();
    while (digits.size() < 8) digits += digits.size() == 4 || digits.size() == 6 ? "01" : "0";
    return digits.substr(0, 8) + "T000000";
}

void NanoMenu::esdeGamelistPushScrape(const std::string& rom, const ScrapeEntry& e) {
    if (!esdeStoreMode()) return;
    const std::string keep = e.metaForce ? "" : std::string(1, kGamelistIfEmpty);
    const bool fresh = mEsdeGlDueMs == 0;
    auto set = [&](const char* k, const std::string& v) { if (!v.empty()) esdeGamelistSet(rom, k, keep + v); };
    if (!romNameOverrideFor(rom)) set("name", e.title);   // a title the user typed stays
    set("desc", e.synopsis);
    set("rating", esdeGamelistRating(e.rating));
    set("releasedate", esdeGamelistDate(e.releaseDate));
    set("developer", e.developer);
    set("publisher", e.publisher);
    set("genre", e.genre);
    set("players", e.players);
    // A scrape's results come one after another: written every few seconds, not per game.
    if (fresh && mEsdeGlDueMs != 0) mEsdeGlDueMs = (int64_t)uptimeMillis() + 5000;
}

// A title typed, changed or reverted in Nano is the game's <name> (reverted: the scraped one).
void NanoMenu::esdeStoreSaveNames() {
    if (!esdeStoreMode()) return;
    for (const auto& kv : mRomNameOverride) {
        auto it = mEsdeStoreNames.find(kv.first);
        if (it == mEsdeStoreNames.end() || it->second != kv.second) esdeGamelistSet(kv.first, "name", kv.second);
    }
    for (const auto& kv : mEsdeStoreNames) {
        if (mRomNameOverride.count(kv.first)) continue;
        const ScrapeEntry* se = scrapeEntryFor(kv.first);
        esdeGamelistSet(kv.first, "name", se ? se->title : std::string());
    }
    mEsdeStoreNames = mRomNameOverride;
}

// The card going out and coming back, or another card: everything read from it is read again.
void NanoMenu::esdeStoreTick() {
    if (!esdeStoreMode() || mFileJobKind != 0) return;
    if (!mEsdeFollowPending && (int64_t)uptimeMillis() - mEsdePlayedCheckMs >= 1000) {
        mEsdePlayedCheckMs = (int64_t)uptimeMillis();
        if (!property_get_bool("sys.gammaos.nano.app_launched", false)) esdeStorePlayedTick();
    }
    if (mEsdeFollowPending) {   // another card's ES-DE folder: everything from it
        mEsdeFollowPending = false;
        esdeStoreReload(true);
        return;
    }
    if (mEsdeAdoptLater && esdeStoreReady() && !mEsdeSystemsAdopted) {   // the card's systems changed elsewhere
        mEsdeAdoptLater = false;
        esdeStoreRefresh(kSwSystems);
    }
    if (mEsdeApplyAfterLoad && esdeStoreReady() && !mBgScanThreadRunning && !mEsdeSystemsAdopted) {
        mEsdeApplyAfterLoad = false;   // the card's systems are loaded (or a scan was in the way): their emulators
        esdeStoreRefresh(kSwGamelist);
        esdeAddCardSystems();          // the ES-DE ROM folder's systems the card's list lacks
    }
    const bool ok = esdeStoreReady();
    if (ok && mEsdeNamesNeedLoad) {   // the titles were asked for before the card was in
        mEsdeNamesNeedLoad = false;
        const EsdePlaces places = esdePlacesTake();
        mRomNameOverrideLoaded = true;
        loadRomNameOverrides();
        for (auto& sys : mXmbSystems) applyRomNameOverrides(sys);
        esdePlacesRestore(places);
    }
    if (!mEsdeStoreSeen) {   // the first frame: read it now only if it was not there at startup
        mEsdeStoreSeen = true;
        mEsdeStoreWasOk = ok;
        mEsdeStoreId = ok ? esdeStoreIdentity() : std::string();
        if (ok && !mEsdeFlagsLoadedReady) esdeStoreReload(false);
        else if (ok) esdeStoreRefresh(kSwGamelist | kSwSettings | kSwSystems);   // what startup did not read
        return;
    }
    if (ok && ok == mEsdeStoreWasOk && mEsdeReloadAfterScan && !mBgScanThreadRunning && !mBgScanResultReady
        && !mBgScanAgain) {   // the systems esdeAddCardSystems added are scanned: their games are known now
        mEsdeReloadAfterScan = false;
        esdeStoreRefresh(kSwGamelist | kSwCollections);
        return;
    }
    if (ok == mEsdeStoreWasOk) {
        if (ok) esdeStoreSweep();
        return;
    }
    mEsdeStoreWasOk = ok;
    ALOGI("esde: ES-DE folder %s %s", esdeDataDir().c_str(), ok ? "available" : "gone");
    esdeStoreReload(ok);
}

// Everything read from the ES-DE folder, read again (the card came back, another card, or Nano moved
// there). rescan: games hidden while it was out come back to the lists, another card's games show.
void NanoMenu::esdeStoreReload(bool rescan) {
    const EsdePlaces places = esdePlacesTake();
    mEsdeStoreCheckedMs = 0;
    const bool ok = esdeStoreReady();
    if (ok) {
        esdeGamelistFlush();   // edits made just before are in what is read
        esdeMkdirs(esdeDataDir() + "/downloaded_media");   // where the media lookups and downloads go
    } else {
        mEsdeGlPending.clear();
        mEsdeGlDueMs = 0;
    }
    mEsdeStoreWasOk = ok;
    mEsdeStoreSeen = true;
    mEsdeStoreId = ok ? esdeStoreIdentity() : std::string();
    mEsdeRomDirMs = 0;
    mEsdeFlagsCache.clear();
    mEsdeCollCache.clear();
    mEsdeRootsCache.clear();
    mEsdeGamelistStamp.clear();
    mEsdeMediaPath.clear();
    mEsdeSweep.clear();
    if (ok) {
        esdeStoreAdoptSystems();
        esdeStoreApplySettings();
    }
    loadHiddenGames();
    loadFavorites();
    loadCollections();
    mRomNameOverrideLoaded = true;
    loadRomNameOverrides();
    if (!ok) mRomNamesLoadErr = true;   // nothing to save over until it is back
    scraperRefreshDir();
    if (ok) scraperReloadFromDisk();
    esdeStoreLoadGameEmus();
    esdeStoreApplySystemEmus();
    for (auto& sys : mXmbSystems) applyRomNameOverrides(sys);
    esdeStoreRebuildRecents();
    esdePlacesRestore(places);
    mEsdePlayedRetry = true;
    if (ok && rescan) {
        forceRescanAllSystems();
        mEsdeReloadAfterScan = true;   // its games' favourites, titles and collections, once they are known
    }
    mPs3CatsStale = true;
    mDisplayDirty = true;
}

// ---------------------------------------------------------------------------
// Changes made elsewhere: the ES-DE folder's files looked at every few seconds.
// ---------------------------------------------------------------------------

// The games directly in a system folder (the files with its extensions, and folders), as one number:
// a save or screenshot written beside the games changes the folder but not this.
static long long esdeGamesSig(const std::string& root, const std::string& exts) {
    long long sig = 0;
    DIR* d = opendir(esdeRawPath(root).c_str());
    if (!d) return -1;
    const std::string want = "," + esdeLower(exts) + ",";
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const size_t dot = n.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string() : esdeLower(n.substr(dot));
        // Any extension: not the saves, states and screenshots an emulator writes beside its games.
        static const std::string kNotGames = ",.srm,.sav,.rtc,.mcr,.auto,.png,.jpg,.jpeg,.bmp,.txt,.nfo,.xml,.cfg,.opt,.log,";
        const bool game = e->d_type == DT_DIR
                          || (exts.empty() ? n[0] != '.' && ext.compare(0, 6, ".state") != 0
                                                 && kNotGames.find("," + ext + ",") == std::string::npos
                                           : !ext.empty() && want.find("," + ext + ",") != std::string::npos);
        if (game) sig += (long long)std::hash<std::string>()(esdeLower(n));
    }
    closedir(d);
    return sig;
}

void NanoMenu::esdeStoreSweepBuild() {
    mEsdeSweep.clear();
    const std::string dir = esdeDataDir();
    auto add = [&](const std::string& p, int kind) {
        for (const auto& it : mEsdeSweep) if (it.path == p) return;
        mEsdeSweep.push_back({p, kind, esdeStamp(p)});
    };
    mEsdeSweepRoots.clear();
    std::set<std::string> folders;
    for (int s = 0; s < (int)mXmbSystems.size(); s++)
        for (const auto& r : esdeSystemRoots(s)) {
            mEsdeSweepRoots += r + "\n";
            folders.insert(esdeRootFolder(r, s));
            const size_t n = mEsdeSweep.size();
            add(r, kSwRoms);   // a game added or deleted there
            if (mEsdeSweep.size() > n) {
                mEsdeSweep.back().exts = mXmbSystems[s].acceptExts;
                mEsdeSweep.back().games = esdeGamesSig(r, mXmbSystems[s].acceptExts);
            }
        }
    add(dir + "/gamelists", kSwGamelist);
    for (const auto& f : folders) if (!f.empty()) add(dir + "/gamelists/" + f + "/gamelist.xml", kSwGamelist);
    add(dir + "/collections", kSwCollections);
    if (DIR* d = opendir(esdeRawPath(dir + "/collections").c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 11 && n.compare(0, 7, "custom-") == 0 && n.compare(n.size() - 4, 4, ".cfg") == 0)
                add(dir + "/collections/" + n, kSwCollections);
        }
        closedir(d);
    }
    add(dir + "/settings/es_settings.xml", kSwSettings);
    add(dir + "/custom_systems/es_systems.xml", kSwSystems);
    add(dir + "/nano/systems.json", kSwSystems);
    add(dir + "/nano/names.json", kSwNames);
    add(dir + "/nano/emulators.json", kSwGamelist);
}

// A file Nano wrote: not a change made elsewhere. A new one in a folder looked at is looked at too.
void NanoMenu::esdeNoteWrite(const std::string& path) {
    for (auto& it : mEsdeSweep)
        if (it.path == path) { it.stamp = esdeStamp(path); return; }
    const std::string parent = path.substr(0, path.find_last_of('/'));
    for (const auto& it : mEsdeSweep)
        if (it.path == parent && it.kind != kSwRoms) { mEsdeSweep.push_back({path, it.kind, esdeStamp(path)}); return; }
}

// Every two seconds while the home is up (not under a game): another card, or a file changed.
void NanoMenu::esdeStoreSweep() {
    const int64_t now = (int64_t)uptimeMillis();
    if (now - mEsdeSweepMs < 2000) return;
    mEsdeSweepMs = now;
    if (mEsdeGlDueMs != 0 || property_get_bool("sys.gammaos.nano.app_launched", false)) return;
    const std::string id = esdeStoreIdentity();
    if (id != mEsdeStoreId) {
        ALOGI("esde: the ES-DE folder is now %s", esdeDataDir().c_str());
        esdeStoreReload(true);
        return;
    }
    std::string roots;
    for (int s = 0; s < (int)mXmbSystems.size(); s++)
        for (const auto& r : esdeSystemRoots(s)) roots += r + "\n";
    if (mEsdeSweep.empty() || roots != mEsdeSweepRoots) {
        const bool first = mEsdeSweep.empty();
        esdeStoreSweepBuild();
        if (!first) esdeStoreRefresh(kSwGamelist | kSwCollections);   // the games' folders changed: found again
        return;
    }
    int kinds = 0;
    for (auto& it : mEsdeSweep) {
        const long long st = esdeStamp(it.path);
        if (st == it.stamp) continue;
        it.stamp = st;
        if (it.kind == kSwRoms) {   // only games added or deleted count
            const long long g = esdeGamesSig(it.path, it.exts);
            if (g == it.games) continue;
            it.games = g;
        }
        kinds |= it.kind;
    }
    if (!kinds) return;
    ALOGI("esde: the ES-DE folder changed (%d); reading it again", kinds);
    esdeStoreSweepBuild();   // files that appeared
    esdeStoreRefresh(kinds);
}

// What changed, read again, and the lists showing it rebuilt.
void NanoMenu::esdeStoreRefresh(int kinds) {
    if (!esdeStoreReady()) return;
    esdeGamelistFlush();
    const EsdePlaces places = esdePlacesTake();
    if (kinds & kSwSystems) {
        esdeStoreAdoptSystems();
        esdeStoreWriteCustomSystems();
    }
    if (kinds & kSwSettings) {
        esdeStoreApplySettings();
        kinds |= kSwCollections;   // CollectionSystemsCustom
    }
    if (kinds & kSwRoms) {
        forceRescanAllSystems();
        mEsdeReloadAfterScan = true;
    }
    bool shown = false;   // shown again: the lists no longer hold it, a scan finds it
    if (kinds & (kSwGamelist | kSwNames)) {
        const std::map<std::string, std::string> hiddenBefore = mHiddenGames;
        loadHiddenGames();
        loadFavorites();
        mRomNameOverrideLoaded = true;
        loadRomNameOverrides();
        esdeStoreLoadGameEmus();
        esdeStoreApplySystemEmus();
        mEsdeGamelistStamp.clear();   // the ES-DE theme reads them again
        for (auto& sys : mXmbSystems) applyRomNameOverrides(sys);
        esdeStoreRebuildRecents();
        for (const auto& kv : hiddenBefore) if (!mHiddenGames.count(kv.first)) shown = true;
    }
    if (kinds & kSwCollections) loadCollections();
    esdePlacesRestore(places);
    if (shown) forceRescanAllSystems();
    mEsdePlayedRetry = true;
    FrameFlag::wake();
}

// Where each open list, option menu and the ES-DE game list are, by game: a reload re-sorts the
// lists, and they point into them by index.
std::string NanoMenu::esdePlaceKey(const Ps3Item& it) const {
    switch (it.kind) {
        case PS3_ROM:
            return it.a >= 0 && it.a < (int)mXmbSystems.size() && it.b >= 0 && it.b < (int)mXmbSystems[it.a].roms.size()
                       ? "r:" + mXmbSystems[it.a].roms[it.b] : std::string();
        case PS3_RECENT:
            return it.a >= 0 && it.a < (int)mXmbRecent.size() ? "p:" + mXmbRecent[it.a].romPath : std::string();
        case PS3_COLLECTION:
            return it.a >= 0 && it.a < (int)mXmbCollections.size() ? "c:" + mXmbCollections[it.a].name : std::string();
        case PS3_COLLECTION_NEW: return "n:";
        default: return std::string();
    }
}

NanoMenu::EsdePlaces NanoMenu::esdePlacesTake() {
    EsdePlaces p;
    for (const auto& lvl : mPs3Stack) {
        p.level.push_back(lvl.sel >= 0 && lvl.sel < (int)lvl.items.size() ? esdePlaceKey(lvl.items[lvl.sel]) : std::string());
        p.levelColl.push_back(lvl.collectionIdx >= 0 && lvl.collectionIdx < (int)mXmbCollections.size()
                                  ? mXmbCollections[lvl.collectionIdx].name : std::string());
    }
    for (const auto& s : mXmbSystems) p.roms[s.id] = s.roms;
    if (mPs3OptCtxKind == PS3_COLLECTION && mPs3OptCtxA >= 0 && mPs3OptCtxA < (int)mXmbCollections.size())
        p.ctxColl = mXmbCollections[mPs3OptCtxA].name;
    if (mPs3OptCtxKind == PS3_RECENT && mPs3OptCtxA >= 0 && mPs3OptCtxA < (int)mXmbRecent.size())
        p.ctxRecent = mXmbRecent[mPs3OptCtxA].romPath;
    if (mPs3OptCtxKind == PS3_ROM && mPs3OptCtxA >= 0 && mPs3OptCtxA < (int)mXmbSystems.size() && mPs3OptCtxB >= 0
        && mPs3OptCtxB < (int)mXmbSystems[mPs3OptCtxA].roms.size()) {
        p.ctxSys = mXmbSystems[mPs3OptCtxA].id;
        p.ctx = mXmbSystems[mPs3OptCtxA].roms[mPs3OptCtxB];
    }
    if (mEsdeSysSel >= 0 && mEsdeSysSel < (int)mEsdeSysList.size()) {
        const int o = mEsdeSysList[mEsdeSysSel];
        if (o >= 0 && o < (int)mXmbSystems.size() && mEsdeGameSel >= 0 && mEsdeGameSel < (int)mXmbSystems[o].roms.size()) {
            p.esdeSys = mXmbSystems[o].id;
            p.esde = mXmbSystems[o].roms[mEsdeGameSel];
        }
    }
    return p;
}

// The lists rebuilt after a reload, each back on the game it was on (one that went: as near as it was).
void NanoMenu::esdePlacesRestore(const EsdePlaces& p) {
    auto collIdx = [this](const std::string& name) {
        for (size_t i = 0; i < mXmbCollections.size(); i++) if (mXmbCollections[i].name == name) return (int)i;
        return -1;
    };
    for (int s = 0; s < (int)mXmbSystems.size(); s++) {
        const XmbSystem& sys = mXmbSystems[s];
        auto before = p.roms.find(sys.id);
        if (before != p.roms.end() && before->second == sys.roms) continue;
        rebuildOpenRomLevels(s, std::string());
        if (before == p.roms.end()) continue;
        std::unordered_map<std::string, int> now;
        for (size_t i = 0; i < sys.roms.size(); i++) now.emplace(sys.roms[i], (int)i);
        auto remap = [&](int& idx) {
            if (idx < 0 || idx >= (int)before->second.size()) return;
            auto it = now.find(before->second[idx]);
            idx = it == now.end() ? -1 : it->second;
        };
        for (auto& gr : mGSearchResults) if (gr.kind == PS3_ROM && gr.a == s) remap(gr.b);
        for (auto& sr : mSearchResults) if (sr.sysIdx == s) remap(sr.gameIdx);
    }
    applyRomNameOverridesToRecents();
    for (size_t li = 0; li < mPs3Stack.size() && li < p.levelColl.size(); li++) {
        if (p.levelColl[li].empty()) continue;
        const int c = collIdx(p.levelColl[li]);
        if (c < 0) { mPs3Stack.resize(li); break; }   // a collection that went closes with what is over it
        mPs3Stack[li].collectionIdx = c;
    }
    refreshListsAfterHide();   // the collection, Favourites and Recently Played lists
    for (auto& lvl : mPs3Stack) {
        if (lvl.collectionIdx >= 0 || lvl.title != "Collections") continue;
        const int keep = lvl.sel;
        buildCollectionsSubmenu(lvl);
        lvl.sel = std::max(0, std::min(keep, (int)lvl.items.size() - 1));
    }
    for (size_t li = 0; li < mPs3Stack.size() && li < p.level.size(); li++) {
        if (p.level[li].empty()) continue;
        Ps3Level& lvl = mPs3Stack[li];
        for (int i = 0; i < (int)lvl.items.size(); i++)
            if (esdePlaceKey(lvl.items[i]) == p.level[li]) { lvl.sel = i; break; }
    }
    if (!p.ctxColl.empty() && mPs3OptCtxKind == PS3_COLLECTION) {
        mPs3OptCtxA = collIdx(p.ctxColl);
        if (mPs3OptCtxA < 0) mPs3OptActive = false;
    }
    if (!p.ctxRecent.empty() && mPs3OptCtxKind == PS3_RECENT) {
        mPs3OptCtxA = -1;
        for (size_t i = 0; i < mXmbRecent.size(); i++)
            if (mXmbRecent[i].romPath == p.ctxRecent) { mPs3OptCtxA = (int)i; break; }
        if (mPs3OptCtxA < 0) mPs3OptActive = false;
    }
    auto find = [this](const std::string& sysId, const std::string& rom, int* s, int* r) {
        for (int i = 0; i < (int)mXmbSystems.size(); i++) {
            if (mXmbSystems[i].id != sysId) continue;
            const auto& roms = mXmbSystems[i].roms;
            const auto it = std::find(roms.begin(), roms.end(), rom);
            *s = i;
            *r = it == roms.end() ? -1 : (int)(it - roms.begin());
            return;
        }
        *s = *r = -1;
    };
    if (!p.ctx.empty()) {
        int s, r;
        find(p.ctxSys, p.ctx, &s, &r);
        if (s >= 0) { mPs3OptCtxA = s; mPs3OptCtxB = r; }
        if (s < 0 || r < 0) mPs3OptActive = false;   // the game left the list: its menu closes
    }
    if (!p.esde.empty()) {
        int s, r;
        find(p.esdeSys, p.esde, &s, &r);
        if (s >= 0 && r >= 0) mEsdeGameSel = r;
        else if (s >= 0) mEsdeGameSel = std::min(mEsdeGameSel, std::max(0, (int)mXmbSystems[s].roms.size() - 1));
    }
    mPs3CatsStale = true;
    mDisplayDirty = true;
}

// ---------------------------------------------------------------------------
// Game emulators (<altemulator>, else nano/emulators.json) and Recently Played (<lastplayed>).
// ---------------------------------------------------------------------------

static const char* const kEsdeGameEmusFile = "/nano/emulators.json";

void NanoMenu::esdeStoreLoadGameEmus() {
    if (!esdeStoreMode()) return;
    mGameEmusLoaded = true;
    mGameEmus.clear();
    mGameEmusLoadErr = !esdeStoreReady();   // out: nothing is saved over
    if (mGameEmusLoadErr) return;
    const std::set<std::string> installed = emuInstalledPackages();
    esdeRomCaseRefresh();
    for (const auto& kv : mEsdeGlAltEmu) {
        const std::string rom = esdeCanonRom(kv.first);
        if (rom.empty()) continue;
        const std::string cmds = esdeCommandSystem(kv.second.folder, kv.second.sys);
        int cat = esdeCatalogForLabel(kv.second.sys, cmds, kv.second.label, installed);
        // OA-38: a label for an emulator that cannot run here is still the game's pick; its launch
        // falls back to this platform's default (emuApplyDefault).
        if (cat < 0) cat = esdeCatalogForLabel(kv.second.sys, cmds, kv.second.label, installed, false);
        if (cat >= 0) mGameEmus[gameEmuKey(rom)] = GameEmulator{emuEntryLabel(cat), mEmuCatalog[cat].amArgs};
    }
    std::string text;
    njson::Value root;
    const njson::Value* items = nullptr;
    if (esdeReadFile(esdeDataDir() + kEsdeGameEmusFile, &text) && njson::parse(text, &root) && root.isObject())
        items = root.find("items");
    if (!items || !items->isArray()) return;
    loadEmuCatalog();
    for (const auto& it : items->arr) {
        if (!it.isObject()) continue;
        const std::string game = it.getString("game"), am = it.getString("am");
        const size_t sl = game.find('/');
        if (sl == std::string::npos || am.empty()) continue;
        // Only an emulator of Nano's catalog: anything can write to the card, and games start as root.
        int cat = -1;
        for (int i = 0; i < (int)mEmuCatalog.size() && cat < 0; i++) if (mEmuCatalog[i].amArgs == am) cat = i;
        if (cat < 0) { ALOGW("esde: %s's emulator in %s is not one Nano knows; left out", game.c_str(), kEsdeGameEmusFile); continue; }
        auto fr = mEsdeFolderRoots.find(game.substr(0, sl));
        if (fr == mEsdeFolderRoots.end()) continue;
        for (const auto& r : fr->second) {
            const std::string rom = esdeFindGame(r, game.substr(sl + 1));
            if (rom.empty()) continue;
            if (!mGameEmus.count(gameEmuKey(rom))) mGameEmus[gameEmuKey(rom)] = GameEmulator{emuEntryLabel(cat), am};
            break;
        }
    }
}

// A game's emulator chosen in Nano: its <altemulator> when ES-DE has a command for it, else
// nano/emulators.json (by ES-DE folder and path, so another card with the same layout finds it).
void NanoMenu::esdeStoreSaveGameEmu(const std::string& rom, const GameEmulator* emu) {
    std::string rel, folder;
    if (!esdeRomRef(rom, nullptr, &rel, &folder)) {
        photoShowBanner(trDyn("This game is in no system folder, so the ES-DE folder cannot keep that."));
        return;
    }
    std::string label;
    if (emu) {
        XmbSystem g;
        EmuCatEntry e;
        e.amArgs = emu->amArgs;
        applyEmuEntryToSystem(g, e);
        int s = -1;
        collectionResolveRom(rom, &s, nullptr);
        label = esdeEmulatorLabel(esdeCommandSystem(folder, s), g);
    }
    esdeGamelistSet(rom, "altemulator", label);
    if (label.empty()) mEsdeGlAltEmu.erase(esdeKey(rom));
    else mEsdeGlAltEmu[esdeKey(rom)] = {label, folder, -1};
    if (!esdeStoreReady()) return;
    const std::string path = esdeDataDir() + kEsdeGameEmusFile, game = folder + "/" + rel;
    std::string text;
    njson::Value root;
    struct stat est;
    const bool exists = stat(path.c_str(), &est) == 0 || errno != ENOENT;
    if (exists && (!esdeReadFile(path, &text) || !njson::parse(text, &root) || !root.isObject())) {
        ALOGW("esde: %s unreadable; not overwriting it", path.c_str());
        return;
    }
    if (!root.isObject()) root = njson::Value::makeObject();
    njson::Value items = njson::Value::makeArray();
    bool changed = false;
    if (const njson::Value* old = root.find("items"))
        if (old->isArray())
            for (const auto& it : old->arr) {
                if (it.isObject() && it.getString("game") == game) { changed = true; continue; }
                items.arr.push_back(it);
            }
    if (emu && label.empty()) {
        njson::Value o = njson::Value::makeObject();
        o.set("game") = njson::Value::makeString(game);
        o.set("label") = njson::Value::makeString(emu->label);
        o.set("am") = njson::Value::makeString(emu->amArgs);
        items.arr.push_back(std::move(o));
        changed = true;
    }
    if (!changed) return;
    root.set("version") = njson::Value::makeNumber(1);
    root.set("items") = std::move(items);
    if (!esdeWriteAtomic(path, njson::serialize(root, true))) ALOGW("esde: writing %s failed", path.c_str());
    esdeNoteWrite(path);
}

// Recently Played as the gamelists have it: the games with the latest <lastplayed> (a game started
// from ES-DE too), each with the emulator it starts with now.
void NanoMenu::esdeStoreRebuildRecents() {
    if (!esdeStoreReady() || mXmbRecentMax <= 0) return;
    esdeRomCaseRefresh();
    std::vector<const EsdePlayed*> order;
    for (const auto& p : mEsdeGlPlayed) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [](const EsdePlayed* a, const EsdePlayed* b) { return a->when > b->when; });
    std::vector<XmbRecentEntry> next;
    std::unordered_set<std::string> seen;
    for (const EsdePlayed* p : order) {
        if ((int)next.size() >= mXmbRecentMax) break;
        for (const auto& path : p->paths) {
            const std::string rom = esdeCanonRom(path);
            int s = -1, r = -1;
            if (rom.empty() || !collectionResolveRom(rom, &s, &r)) continue;
            if (seen.insert(hiddenKey(rom)).second) next.push_back(recentEntryFor(s, r));
            break;
        }
    }
    bool same = next.size() == mXmbRecent.size();
    for (size_t i = 0; same && i < next.size(); i++)
        same = next[i].romPath == mXmbRecent[i].romPath && next[i].coreSo == mXmbRecent[i].coreSo
               && next[i].launchPkg == mXmbRecent[i].launchPkg && next[i].launchIntent == mXmbRecent[i].launchIntent
               && next[i].displayName == mXmbRecent[i].displayName;
    if (same) return;
    std::vector<std::string> selPath(mPs3Stack.size());   // each Recently Played list stays on its game
    for (size_t li = 0; li < mPs3Stack.size(); li++) {
        const Ps3Level& lvl = mPs3Stack[li];
        if (lvl.sel >= 0 && lvl.sel < (int)lvl.items.size()) selPath[li] = esdePlaceKey(lvl.items[lvl.sel]);
    }
    mXmbRecent = std::move(next);
    saveXmbRecent();
    for (size_t li = 0; li < mPs3Stack.size(); li++) {
        Ps3Level& lvl = mPs3Stack[li];
        if (lvl.items.empty() || lvl.items[0].kind != PS3_RECENT) continue;
        const int keep = lvl.sel;
        buildRecentSubmenu(lvl);
        lvl.sel = std::max(0, std::min(keep, (int)lvl.items.size() - 1));
        for (int i = 0; i < (int)lvl.items.size(); i++)
            if (esdePlaceKey(lvl.items[i]) == selPath[li]) { lvl.sel = i; break; }
    }
    mPs3CatsStale = true;
}

// ---------------------------------------------------------------------------
// Systems in folders ES-DE has no system for: custom_systems/es_systems.xml.
// ---------------------------------------------------------------------------

// A system whose folder in ES-DE's ROM folder is not an ES-DE system (a catalog platform's folder:
// coleco, 3ds) gets an entry with the commands of the ES-DE system it is, so ES-DE lists its games
// too. Entries already there (ES-DE's user's, or Nano's from before) are left as they are.
void NanoMenu::esdeStoreWriteCustomSystems() {
    if (!esdeStoreReady()) return;
    const std::string romDir = esdeRomDirectory();
    if (romDir.empty()) return;
    std::vector<std::pair<std::string, int>> want;   // folder, system
    for (int s = 0; s < (int)mXmbSystems.size(); s++) {
        for (const auto& r : esdeSystemRoots(s)) {
            const size_t sl = r.find_last_of('/');
            if (sl == std::string::npos || !sameFile(esdeAppPath(r.substr(0, sl)), esdeAppPath(romDir))) continue;
            const std::string base = r.substr(sl + 1);
            if (esdesys::esdeKnownSystem(base) || esdesys::esdeKnownSystem(esdeLower(base))) continue;
            const XmbSystem& sys = mXmbSystems[s];
            if (!esdesys::esdeKnownSystem(esdesys::esdePlatformSystemName(sys.romDir, sys.shortname))) continue;
            bool dup = false;
            for (const auto& w : want) dup = dup || esdeLower(w.first) == esdeLower(base);
            if (!dup) want.push_back({base, s});
        }
    }
    if (want.empty()) return;
    const std::string path = esdeDataDir() + "/custom_systems/es_systems.xml";
    tinyxml2::XMLDocument doc;
    const bool exists = access(path.c_str(), F_OK) == 0;
    if (exists && doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS) {
        ALOGW("esde: %s cannot be read; left as it is", path.c_str());
        return;
    }
    tinyxml2::XMLElement* list = doc.FirstChildElement("systemList");
    if (!list) {
        if (exists) return;
        doc.InsertEndChild(doc.NewDeclaration());
        list = doc.NewElement("systemList");
        doc.InsertEndChild(list);
    }
    std::set<std::string> have;
    for (tinyxml2::XMLElement* e = list->FirstChildElement("system"); e; e = e->NextSiblingElement("system"))
        if (const tinyxml2::XMLElement* n = e->FirstChildElement("name"))
            if (n->GetText()) have.insert(esdeLower(n->GetText()));
    bool added = false;
    for (const auto& w : want) {
        if (have.count(esdeLower(w.first))) continue;
        const XmbSystem& sys = mXmbSystems[w.second];
        const std::string esde = esdesys::esdePlatformSystemName(sys.romDir, sys.shortname);
        std::string ext, platform, theme;
        if (!esdesys::esdeSystemDef(esde, &ext, &platform, &theme)) continue;
        tinyxml2::XMLElement* e = doc.NewElement("system");
        auto child = [&](const char* k, const std::string& v) {
            tinyxml2::XMLElement* c = doc.NewElement(k);
            c->SetText(v.c_str());
            e->InsertEndChild(c);
            return c;
        };
        child("name", w.first);
        child("fullname", sys.name.empty() ? esdesys::esdeSystemFullName(esde) : sys.name);
        child("path", "%ROMPATH%/" + w.first);
        child("extension", ext);
        for (const auto& c : esdesys::esdeSystemCommands(esde)) {
            tinyxml2::XMLElement* ce = child("command", c.command);
            if (!c.label.empty()) ce->SetAttribute("label", c.label.c_str());
        }
        child("platform", platform);
        child("theme", theme);
        list->InsertEndChild(e);
        added = true;
        ALOGI("esde: custom system %s (as %s) for ES-DE", w.first.c_str(), esde.c_str());
    }
    if (!added) return;
    tinyxml2::XMLPrinter printer;
    doc.Print(&printer);
    if (!esdeWriteAtomic(path, std::string(printer.CStr(), printer.CStrSize() > 0 ? printer.CStrSize() - 1 : 0)))
        ALOGW("esde: writing %s failed", path.c_str());
    esdeNoteWrite(path);
}

// ---------------------------------------------------------------------------
// Settings ES-DE has too, in es_settings.xml (the scraper accounts never go there).
// ---------------------------------------------------------------------------

static const struct EsdeSettingProp {
    const char* prop;
    const char* key;
    const char* type;     // es_settings.xml tag
    const char* def;      // Nano's default
    const char* values;   // the values Nano takes ("" = true/false)
} kEsdeSettingProps[] = {
    {"persist.gammaos.scraper.engine", "Scraper", "string", "screenscraper", "screenscraper,thegamesdb"},
    {"persist.gammaos.scraper.region", "ScraperRegion", "string", "us", "us,eu,jp,wor"},
    {"persist.gammaos.scraper.overwrite", "ScraperOverwriteData", "bool", "false", ""},
    {"persist.gammaos.scraper.media.screenshot", "ScrapeScreenshots", "bool", "false", ""},
    {"persist.gammaos.scraper.media.titlescreen", "ScrapeTitleScreens", "bool", "false", ""},
    {"persist.gammaos.scraper.media.backcover", "ScrapeBackCovers", "bool", "false", ""},
    {"persist.gammaos.scraper.media.3dbox", "Scrape3DBoxes", "bool", "false", ""},
    {"persist.gammaos.scraper.media.physicalmedia", "ScrapePhysicalMedia", "bool", "false", ""},
    {"persist.gammaos.scraper.media.marquee", "ScrapeMarquees", "bool", "false", ""},
    {"persist.gammaos.scraper.media.miximage", "MiximageGenerate", "bool", "false", ""},
    {"persist.gammaos.scraper.media.manual", "ScrapeManuals", "bool", "false", ""},
    {"persist.gammaos.scraper.media.video", "ScrapeVideos", "bool", "false", ""},
};

static bool esdeSettingValueOk(const EsdeSettingProp& p, const std::string& v) {
    const std::string values = p.values[0] ? p.values : "true,false";
    for (const auto& ok : esdeSplitList(values)) if (ok == v) return true;
    return false;
}

// The card's settings become Nano's (a setting the file does not have, or Nano does not take, stays),
// and ES-DE's ROM folder follows a card swapped for another laid out the same.
void NanoMenu::esdeStoreApplySettings() {
    if (!esdeStoreReady()) return;
    const std::string file = esdeDataDir() + "/settings/es_settings.xml";
    std::string text;
    if (esdeReadFile(file, &text)) {
        for (const auto& p : kEsdeSettingProps) {
            size_t end;
            if (esdeSettingLine(text, p.key, &end) == std::string::npos) continue;
            const std::string v = esdeSettingsGetIn(file, p.key);
            if (!esdeSettingValueOk(p, v)) continue;
            char cur[PROPERTY_VALUE_MAX] = {};
            property_get(p.prop, cur, p.def);
            if (v != cur) {
                property_set(p.prop, v.c_str());
                ALOGI("esde: %s = %s (es_settings.xml %s)", p.prop, v.c_str(), p.key);
            }
        }
    }
    // ES-DE for Android asks for its ROM folder itself; the setting is for ES-DE elsewhere: filled in
    // only when it has none (another value, ~/ROMs or %ESPATH%/ROMs, is ES-DE's own).
    mEsdeRomDirMs = 0;
    const std::string romDir = esdeRomDirectory();
    if (!romDir.empty() && esdeSettingsGet("ROMDirectory").empty() && esdeAppPath(romDir) == romDir)
        esdeSettingsSet("ROMDirectory", romDir);
}

// Nano's values of the settings ES-DE has too: es_settings.xml key -> value, tag.
std::vector<std::pair<std::string, std::pair<std::string, const char*>>> NanoMenu::esdeSettingsFromProps() {
    std::vector<std::pair<std::string, std::pair<std::string, const char*>>> out;
    for (const auto& p : kEsdeSettingProps) {
        char v[PROPERTY_VALUE_MAX] = {};
        property_get(p.prop, v, p.def);
        out.push_back({p.key, {v, p.type}});
    }
    return out;
}

// A setting changed in Nano goes to es_settings.xml too.
void NanoMenu::esdeStoreSettingChanged(const std::string& prop) {
    if (!esdeStoreReady()) return;
    for (const auto& p : kEsdeSettingProps) {
        if (prop != p.prop) continue;
        char v[PROPERTY_VALUE_MAX] = {};
        property_get(p.prop, v, p.def);
        if (!esdeSettingsSet(p.key, v, p.type)) ALOGW("esde: es_settings.xml %s not written", p.key);
        return;
    }
}

}  // namespace android
