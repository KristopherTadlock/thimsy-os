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

// Game Settings > Move ES-DE to microSD and Move Emulator Files to microSD. Each asks first, then
// moves the files to the card behind the progress modal (fileCopyStart, kind 3 and 4): every file is
// copied unless the card already has it, the program is set to use the card, and the internal
// originals are deleted. One the card already had stays as it is; the internal copy goes to
// Internal storage/Nano Backup, and the user then picks Keep Files, Delete Internal Copies or
// Overwrite on SD Card (kind 5). From then on the card is the only copy.
// Setup runs the same moves without asking (mSdMoveAuto, NanoBiosUi.cpp): only an emulator or an
// ES-DE folder that has files is moved, the card's copy wins a clash and the internal one is deleted.
//
//   <card>/ES-DE                         the ES-DE folder (NanoEsdeStore.cpp): ES-DE's own files,
//                                        Nano's favourites/hidden games/titles/metadata in its
//                                        gamelists, its collections, its covers and fan art
//   <card>/emulator_config/<emulator>    each emulator's settings, saves and states. Dolphin and hakuX
//                                        have no setting for their folders: links to the card replace them.

#define LOG_TAG "NanoMenu"

#include "NanoMenu.h"
#include "NanoAppFiles.h"
#include "NanoBiosUtil.h"
#include "NanoEmuSync.h"
#include "NanoEsdeSystemName.h"
#include "NanoI18n.h"
#include "NanoJson.h"
#include "NanoPlatform.h"
#include "NanoScraper.h"
#include "NanoScrapeQueue.h"
#include "NanoSdMove.h"
#include "NanoMenuUtils.h"
#include "NanoRaConfig.h"

#include <cutils/properties.h>
#include <utils/SystemClock.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/wait.h>
#include <tinyxml2.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

namespace android {

static const char* const kBackupRoot = appdef::kBackupRoot;
static const char* const kEsdeInternal = appdef::kEsdeInternal;
static const char* const kEsdeStage = "/data/system/nano_esde_stage";
static const char* const kEsdeStageMerged = "/data/system/nano_esde_stage_merged";
static const char* const kEsdeCardOrig = "/data/system/nano_esde_card_orig";   // what kEsdeStageMerged replaces

// The files under dir, relative to it.
static void sdListFiles(const std::string& dir, const std::string& rel, std::vector<std::string>* out) {
    DIR* d = opendir((rel.empty() ? dir : dir + "/" + rel).c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st;
        if (lstat((dir + "/" + r).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) sdListFiles(dir, r, out);
        else if (S_ISREG(st.st_mode)) out->push_back(r);
    }
    closedir(d);
}
static const char* const kBiosManifestPath = "/data/system/nano_bios/installed.json";   // NanoBiosInstall.cpp
static const char* const kEmuSdProp = "persist.gammaos.nano.emu_sd";   // emulator files kept on the card: "1"
static const char* const kEmuSdCardProp = "persist.gammaos.nano.emu_sd_card";   // the card adopted last (its id)
static const char* const kEmuSdSkipProp = "sys.gammaos.nano.emu_sd_skip";   // a card left alone this boot (Not Now)
// A card whose adoption move failed this boot: not adopted again until the next.
static const char* const kEmuSdAdoptFailedProp = "sys.gammaos.nano.emu_sd_adopt_failed";
// The emulator groups the look after a game failed to move this boot ("1,4"), "all" when its move failed.
static const char* const kEmuSdFailedProp = "sys.gammaos.nano.emu_sd_failed";
// A card adopted with Keep This Device's: its files never synced go to the card until all have been.
static const char* const kEmuSdAppWinsProp = "persist.gammaos.nano.emu_sd_appwins";
// Held (exclusive) by a move of emulator files, shared by a settings sync: never both at once.
static const char* const kEmuMoveLock = "/data/system/nano_emu_move.lock";
static const char* const kEsdePkg = "org.es_de.frontend";
static const char* const kPpssppPkg = "org.ppsspp.ppsspp";
static const char* const kPpssppMemstickFile = appdef::kPpssppMemstickFile;
static const char* const kFlycastPkg = "com.flycast.emulator";
static const char* const kFlycastPrefs = appdef::kFlycastPrefs;
static const char* const kFlycastDefHome = appdef::kFlycastDefHome;
static const char* const kDrasticNanoDefDir = "/storage/emulated/0/drastic-nano";
static const char* const kDrasticPkg = "com.dsemu.drastic";
static const char* const kDrasticFsPrefs = appdef::kDrasticFsPrefs;
static const char* const kDrasticDefDir = appdef::kDrasticDefDir;
static const char* const kMupenPkg = "org.mupen64plusae.v3.fzurita";
static const char* const kMupenPrefs = appdef::kMupenPrefs;
static const char* const kMupenInternalData = "/data/data/org.mupen64plusae.v3.fzurita/files/GameData";
static const char* const kArmsx2Pkg = "com.armsx2";
static const char* const kArmsx2Prefs = appdef::kArmsx2Prefs;
static const char* const kScummvmPkg = "org.scummvm.scummvm";
static const char* const kWatermelonPkg = "me.magnum.melondualds";
static const char* const kScummvmIni = appdef::kScummvmIni;
static const char* const kScummvmDefSaves = appdef::kScummvmDefSaves;
static const char* const kVita3kPkg = "org.vita3k.emulator";
static const char* const kVita3kConfig = appdef::kVita3kConfig;
static const char* const kVita3kDefPref = appdef::kVita3kDefPref;
static const char* const kDolphinPkg = "org.dolphinemu.dolphinemu";
static const char* const kDolphinUserDir = "/data/media/0/Android/data/org.dolphinemu.dolphinemu/files";
static const char* const kAzaharPkg = "org.azahar_emu.azahar";
static const char* const kAzaharPrefs = appdef::kAzaharPrefs;
static const char* const kHakuxPkg = "com.rfandango.haku_x";
static const char* const kHakuxNative = "/data/data/com.rfandango.haku_x/files/native";
static const char* const kSeedlessPkg = "com.seedlessds.app";
static const char* const kSeedlessPrefs = appdef::kSeedlessPrefs;

enum { kGroupRetroArch = 0, kGroupDrasticNano, kGroupPpsspp, kGroupFlycast, kGroupEsde, kGroupDrastic, kGroupMupen,
       kGroupArmsx2, kGroupScummvm, kGroupVita3k, kGroupDolphin, kGroupAzahar, kGroupHakux, kGroupSeedless };

static const std::pair<int, const char*> kGroupPkg[] = {
    {kGroupRetroArch, "com.retroarch.aarch64"}, {kGroupPpsspp, kPpssppPkg}, {kGroupFlycast, kFlycastPkg},
    {kGroupDrastic, kDrasticPkg}, {kGroupMupen, kMupenPkg}, {kGroupArmsx2, kArmsx2Pkg},
    {kGroupScummvm, kScummvmPkg}, {kGroupVita3k, kVita3kPkg}, {kGroupDolphin, kDolphinPkg},
    {kGroupAzahar, kAzaharPkg}, {kGroupHakux, kHakuxPkg}, {kGroupSeedless, kSeedlessPkg}};

static const char* groupPkg(int group) {   // "" = built in (the DS emulator)
    for (const auto& gp : kGroupPkg) if (gp.first == group) return gp.second;
    return "";
}

bool nanoNetBridgeCmd(const char* verb, const std::string& arg);   // NanoMenuSettings.cpp

static void sdMkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if ((i == dir.size() || dir[i] == '/') && !nanoscraper::isVolumeRoot(dir.substr(0, i)))
            ::mkdir(dir.substr(0, i).c_str(), 0775);
}

static void sdRemoveTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(path.c_str())) {
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) sdRemoveTree(path + "/" + e->d_name);
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

// Removes the folders under dir (and dir) that hold nothing.
static bool sdPruneEmpty(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool empty = true;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        const std::string full = dir + "/" + e->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && sdPruneEmpty(full)) continue;
        empty = false;
    }
    closedir(d);
    return empty && rmdir(dir.c_str()) == 0;
}

bool sdReadFile(const std::string& path, std::string* out) {
    // Never through a link or from a FIFO (whose open would block); app files: appFileRead.
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < 64 * 1024 * 1024;
    if (ok) {
        out->resize(st.st_size);
        size_t got = 0;
        while (got < out->size()) {   // FUSE (a card through /storage) returns short reads
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

static bool sdWriteFile(const std::string& path, const std::string& text) {
    sdMkdirs(path.substr(0, path.find_last_of('/')));
    const std::string tmp = path + ".nanotmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

// An app has a process (its package name, or a :name process of it).
bool sdPkgRunning(const std::string& pkg) {
    DIR* d = opendir("/proc");
    if (!d) return false;
    bool on = false;
    while (dirent* e = on ? nullptr : readdir(d)) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char path[64];
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buf[256];
        const ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        on = strncmp(buf, pkg.c_str(), pkg.size()) == 0 && (buf[pkg.size()] == 0 || buf[pkg.size()] == ':');
    }
    closedir(d);
    return on;
}

// Stops an app that is running (one that is not needs nothing).
void sdStopIfRunning(const std::string& pkg) {
    if (sdPkgRunning(pkg)) runArgv({"/system/bin/am", "force-stop", pkg}, nullptr, 15000);
}

// Runs a program without a shell and waits up to timeoutMs (then kills it); its standard output
// into *out when asked (up to 16 MB). The child gets no descriptors but 0-2. The exit status, or -1.
int runArgv(const std::vector<std::string>& args, std::string* out, int timeoutMs) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    struct rlimit rl;   // read here: getrlimit is not async-signal-safe
    int maxFd = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
        maxFd = rl.rlim_cur == RLIM_INFINITY ? 65536 : (int)std::min<rlim_t>(rl.rlim_cur, 65536);
    int pfd[2] = {-1, -1};
    if (out && pipe2(pfd, O_CLOEXEC) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(pfd[0]); close(pfd[1]); }
        return -1;
    }
    if (pid == 0) {   // only async-signal-safe calls here
        // Not at the caller's priority: the render thread's real-time one stalls system_server.
        const sched_param sp = {};
        sched_setscheduler(0, SCHED_OTHER, &sp);
        setpriority(PRIO_PROCESS, 0, 0);
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
        dup2(out ? pfd[1] : nul, 1);
        long r = -1;
#ifdef __NR_close_range
        r = syscall(__NR_close_range, 3u, ~0u, 0u);
#endif
        if (r != 0) for (int fd = 3; fd < maxFd; fd++) close(fd);
        execv(argv[0], argv.data());
        _exit(127);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t end = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeoutMs;
    auto left = [&]() {
        struct timespec n;
        clock_gettime(CLOCK_MONOTONIC, &n);
        return std::max<int64_t>(0, end - ((int64_t)n.tv_sec * 1000 + n.tv_nsec / 1000000));
    };
    bool timedOut = false;
    if (out) {
        close(pfd[1]);
        char buf[8192];
        for (;;) {
            struct pollfd pf = {pfd[0], POLLIN, 0};
            const int64_t ms = left();
            const int pr = ms > 0 ? poll(&pf, 1, (int)std::min<int64_t>(ms, 1000)) : 0;
            if (pr < 0 && errno == EINTR) continue;
            if (pr == 0) { if (left() == 0) { timedOut = true; break; } continue; }
            if (pr < 0) break;
            const ssize_t n = read(pfd[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            if (out->size() < (16u << 20)) out->append(buf, (size_t)n);
        }
        close(pfd[0]);
    }
    int st = 0;
    for (;;) {
        const pid_t w = timedOut ? 0 : waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) return -1;
        if (timedOut || left() == 0) {   // too long: killed and reaped
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            ALOGW("runArgv: %s timed out", args.empty() ? "?" : args[0].c_str());
            return -1;
        }
        usleep(20000);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Replaces a file in an app's private folder: owned like its folder, the old file's mode, the
// app's SELinux label, never written through a link (nanobios::appFileWrite). The app is stopped
// first (it rewrites its settings when it closes).
bool sdWriteAppFile(const std::string& path, const std::string& text) {
    return nanobios::appFileWrite(path, text, [](const std::vector<std::string>& a, std::string* o, int t) { return runArgv(a, o, t); });
}

// An app's SharedPreferences file as the app would load it: its backup when there is one (the
// file itself was being written when the app stopped). Read as nanobios::appFileRead does.
bool sdReadPrefs(const std::string& path, std::string* out) { return nanobios::appPrefsRead(path, out); }

static std::string sdShown(const std::string& path) {   // a path as the user knows it
    const std::string emu = "/storage/emulated/0";
    if (path.compare(0, emu.size(), emu) == 0) return trDyn("Internal storage") + path.substr(emu.size());
    const std::string self = "/storage/self/primary";
    if (path.compare(0, self.size(), self) == 0) return trDyn("Internal storage") + path.substr(self.size());
    if (path.compare(0, 17, "/storage/emulated") == 0 || path.compare(0, 13, "/storage/self") == 0)
        return path;   // another user, or all of them
    if (path.compare(0, 9, "/storage/") == 0) {   // the volume's own folder shows as the card itself
        const size_t rest = path.find('/', 9);
        return trDyn("microSD card") + (rest == std::string::npos ? std::string() : path.substr(rest));
    }
    return path;
}

// A file anywhere under path (or path is one), but in its top-level skipDirs. Links are not followed
// below path. Stops at the first.
static bool sdHasFiles(const std::string& path, const std::vector<std::string>& skipDirs, int depth = 0) {
    struct stat st;
    if ((depth == 0 ? stat(path.c_str(), &st) : lstat(path.c_str(), &st)) != 0) return false;
    if (S_ISREG(st.st_mode)) return true;
    if (!S_ISDIR(st.st_mode) || depth > 16) return false;
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    bool any = false;
    while (!any) {
        dirent* e = readdir(d);
        if (!e) break;
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (depth == 0 && std::find(skipDirs.begin(), skipDirs.end(), n) != skipDirs.end()) continue;
        any = sdHasFiles(path + "/" + n, skipDirs, depth + 1);
    }
    closedir(d);
    return any;
}

// What a move says: a banner, or in setup (mSdMoveAuto) a line for its page.
void NanoMenu::sdMoveNotice(const std::string& msg) {
    if (msg.empty()) return;
    if (mSdMoveQuiet || mSdMoveQuietJob) { ALOGW("sdmove: %s", msg.c_str()); return; }   // the look after a game says nothing
    if (!mSdMoveAuto || mSdMoveAdopt) { photoShowBanner(msg); return; }
    if (mSdMoveAutoNote.find(msg) != std::string::npos) return;
    mSdMoveAutoNote += (mSdMoveAutoNote.empty() ? "" : "\n") + msg;
}

// A gamelist's <alternativeEmulator>: ES-DE writes it before <gameList> and reads it there or inside.
static tinyxml2::XMLElement* sdGamelistAltEmu(tinyxml2::XMLDocument& doc) {
    if (tinyxml2::XMLElement* e = doc.FirstChildElement("alternativeEmulator")) return e;
    tinyxml2::XMLElement* root = doc.FirstChildElement("gameList");
    return root ? root->FirstChildElement("alternativeEmulator") : nullptr;
}

static void sdGamelistPutAltEmu(tinyxml2::XMLDocument& doc, tinyxml2::XMLNode* ae) {
    tinyxml2::XMLNode* decl = doc.FirstChild() && doc.FirstChild()->ToDeclaration() ? doc.FirstChild() : nullptr;
    if (decl) doc.InsertAfterChild(decl, ae);
    else doc.InsertFirstChild(ae);
}

// The card is the microSD slot's (an MMC disk), not a USB drive.
static bool sdIsSlot(const std::string& storagePath) {
    return nanoplat::get().volumes->isSlot(storagePath);
}

// The microSD card: the first removable volume Nano can write to, as apps see it (/storage/XXXX-XXXX),
// the slot's before a USB drive's.
std::string NanoMenu::sdCardRoot() {
    return nanoplat::get().volumes->cardRoot();
}

// ---------------------------------------------------------------------------
// Move ES-DE to microSD
// ---------------------------------------------------------------------------

// The systems in ES-DE's ROM folder that Nano has none for (ES-DE lists every system folder holding
// games, and the card's gamelists name them, xbox for one): ES-DE's ROMDirectory when it is a folder,
// else ROMs beside the ES-DE folder.
void NanoMenu::esdeAddCardSystems() {
    if (!esdeStoreReady()) return;
    const std::string roms = esdeRomDirectory();
    if (!roms.empty()) gsAutoAddFromRoot(roms, true);
}

void NanoMenu::esdeMoveOffer() {
    // Over a running or paused game or app: its files would move under it.
    if (property_get_bool("sys.gammaos.nano.app_launched", false)) {
        photoShowBanner(trDyn("Close the game first."));
        return;
    }
    if (esdeStoreMode()) {   // moved before: the card's ES-DE folder read again, its systems added
        esdeStoreReload(false);
        esdeAddCardSystems();
        photoShowBanner(trDyn("ES-DE is already on the microSD card."));
        return;
    }
    const std::string sd = sdCardRoot();
    if (sd.empty()) { photoShowBanner(trDyn("Insert a microSD card first.")); return; }
    mSdMoveRoot = sd;
    // Short enough for a 640x480 screen's dialog.
    const std::string body = trDyn("Moves the ES-DE folder, gamelists, collections, favourites and game media to the "
                                   "microSD card. The card then holds the only copy. Nothing on it is overwritten.");
    openConfirmDialog(63, "Move ES-DE to microSD", body, "Move");
}

// Nano's data as ES-DE files in the staging folder, then the move. Render thread.
void NanoMenu::esdeMoveStart() {
    if (mScrapeRunning || !mScrapeMoveDst.empty() || mFileJobKind != 0 || !mBgScrapeRun.empty()
        || !nanoscraper::newestWaitingRun().empty()) {
        sdMoveNotice(trDyn("Another copy or scrape is running"));
        return;
    }
    const std::string sd = mSdMoveRoot;
    if (sd.empty() || access(sd.c_str(), W_OK) != 0) { sdMoveNotice(trDyn("Insert a microSD card first.")); return; }
    scraperEnsureLoaded();
    romNameOverrideFor(std::string());   // loads the title overrides
    if (mScrapeIndexLoadErr || mRomNamesLoadErr) {
        sdMoveNotice(trDyn("Nano's game data could not be read. Nothing was moved."));
        return;
    }
    esdeGamelistFlush();
    system("am force-stop org.es_de.frontend >/dev/null 2>&1");   // not writing its gamelists while they move
    const std::string card = sd + "/ES-DE";
    const std::string backup = std::string(kBackupRoot) + "/ES-DE";
    const std::string internal = kEsdeInternal;
    sdRemoveTree(kEsdeStage);
    sdRemoveTree(kEsdeStageMerged);
    sdMkdirs(kEsdeStage);
    std::vector<std::string> merged;   // under the internal ES-DE folder: their data is in the staged files
    mSdMoveUnmapped.clear();
    if (!mGameEmusLoaded) loadGameEmulators();
    // ES-DE's ROM folder once the card is the store: games are filed under its folders' names as they
    // will be then (esdeRootFolder), a folder ES-DE has no system for (md) under its own.
    std::string romDir = esdeSettingsGetIn(card + "/settings/es_settings.xml", "ROMDirectory");
    if (romDir.empty()) romDir = esdeSettingsGetIn(internal + "/settings/es_settings.xml", "ROMDirectory");
    if (romDir.empty() || access(romDir.c_str(), F_OK) != 0) {
        romDir = sd + "/ROMs";
        if (access(romDir.c_str(), F_OK) != 0) romDir.clear();
    }
    struct StagingRomDir {
        std::string& m;
        StagingRomDir(std::string& member, const std::string& v) : m(member) { m = v; }
        ~StagingRomDir() { m.clear(); }
    } stagingRomDir(mEsdeStagingRomDir, romDir.empty() ? std::string("/") : romDir);
    mEsdeRootsCache.clear();
    // A file the card has already is merged with it, entry by entry (the card's entries win), and
    // replaces it (kEsdeStageMerged); another goes beside what is there (kEsdeStage).
    auto onCard = [&](const std::string& rel) { return access((card + "/" + rel).c_str(), F_OK) == 0; };
    auto stage = [&](const std::string& rel, const std::string& text) {
        return sdWriteFile(std::string(onCard(rel) ? kEsdeStageMerged : kEsdeStage) + "/" + rel, text);
    };
    auto fail = [&](const char* msg) {
        sdMoveNotice(trDyn(msg));
        sdRemoveTree(kEsdeStage);
        sdRemoveTree(kEsdeStageMerged);
    };

    // Gamelists: the card's (if any), the internal one's entries it lacks, then Nano's favourites,
    // hidden games, titles, emulators, play times and metadata. Favourites and hidden games are
    // added; a value the card or ES-DE has stays.
    struct Game {
        bool fav = false, unfav = false, hidden = false;
        std::string name, emu, played;
        const ScrapeEntry* se = nullptr;
    };
    std::map<std::string, std::map<std::string, Game>> games;   // gamelist (relative to ES-DE/) -> rel -> data
    std::map<std::string, std::string> sysEmus;                  // gamelist -> its system's emulator
    auto at = [&](const std::string& rom) -> Game* {
        std::string gl, rel;
        if (!esdeRomRef(rom, &gl, &rel, nullptr)) return nullptr;
        return &games[gl.substr(internal.size() + 1)][rel];
    };
    for (const auto& r : mXmbFavorites) {
        if (Game* g = at(r)) g->fav = true; else mSdMoveUnmapped.push_back("favorite\t" + r);
    }
    for (const auto& r : mEsdeUnfavorites) if (Game* g = at(r)) g->unfav = true;
    for (const auto& kv : mHiddenGames) {
        if (Game* g = at(kv.second)) g->hidden = true; else mSdMoveUnmapped.push_back("hidden\t" + kv.second);
    }
    for (const auto& kv : mRomNameOverride) {
        if (Game* g = at(kv.first)) g->name = kv.second; else mSdMoveUnmapped.push_back("title\t" + kv.first + "\t" + kv.second);
    }
    for (const auto& kv : mScrapeIndex) if (Game* g = at(kv.first)) g->se = &kv.second;
    // A game's emulator ES-DE has a command for is its <altemulator>; another goes to nano/emulators.json.
    njson::Value emuItems = njson::Value::makeArray();
    for (const auto& kv : mGameEmus) {
        std::string rel, folder;
        if (!esdeRomRef(kv.first, nullptr, &rel, &folder)) { mSdMoveUnmapped.push_back("emulator\t" + kv.first + "\t" + kv.second.label); continue; }
        XmbSystem launch;
        EmuCatEntry e;
        e.amArgs = kv.second.amArgs;
        applyEmuEntryToSystem(launch, e);
        int s = -1;
        collectionResolveRom(kv.first, &s, nullptr);
        const std::string label = esdeEmulatorLabel(esdeCommandSystem(folder, s), launch);
        if (!label.empty()) { if (Game* g = at(kv.first)) g->emu = label; continue; }
        njson::Value o = njson::Value::makeObject();
        o.set("game") = njson::Value::makeString(folder + "/" + rel);
        o.set("label") = njson::Value::makeString(kv.second.label);
        o.set("am") = njson::Value::makeString(kv.second.amArgs);
        emuItems.arr.push_back(std::move(o));
    }
    // Recently Played, in its order, as play times (the gamelists' <lastplayed> is what it is read from).
    {
        const time_t now = time(nullptr);
        for (size_t i = 0; i < mXmbRecent.size(); i++) {
            Game* g = at(mXmbRecent[i].romPath);
            if (!g) continue;
            const time_t t = now - (time_t)i * 60;
            struct tm lt;
            localtime_r(&t, &lt);
            char b[32];
            strftime(b, sizeof(b), "%Y%m%dT%H%M%S", &lt);
            g->played = b;
        }
    }
    for (int s = 0; s < (int)mXmbSystems.size(); s++)
        for (const auto& f : esdeSystemFolders(s)) {
            const std::string cmds = esdeCommandSystem(f, s);
            const std::string label = esdeEmulatorLabel(cmds, mXmbSystems[s]);
            if (!label.empty() && label != esdesys::esdeDefaultLabel(cmds)) sysEmus.emplace("gamelists/" + f + "/gamelist.xml", label);
        }
    // The internal gamelists the card has too: merged with it, never one replacing the other.
    if (DIR* d = opendir((internal + "/gamelists").c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string rel = std::string("gamelists/") + e->d_name + "/gamelist.xml";
            if (onCard(rel) && access((internal + "/" + rel).c_str(), F_OK) == 0) games[rel];
        }
        closedir(d);
    }
    for (const auto& kv : sysEmus) games[kv.first];
    if (!mSdMoveUnmapped.empty()) {   // games in no system folder: kept for the user, not lost
        std::string text;
        for (const auto& l : mSdMoveUnmapped) text += l + "\n";
        if (!sdWriteFile(std::string(kEsdeStage) + "/nano/not-in-a-system-folder.txt", text)) {
            fail("Nano's game data could not be prepared. Nothing was moved.");
            return;
        }
    }
    auto pathOf = [](tinyxml2::XMLElement* g) {
        const tinyxml2::XMLElement* pe = g->FirstChildElement("path");
        std::string p = pe && pe->GetText() ? pe->GetText() : "";
        while (p.compare(0, 2, "./") == 0) p.erase(0, 2);
        return p;
    };
    for (const auto& file : games) {
        const std::string from = internal + "/" + file.first, there = card + "/" + file.first;
        const bool exists = access(from.c_str(), F_OK) == 0, cardHas = access(there.c_str(), F_OK) == 0;
        tinyxml2::XMLDocument doc, other;
        if ((cardHas && doc.LoadFile(there.c_str()) != tinyxml2::XML_SUCCESS)
            || (exists && (cardHas ? other : doc).LoadFile(from.c_str()) != tinyxml2::XML_SUCCESS)) {
            fail("An ES-DE gamelist could not be read. Nothing was moved.");
            ALOGW("sdmove: %s cannot be read", cardHas ? there.c_str() : from.c_str());
            return;
        }
        tinyxml2::XMLElement* root = doc.FirstChildElement("gameList");
        if (!root) {
            if (exists || cardHas) { fail("An ES-DE gamelist could not be read. Nothing was moved."); return; }
            doc.InsertEndChild(doc.NewDeclaration());
            root = doc.NewElement("gameList");
            doc.InsertEndChild(root);
        }
        // Games by path, and folders ("\x01" + path); the favourites the card's own entries have.
        std::unordered_map<std::string, tinyxml2::XMLElement*> byPath;
        std::unordered_set<std::string> cardFavs;
        auto keyOf = [&](tinyxml2::XMLElement* g) {
            const std::string p = pathOf(g);
            return p.empty() ? p : (strcmp(g->Name(), "folder") == 0 ? "\x01" : "") + p;
        };
        for (tinyxml2::XMLElement* g = root->FirstChildElement(); g; g = g->NextSiblingElement()) {
            if (strcmp(g->Name(), "game") != 0 && strcmp(g->Name(), "folder") != 0) continue;
            const std::string k = keyOf(g);
            if (k.empty()) continue;
            byPath.emplace(k, g);
            const tinyxml2::XMLElement* f = g->FirstChildElement("favorite");
            if (cardHas && f && f->GetText() && !strcmp(f->GetText(), "true")) cardFavs.insert(k);
        }
        // The internal entries the card's gamelist lacks, and the values it lacks of the others.
        if (cardHas && exists) {
            tinyxml2::XMLElement* oroot = other.FirstChildElement("gameList");
            if (!oroot) { fail("An ES-DE gamelist could not be read. Nothing was moved."); return; }
            if (tinyxml2::XMLElement* ae = sdGamelistAltEmu(other))
                if (!sdGamelistAltEmu(doc)) sdGamelistPutAltEmu(doc, ae->DeepClone(&doc));
            for (tinyxml2::XMLElement* g = oroot->FirstChildElement(); g; g = g->NextSiblingElement()) {
                if (strcmp(g->Name(), "game") != 0 && strcmp(g->Name(), "folder") != 0) continue;
                const std::string k = keyOf(g);
                auto it = byPath.find(k);
                if (it == byPath.end()) {
                    tinyxml2::XMLNode* copy = g->DeepClone(&doc);
                    root->InsertEndChild(copy);
                    if (!k.empty()) byPath.emplace(k, copy->ToElement());
                    continue;
                }
                for (tinyxml2::XMLElement* c = g->FirstChildElement(); c; c = c->NextSiblingElement())
                    if (!it->second->FirstChildElement(c->Name())) it->second->InsertEndChild(c->DeepClone(&doc));
            }
            // The internal values the card's differ from: kept in Nano Backup (setup deletes them).
            std::string orig;
            if (!mSdMoveAuto && !(sdReadFile(from, &orig) && sdWriteFile(backup + "/" + file.first, orig))) {
                fail("Nano's game data could not be prepared. Nothing was moved.");
                return;
            }
        }
        auto se = sysEmus.find(file.first);
        if (se != sysEmus.end() && !sdGamelistAltEmu(doc)) {
            tinyxml2::XMLElement* ae = doc.NewElement("alternativeEmulator");
            tinyxml2::XMLElement* l = doc.NewElement("label");
            l->SetText(se->second.c_str());
            ae->InsertEndChild(l);
            sdGamelistPutAltEmu(doc, ae);
        }
        for (const auto& kv : file.second) {
            const Game& d = kv.second;
            const ScrapeEntry* se = d.se;
            const bool anything = d.fav || d.unfav || d.hidden || !d.name.empty() || !d.emu.empty() || !d.played.empty() || se;
            if (!anything) continue;
            tinyxml2::XMLElement* g;
            auto it = byPath.find(kv.first);
            if (it != byPath.end()) {
                g = it->second;
            } else {
                if (!d.fav && !d.hidden && d.name.empty() && d.emu.empty() && d.played.empty() && !se) continue;
                g = doc.NewElement("game");
                tinyxml2::XMLElement* pe = doc.NewElement("path");
                pe->SetText(("./" + kv.first).c_str());
                g->InsertEndChild(pe);
                root->InsertEndChild(g);
            }
            auto set = [&](const char* k, const std::string& v, bool always) {
                if (v.empty()) return;
                tinyxml2::XMLElement* c = g->FirstChildElement(k);
                if (c && !always) return;
                if (!c) { c = doc.NewElement(k); g->InsertEndChild(c); }
                c->SetText(v.c_str());
            };
            if (d.fav) set("favorite", "true", true);
            else if (d.unfav && !cardFavs.count(kv.first))   // the internal one's favourite the user took back
                if (tinyxml2::XMLElement* c = g->FirstChildElement("favorite")) g->DeleteChild(c);
            if (d.hidden) set("hidden", "true", true);
            set("name", d.name, !cardHas);
            set("altemulator", d.emu, false);
            set("lastplayed", d.played, false);
            if (se) {
                set("name", se->title, false);
                set("desc", se->synopsis, false);
                set("rating", esdeGamelistRating(se->rating), false);
                set("releasedate", esdeGamelistDate(se->releaseDate), false);
                set("developer", se->developer, false);
                set("publisher", se->publisher, false);
                set("genre", se->genre, false);
                set("players", se->players, false);
            }
        }
        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        if (!stage(file.first, std::string(printer.CStr(), printer.CStrSize() > 0 ? printer.CStrSize() - 1 : 0))) {
            fail("Nano's game data could not be prepared. Nothing was moved.");
            return;
        }
        if (exists) merged.push_back(file.first);
    }

    // Collections: Nano's, joined with an ES-DE collection of the same name on the card and inside.
    std::vector<std::string> collectionNames;
    std::map<std::string, std::vector<std::string>> collections;   // file -> Nano's games
    for (const auto& c : mXmbCollections) {
        const std::string file = esdeCollectionFileName(c.name);
        if (!file.empty()) collections[file] = c.roms;
    }
    if (DIR* d = opendir((internal + "/collections").c_str())) {   // the card has it too: merged
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 11 && n.compare(0, 7, "custom-") == 0 && n.compare(n.size() - 4, 4, ".cfg") == 0
                && onCard("collections/" + n))
                collections[n.substr(7, n.size() - 11)];
        }
        closedir(d);
    }
    for (const auto& c : collections) {
        const std::string rel = "collections/custom-" + c.first + ".cfg";
        std::vector<std::string> lines;
        auto addLines = [&](const std::string& text) {
            for (size_t p = 0; p < text.size();) {
                size_t nl = text.find('\n', p);
                if (nl == std::string::npos) nl = text.size();
                std::string l = text.substr(p, nl - p);
                while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                if (!l.empty() && std::find(lines.begin(), lines.end(), l) == lines.end()) lines.push_back(l);
                p = nl + 1;
            }
        };
        std::string old;
        if (sdReadFile(card + "/" + rel, &old)) addLines(old);
        else if (onCard(rel)) { fail("Nano's game data could not be prepared. Nothing was moved."); return; }
        if (sdReadFile(internal + "/" + rel, &old)) {
            merged.push_back(rel);
            addLines(old);
        }
        for (const auto& r : c.second) {
            const std::string line = esdeCollectionLine(r, romDir, card);
            if (std::find(lines.begin(), lines.end(), line) == lines.end()) lines.push_back(line);
        }
        std::string text;
        for (const auto& l : lines) text += l + "\n";
        if (!stage(rel, text)) {
            fail("Nano's game data could not be prepared. Nothing was moved.");
            return;
        }
        if (mXmbCollections.end() != std::find_if(mXmbCollections.begin(), mXmbCollections.end(),
                                                  [&](const XmbCollection& x) { return esdeCollectionFileName(x.name) == c.first; }))
            collectionNames.push_back(c.first);
    }

    // es_settings.xml: the card's, else the internal one with the media in the ES-DE folder (no
    // MediaDirectory elsewhere); the collections enabled, and the ROM folder and Nano's scraper
    // settings where it has none.
    {
        const std::string rel = "settings/es_settings.xml";
        const bool cardHas = onCard(rel);
        const std::string staged = std::string(cardHas ? kEsdeStageMerged : kEsdeStage) + "/" + rel;
        std::string text;
        const bool have = sdReadFile((cardHas ? card : internal) + "/" + rel, &text);
        if ((cardHas && !have) || (have && !sdWriteFile(staged, text))) {
            fail("Nano's game data could not be prepared. Nothing was moved.");
            return;
        }
        if (have && !cardHas) merged.push_back(rel);   // else the internal copy clashes, to Nano Backup
        if (have && !cardHas && !esdeSettingsGetIn(staged, "MediaDirectory").empty()) esdeSettingsSetIn(staged, "MediaDirectory", "");
        if (!collectionNames.empty()) {
            std::string enabled = esdeSettingsGetIn(staged, "CollectionSystemsCustom");
            for (const auto& n : collectionNames) {
                bool listed = false;
                for (size_t p = 0; p <= enabled.size();) {
                    size_t q = enabled.find(',', p);
                    if (q == std::string::npos) q = enabled.size();
                    if (enabled.compare(p, q - p, n) == 0 && q - p == n.size()) { listed = true; break; }
                    p = q + 1;
                }
                if (!listed) enabled += (enabled.empty() ? "" : ",") + n;
            }
            esdeSettingsSetIn(staged, "CollectionSystemsCustom", enabled);
        }
        if (!romDir.empty() && esdeSettingsGetIn(staged, "ROMDirectory").empty()) esdeSettingsSetIn(staged, "ROMDirectory", romDir);
        for (const auto& kv : esdeSettingsFromProps())
            if (esdeSettingsGetIn(staged, kv.first).empty()) esdeSettingsSetIn(staged, kv.first, kv.second.first, kv.second.second);
    }

    // The media folders: ES-DE's own MediaDirectory and Nano's ES-DE Media Folder, wherever they
    // are, all to downloaded_media on the card (the default one moves with the ES-DE folder).
    std::vector<std::string> mediaDirs;
    std::vector<std::string> skipDirs;   // ... the ones inside the ES-DE folder, out of its own move
    {
        std::vector<std::string> named;
        std::string esdeMedia = esdeSettingsGetIn(internal + "/settings/es_settings.xml", "MediaDirectory");
        if (esdeMedia.compare(0, 1, "~") == 0) esdeMedia = internal + esdeMedia.substr(1);   // ~ is the ES-DE folder on Android
        named.push_back(esdeMedia);
        char media[PROPERTY_VALUE_MAX] = {};
        property_get("persist.gammaos.nano.esde.mediadir", media, "");
        named.push_back(media);
        const std::string ci = sdCanon(internal);
        for (std::string m : named) {
            while (m.size() > 1 && m.back() == '/') m.pop_back();
            if (m.size() < 2 || m[0] != '/' || sdCanon(m) == ci + "/downloaded_media") continue;
            if (m.compare(0, card.size() + 1, card + "/") == 0 || m == card) continue;
            if (sdTooBroad(m, false)) {   // it would go along; its setting would not
                char msg[600];
                snprintf(msg, sizeof(msg), trDyn("The media folder %s holds more than ES-DE media, so nothing was moved. "
                                                 "Choose a folder of its own for the media first."), sdShown(m).c_str());
                sdRemoveTree(kEsdeStage);
                sdRemoveTree(kEsdeStageMerged);
                if (mSdMoveAuto) { sdMoveNotice(msg); return; }
                feInfoDialog("Move ES-DE to microSD", msg);
                mPs3DlgKeepOpen = true;   // opened from the confirm dialog's accept
                return;
            }
            const std::string cm = sdCanon(m);
            bool dup = false;
            for (size_t k = 0; k < mediaDirs.size() && !dup; k++) {
                const std::string ck = sdCanon(mediaDirs[k]);
                if (sdUnder(cm, ck)) dup = true;                                         // in one already listed
                else if (sdUnder(ck, cm)) { mediaDirs.erase(mediaDirs.begin() + k); k--; }   // holds one listed
            }
            if (dup) continue;
            if (sdUnder(ci, cm)) continue;   // holds the ES-DE folder (caught above, kept for safety)
            mediaDirs.push_back(m);
            if (sdUnder(cm, ci) && cm != ci) skipDirs.push_back(cm.substr(ci.size() + 1));
        }
    }

    // Covers and fan art: Nano's (named by a hash of the game's path) to covers/ and fanart/ under
    // the game's ES-DE name, the index pointing at them on the card. ES-DE's own cover of a game
    // (any image type) is the one kept; Nano's then clashes with it.
    std::vector<std::string> artRoots = {internal + "/downloaded_media", card + "/downloaded_media"};
    for (const auto& m : mediaDirs) artRoots.push_back(m);
    std::vector<FileCopyPair> pairs;
    std::unordered_map<std::string, ScrapeEntry> index = mScrapeIndex;
    std::set<std::string> artDst;
    const std::string cache = mScrapeCacheDir + "/";
    for (auto& kv : index) {
        std::vector<std::string> sysDirs, names;
        std::string rel, sys;
        if (!esdeMediaLayout(kv.first, -1, &sysDirs, &names, nullptr) || names.empty() || sysDirs.empty()) continue;
        if (!esdeRomRef(kv.first, nullptr, &rel, &sys)) sys = sysDirs[0];   // as esdeMediaStems writes it
        auto move = [&](std::string& path, const char* sub, const char* ext, int bit) {
            if (path.empty()) return;
            if (path.compare(0, internal.size() + 1, internal + "/") == 0) { path = card + path.substr(internal.size()); return; }
            if (path.compare(0, cache.size(), cache) != 0) return;
            std::string relDst = "/downloaded_media/" + sys + "/" + sub + "/" + names[0] + ext;
            bool have = false;
            for (const auto& r : artRoots) {
                for (const char* e : {".png", ".jpg", ".jpeg", ".webp"}) {
                    const std::string rel = "/" + sys + "/" + sub + "/" + names[0] + e;
                    if (access((r + rel).c_str(), F_OK) == 0) { relDst = "/downloaded_media" + rel; have = true; break; }
                }
                if (have) break;
            }
            if (have) kv.second.artKept |= bit;   // ES-DE's picture: Nano's own copy clashes and goes to the backup
            if (artDst.insert(relDst).second) {
                FileCopyPair p{path, card + relDst};
                p.move = true;
                p.backup = backup + relDst;
                pairs.push_back(p);
            }
            path = card + relDst;
        };
        move(kv.second.box, "covers", ".png", 1);
        move(kv.second.fan, "fanart", ".jpg", 2);
    }
    // Nano's own files: one the card has already gets the entries it lacks (by game).
    auto stageItems = [&](const std::string& rel, const std::string& text, const char* key) {
        std::string cardText;
        if (!onCard(rel)) return stage(rel, text);
        njson::Value cardRoot, mine;
        if (!sdReadFile(card + "/" + rel, &cardText) || !njson::parse(cardText, &cardRoot) || !cardRoot.isObject()
            || !njson::parse(text, &mine)) {
            ALOGW("sdmove: %s on the card cannot be read; nothing moved", rel.c_str());
            return false;
        }
        njson::Value items = njson::Value::makeArray();
        std::set<std::string> have;
        if (const njson::Value* ci = cardRoot.find("items"))
            if (ci->isArray())
                for (const auto& it : ci->arr) {
                    if (it.isObject()) have.insert(it.getString(key));
                    items.arr.push_back(it);
                }
        if (const njson::Value* mi = mine.find("items"))
            if (mi->isArray())
                for (const auto& it : mi->arr)
                    if (it.isObject() && !have.count(it.getString(key))) items.arr.push_back(it);
        cardRoot.set("items") = std::move(items);
        return stage(rel, njson::serialize(cardRoot, true));
    };
    bool staged = stageItems("nano/index.json", scrapeIndexText(index), "rom");
    {
        njson::Value root = njson::Value::makeObject();
        root.set("version") = njson::Value::makeNumber(1);
        njson::Value items = njson::Value::makeArray();
        for (const auto& kv : mRomNameOverride) {
            njson::Value o = njson::Value::makeObject();
            o.set("rom") = njson::Value::makeString(kv.first);
            o.set("name") = njson::Value::makeString(kv.second);
            items.arr.push_back(std::move(o));
        }
        root.set("items") = std::move(items);
        staged = stageItems("nano/names.json", njson::serialize(root, true), "rom") && staged;
    }
    if (!emuItems.arr.empty()) {
        njson::Value root = njson::Value::makeObject();
        root.set("version") = njson::Value::makeNumber(1);
        root.set("items") = std::move(emuItems);
        staged = stageItems("nano/emulators.json", njson::serialize(root, true), "game") && staged;
    }
    if (!staged) {
        fail("Nano's game data could not be prepared. Nothing was moved.");
        return;
    }

    // ES-DE's own folder first (its media wins over a Nano cover of the same game), then a media
    // folder chosen elsewhere, the covers, and the staged files.
    std::vector<FileCopyPair> all;
    {
        FileCopyPair p{internal, card};
        p.move = true;
        p.backup = backup;
        p.merged = merged;
        p.mergedInto = {kEsdeStage, kEsdeStageMerged};
        p.skipDirs = skipDirs;
        all.push_back(p);
    }
    for (const auto& m : mediaDirs) {
        // One on internal storage moves; one elsewhere (another card, a USB drive, a share) is copied.
        FileCopyPair p{m, card + "/downloaded_media"};
        const std::string cm = sdCanon(m), cardVol = sdCanon(sd);
        p.move = sdUnder(cm, "/data/media/0") || sdUnder(cm, cardVol);
        p.backup = backup + "/downloaded_media";
        all.push_back(p);
    }
    for (auto& p : pairs) all.push_back(std::move(p));
    if (access(kEsdeStageMerged, F_OK) == 0) {   // the card's files with the entries they lacked: over them
        // Their originals, put back if the move is cancelled or fails before it is done.
        sdRemoveTree(kEsdeCardOrig);
        std::vector<std::string> files;
        sdListFiles(kEsdeStageMerged, std::string(), &files);
        for (const auto& f : files) {
            std::string text;
            if (!sdReadFile(card + "/" + f, &text) || !sdWriteFile(std::string(kEsdeCardOrig) + "/" + f, text)) {
                sdRemoveTree(kEsdeCardOrig);
                fail("Nano's game data could not be prepared. Nothing was moved.");
                return;
            }
        }
        FileCopyPair p{kEsdeStageMerged, card};
        p.move = true;
        p.overwrite = true;
        all.push_back(p);
    }
    {
        FileCopyPair p{kEsdeStage, card};
        p.move = true;
        p.backup = backup;
        p.group = kGroupEsde;   // switching Nano to the card is this move's relocation
        all.push_back(p);
    }
    mFileJobBackupRoot = backup;
    mSdMoveCard = card;
    mSdMoveOldScrapeDir = mScrapeCacheDir;
    mFileJobStopPkgs.clear();
    if (!fileCopyStart(3, "Move ES-DE to microSD", std::move(all))) {
        sdRemoveTree(kEsdeStage);
        sdRemoveTree(kEsdeStageMerged);
        sdRemoveTree(kEsdeCardOrig);
    }
    else if (mSdMoveAuto) mScrapeProgActive = false;   // setup draws the progress itself
}

// Setup, Keep ES-DE on the microSD card: the move runs only when the internal ES-DE folder has
// files. Without any, an ES-DE folder already on the card is used as it is; nothing is created.
bool NanoMenu::esdeMoveAuto() {
    if (esdeStoreMode()) return false;
    const std::string sd = sdCardRoot();
    if (sd.empty()) return false;
    if (!sdHasFiles(kEsdeInternal, {})) {
        const std::string card = sd + "/ES-DE";
        struct stat st;
        if (stat(card.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {   // as the move's relocation does
            property_set("persist.gammaos.nano.esde.mediadir", "");
            property_set("persist.gammaos.scraper.dir", (card + "/nano").c_str());
            property_set("persist.gammaos.nano.esde.dir", card.c_str());
            esdeStoreReload(false);
            esdeAddCardSystems();
            sdMoveNotice(trDyn("Thimsy now uses the ES-DE folder on the microSD card."));
            ALOGI("sdmove: setup: nothing to move, ES-DE folder now %s", card.c_str());
        }
        return false;
    }
    mSdMoveRoot = sd;
    esdeMoveStart();
    return mFileJobKind == 3;
}

// ---------------------------------------------------------------------------
// Move Emulator Files to microSD
// ---------------------------------------------------------------------------

// PPSSPP's memory stick (the folder holding PSP/): memstick_dir.txt in its app folder, else the
// root of internal storage. "" when it is a folder picked through Android (a content:// address).
std::string ppssppMemstick() { return ppssppMemstickAt(std::string()); }

std::string flycastHome(std::string* prefs) { return flycastHomeAt(std::string(), prefs); }   // home_directory

// DraStic's system folder: _SystemPath ("" = one picked through Android). Its app folder when unset.
std::string drasticSystemDir() { return drasticSystemDirAt(std::string()); }

static std::string mupenDataDir(bool* internal) {
    std::string prefs;
    sdReadPrefs(kMupenPrefs, &prefs);
    return mupenDataDirFrom(prefs, internal);
}

// A setting's folder is there (not on a card or drive that is out, say): only then is it moved.
static bool sdExists(const std::string& dir) {
    struct stat st;
    return stat(sdCanon(dir).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Gives pkg a folder on a volume as if it had been picked through Android, or takes it back (NanoNetBridge).
static bool sdUriCommand(const char* verb, const char* pkg, const std::string& volume, const std::string& rel) {
    static const char kProp[] = "sys.gammaos.nano.uri_grant";
    const std::string arg = std::string(pkg) + "|" + volume + "|" + rel;
    if (arg.size() + 25 >= PROPERTY_VALUE_MAX) {   // "<13-digit sequence>|uri_revoke|" ahead of it
        ALOGW("sdmove: folder %s for %s too long to send (volume %s)", verb, pkg, volume.c_str());
        return false;
    }
    // One at a time (the hakuX folders and a move to microSD can overlap): there is one command and
    // one answer property.
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    property_set(kProp, "");
    for (int attempt = 0; attempt < 2; attempt++) {   // a Wi-Fi command sent meanwhile replaces it unread
        if (!nanoNetBridgeCmd(verb, arg)) return false;
        for (int i = 0; i < 50; i++) {
            usleep(100000);
            char v[PROPERTY_VALUE_MAX] = {};
            property_get(kProp, v, "");
            if (std::string(v) == std::string("ok|") + pkg) return true;
            if (std::string(v) == std::string("fail|") + pkg) return false;
        }
    }
    ALOGW("sdmove: no answer to the folder %s for %s", verb, pkg);
    return false;
}

bool sdUriGrant(const char* pkg, const std::string& volume, const std::string& rel) {
    return sdUriCommand("uri_grant", pkg, volume, rel);
}

bool sdUriRevoke(const char* pkg, const std::string& volume, const std::string& rel) {
    return sdUriCommand("uri_revoke", pkg, volume, rel);
}

void NanoMenu::emuMoveOffer() {
    // Over a running or paused game or app: its files would move under it.
    if (property_get_bool("sys.gammaos.nano.app_launched", false)) {
        photoShowBanner(trDyn("Close the game first."));
        return;
    }
    const std::string sd = sdCardRoot();
    if (sd.empty()) { photoShowBanner(trDyn("Insert a microSD card first.")); return; }
    mSdMoveRoot = sd;
    // Short enough for a 640x480 screen's dialog.
    const std::string body = trDyn("Moves emulator settings, saves and states to the microSD card, with Vita3K's installed "
                                   "games. Saves kept beside the games stay there. Nothing is overwritten.");
    openConfirmDialog(64, "Move Emulator Files to microSD", body, "Move");
}

static bool sdAppMkdirs(const std::string& appRoot, const std::string& dir, std::string* made);

void NanoMenu::emuMoveStart() {
    mEmuMoveBusy = false;
    mEmuMoveRefused = false;
    if (mScrapeRunning || !mScrapeMoveDst.empty() || mFileJobKind != 0 || mEmuSyncRunning.load() > 0) {
        mEmuMoveBusy = true;
        if (mSdMoveAdopt || !mEmuSdAdoptingId.empty()) return;   // an adoption tries again
        sdMoveNotice(trDyn("Another copy or scrape is running"));
        return;
    }
    const std::string sd = mSdMoveRoot;
    if (sd.empty() || access(sd.c_str(), W_OK) != 0) {
        mEmuMoveRefused = true;
        sdMoveNotice(trDyn("Insert a microSD card first."));
        return;
    }
    const std::string card = sd + "/emulator_config";
    const std::string backup = std::string(kBackupRoot) + "/emulator_config";
    std::vector<FileCopyPair> pairs;
    mFileJobStopPkgs.clear();
    mSdMoveEmuDirs.clear();
    mFileJobFrom.clear();
    mFileJobTo.clear();
    mSdMoveRaIncluded = false;

    char cur[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.retroarch.dir", cur, "");
    if (packageInstalled("com.retroarch.aarch64") && std::string(cur) != card + "/retroarch") {
        retroarchMovePairs(card + "/retroarch", backup + "/retroarch", kGroupRetroArch, &pairs);
        mFileJobStopPkgs.push_back("com.retroarch.aarch64");
        mSdMoveRaIncluded = true;
    } else if (packageInstalled("com.retroarch.aarch64") && !mSdMoveAdopt) {
        // Moved before. A RetroArch session that could not read its config ran on its internal
        // defaults and made files there again: they join the card's (its copy of a file wins), and
        // the relocation points both configs at the card again. Nothing made: only the configs.
        if (sdHasFiles(nanora::kRaDefSd, {"thumbnails"})) {
            mFileJobFrom.clear();
            mFileJobTo = card + "/retroarch";
            FileCopyPair p{nanora::kRaDefSd, mFileJobTo};
            p.move = true; p.backup = backup + "/retroarch"; p.group = kGroupRetroArch; p.skipDirs = {"thumbnails"};
            pairs.push_back(p);
            mFileJobStopPkgs.push_back("com.retroarch.aarch64");
            mSdMoveRaIncluded = true;
        } else {
            retroarchHealStart(true);
        }
    }

    if (drasticNanoAvailable()) {   // the DS emulator: its folder, and a cheats folder chosen elsewhere
        property_get("persist.gammaos.drastic.data_dir", cur, "");
        const std::string from = cur[0] == '/' ? std::string(cur) : std::string(kDrasticNanoDefDir);
        const std::string to = card + "/drastic-nano";
        if (from != to && (cur[0] != '/' || sdExists(from))) {
            FileCopyPair p{from, to};
            p.move = true; p.backup = backup + "/drastic-nano"; p.group = kGroupDrasticNano;
            pairs.push_back(p);
            property_get("persist.gammaos.drastic.cheats_dir", cur, "");
            const std::string cheats = cur;
            if (cheats.size() > 1 && cheats[0] == '/' && cheats.compare(0, from.size() + 1, from + "/") != 0
                && cheats.compare(0, sd.size() + 1, sd + "/") != 0 && sdExists(cheats)) {   // else (a drive that is out) left as it is
                FileCopyPair c{cheats, to + "/cheats"};
                c.move = true; c.backup = backup + "/drastic-nano/cheats"; c.group = kGroupDrasticNano;
                pairs.push_back(c);
                mSdMoveEmuDirs["drastic-cheats"] = "";
            } else if (cheats.compare(0, from.size() + 1, from + "/") == 0) {   // moves with the folder
                const std::string rest = cheats.substr(from.size());
                mSdMoveEmuDirs["drastic-cheats"] = rest == "/cheats" ? std::string() : to + rest;
            }
            mSdMoveEmuDirs["drastic-nano"] = to;
        }
    }

    if (packageInstalled(kPpssppPkg)) {
        const std::string memstick = ppssppMemstick();
        const std::string to = card + "/ppsspp";
        if (!memstick.empty() && memstick != to
            && (sdExists(memstick) || memstick == "/storage/emulated/0/Android/data/org.ppsspp.ppsspp/files")) {
            // The memory stick holds PSP/ (and PPSSPP's PAUTH/, EXDATA/), or is itself a folder named PSP.
            // Named PSP, PPSSPP keeps PAUTH/ and EXDATA/ inside it; they go beside PSP/ on the card.
            const size_t sl = memstick.find_last_of('/');
            const bool isPsp = strcasecmp(memstick.c_str() + (sl == std::string::npos ? 0 : sl + 1), "PSP") == 0;
            const std::string pspDir = isPsp ? memstick : memstick + "/PSP";
            for (const char* part : {"PSP", "PAUTH", "EXDATA"}) {
                const bool psp = !strcmp(part, "PSP");
                FileCopyPair p{rawStoragePath(psp ? pspDir : (isPsp ? pspDir : memstick) + "/" + part), to + "/" + part};
                p.move = true; p.backup = backup + "/ppsspp/" + part; p.group = kGroupPpsspp;
                if (psp && isPsp) p.skipDirs = {"PAUTH", "EXDATA"};
                pairs.push_back(p);
            }
            mSdMoveEmuDirs["ppsspp"] = to;
            mFileJobStopPkgs.push_back(kPpssppPkg);
        }
    }

    if (packageInstalled(kFlycastPkg)) {
        std::string prefs;
        const std::string home = flycastHome(&prefs);
        const std::string to = card + "/flycast";
        if (!home.empty() && home != to && (home == kFlycastDefHome || sdExists(home))) {
            const bool own = home == kFlycastDefHome;
            for (const char* part : own ? std::vector<const char*>{""}
                                        : std::vector<const char*>{"emu.cfg", "data", "mappings", "textures"}) {
                FileCopyPair p{rawStoragePath(*part ? home + "/" + part : home), *part ? to + "/" + part : to};
                p.move = true; p.backup = backup + "/flycast" + (*part ? std::string("/") + part : ""); p.group = kGroupFlycast;
                pairs.push_back(p);
            }
            mSdMoveEmuDirs["flycast"] = to;
            mSdMoveEmuDirs["flycast-from"] = home;
            mSdMoveEmuDirs["flycast-own"] = own ? "1" : "";
            mFileJobStopPkgs.push_back(kFlycastPkg);
        }
    }

    if (packageInstalled(kDrasticPkg)) {   // DraStic's whole system folder: BIOS, config, saves, states, cheats
        const std::string from = drasticSystemDir();
        const std::string to = card + "/drastic";
        if (!from.empty() && from != to && (from == kDrasticDefDir || sdExists(from))) {
            FileCopyPair p{rawStoragePath(from), to};
            p.move = true; p.backup = backup + "/drastic"; p.group = kGroupDrastic;
            pairs.push_back(p);
            mSdMoveEmuDirs["drastic"] = to;
            mFileJobStopPkgs.push_back(kDrasticPkg);
        }
    }

    if (packageInstalled(kMupenPkg)) {   // M64Plus FZ's game data (saves and states)
        bool internal = false;
        const std::string dir = mupenDataDir(&internal);
        const std::string to = card + "/m64plus";
        if ((internal || (!dir.empty() && (dir == "/storage/emulated/0/M64Plus" || sdExists(dir)))) && dir != to) {
            FileCopyPair p{internal ? std::string(kMupenInternalData) : rawStoragePath(dir + "/GameData"), to + "/GameData"};
            p.move = true; p.backup = backup + "/m64plus/GameData"; p.group = kGroupMupen;
            pairs.push_back(p);
            mSdMoveEmuDirs["m64plus"] = to;
            mFileJobStopPkgs.push_back(kMupenPkg);
        }
    }

    if (packageInstalled(kArmsx2Pkg)) {   // its data folder, but for the BIOS (set by its own path) and caches
        std::string prefs;
        if (sdReadPrefs(kArmsx2Prefs, &prefs)) {
            bool chosen = false;
            const std::string from = armsx2SystemDirFrom(prefs, &chosen);
            const std::string to = card + "/armsx2";
            if (from[0] == '/' && from != to && (!chosen || sdExists(from))) {
                FileCopyPair p{rawStoragePath(from), to};
                p.move = true; p.backup = backup + "/armsx2"; p.group = kGroupArmsx2;
                p.skipDirs = {"bios", "cache", "resources", "logs"};
                pairs.push_back(p);
                mSdMoveEmuDirs["armsx2"] = to;
                mSdMoveEmuDirs["armsx2-from"] = from;
                mFileJobStopPkgs.push_back(kArmsx2Pkg);
            }
        }
    }

    if (packageInstalled(kScummvmPkg)) {   // its saves (its settings file has a fixed place)
        std::string ini;
        if (nanobios::appFileRead(kScummvmIni, &ini)) {
            const std::string from = scummvmSavePath(ini);
            const std::string to = card + "/scummvm/saves";
            if (!from.empty() && from != to && (from == kScummvmDefSaves || sdExists(from))) {
                FileCopyPair p{rawStoragePath(from), to};
                p.move = true; p.backup = backup + "/scummvm/saves"; p.group = kGroupScummvm;
                pairs.push_back(p);
                mSdMoveEmuDirs["scummvm"] = to;
                mSdMoveEmuDirs["scummvm-from"] = from;
                mFileJobStopPkgs.push_back(kScummvmPkg);
            }
        }
    }

    if (packageInstalled(kVita3kPkg)) {   // its memory card and system: saves, firmware and installed games
        std::string cfg;
        if (nanobios::appFileRead(kVita3kConfig, &cfg)) {
            std::string from = vita3kPrefPath(cfg);
            const bool chosen = !from.empty() && from != kVita3kDefPref;
            if (from.empty()) from = kVita3kDefPref;
            const std::string to = card + "/vita3k";
            if (from[0] == '/' && from != to && (!chosen || sdExists(from))) {
                FileCopyPair p{rawStoragePath(from), to};
                p.move = true; p.backup = backup + "/vita3k"; p.group = kGroupVita3k;
                pairs.push_back(p);
                mSdMoveEmuDirs["vita3k"] = to;
                mFileJobStopPkgs.push_back(kVita3kPkg);
            }
        }
    }

    if (packageInstalled(kAzaharPkg)) {   // its whole folder but caches and logs
        std::string prefs, uri;
        sdReadPrefs(kAzaharPrefs, &prefs);
        const std::string from = azaharDir(prefs, &uri);
        const std::string volume = sd.substr(sd.find_last_of('/') + 1);
        if (!from.empty() && sdExists(from) && uri != azaharTreeUri(volume, "emulator_config/azahar")) {
            FileCopyPair p{rawStoragePath(from), card + "/azahar"};
            p.move = true; p.backup = backup + "/azahar"; p.group = kGroupAzahar;
            p.skipDirs = {"cache", "log", "shaders"};
            pairs.push_back(p);
            mSdMoveEmuDirs["azahar"] = card + "/azahar";
            mFileJobStopPkgs.push_back(kAzaharPkg);
        }
    }

    if (packageInstalled(kSeedlessPkg)) {   // its saves and states; kept beside the games (its default), they stay there
        std::string prefs, uri;
        sdReadPrefs(kSeedlessPrefs, &prefs);
        int location = kSeedlessSaveBesideGames;
        const std::string from = seedlessSaveDir(prefs, &location, &uri);
        const std::string volume = sd.substr(sd.find_last_of('/') + 1);
        const std::string to = card + "/seedlessds";
        bool any = false;
        if (location == kSeedlessSaveApp) {   // both into one: in a picked folder it keeps them side by side
            for (const char* part : {"backup", "savestates"}) {
                FileCopyPair p{from + "/" + part, to};
                p.move = true; p.backup = backup + "/seedlessds"; p.group = kGroupSeedless;
                pairs.push_back(p);
            }
            any = true;
        } else if (location == kSeedlessSaveFolder && !from.empty() && sdExists(from)
                   && uri != azaharTreeUri(volume, "emulator_config/seedlessds")) {
            FileCopyPair p{rawStoragePath(from), to};
            p.move = true; p.backup = backup + "/seedlessds"; p.group = kGroupSeedless;
            pairs.push_back(p);
            any = true;
        }
        if (any) {
            mSdMoveEmuDirs["seedlessds"] = to;
            mFileJobStopPkgs.push_back(kSeedlessPkg);
        }
    }

    // No setting for these folders: each is moved, and a link to the card takes its place (linkTo).
    // Android/data is the storage's own folder to apps here (no FUSE in between), so they follow it.
    // A folder an earlier move set aside but did not empty (<folder>.nanomove: stopped part way) goes to
    // the card too, after the folder itself, so the newer files win and its clashes go to the backup.
    auto addLinked = [&](const std::string& from, const std::string& to, const std::string& bak, int group) {
        struct stat ls;
        bool any = false;
        const bool there = lstat(from.c_str(), &ls) == 0;
        if (!there || S_ISDIR(ls.st_mode)) {   // a folder, or none yet (the link makes it); a link: moved before
            FileCopyPair p{from, to};
            p.move = true; p.backup = bak; p.group = group; p.linkTo = to;
            pairs.push_back(p);
            any = true;
        }
        const size_t sl = from.find_last_of('/');
        const std::string parent = from.substr(0, sl), aside = from.substr(sl + 1) + ".nanomove";
        std::vector<std::string> left;   // <folder>.nanomove, and .nanomove.<n> of a move after one that stopped
        if (DIR* d = opendir(parent.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if ((n == aside || n.compare(0, aside.size() + 1, aside + ".") == 0)
                    && lstat((parent + "/" + n).c_str(), &ls) == 0 && S_ISDIR(ls.st_mode))
                    left.push_back(parent + "/" + n);
            }
            closedir(d);
        }
        std::sort(left.begin(), left.end());
        for (const auto& l : left) {
            FileCopyPair p{l, to};
            p.move = true; p.backup = bak; p.group = group;
            pairs.push_back(p);
            any = true;
        }
        return any;
    };
    struct stat st;
    if (packageInstalled(kDolphinPkg) && stat(kDolphinUserDir, &st) == 0 && S_ISDIR(st.st_mode)) {
        bool any = false;
        for (const char* part : {"Config", "GameSettings", "StateSaves", "GC", "Wii", "Load", "ResourcePacks", "ScreenShots", "GBA"})
            any = addLinked(std::string(kDolphinUserDir) + "/" + part, card + "/dolphin/" + part,
                            backup + "/dolphin/" + part, kGroupDolphin) || any;
        if (any) mFileJobStopPkgs.push_back(kDolphinPkg);
    }
    // hakuX never started (a reinstall): its files folder is made so the link to the card is there
    // when it first starts.
    if (packageInstalled(kHakuxPkg) && mSdMoveAuto && !mSdMoveQuiet && lstat(kHakuxNative, &st) != 0) {
        std::string made;
        const std::string root = "/data/data/com.rfandango.haku_x", files = root + "/files";
        if (sdAppMkdirs(root, files, &made) && !made.empty()) runArgv({"/system/bin/restorecon", "-R", made}, nullptr, 60000);
    }
    if (packageInstalled(kHakuxPkg)
        && ((lstat(kHakuxNative, &st) == 0 && S_ISDIR(st.st_mode))
            || (mSdMoveAuto && !mSdMoveQuiet && lstat(kHakuxNative, &st) != 0
                && lstat("/data/data/com.rfandango.haku_x/files", &st) == 0 && S_ISDIR(st.st_mode))
            || access((std::string(kHakuxNative) + ".nanomove").c_str(), F_OK) == 0)) {
        // The whole folder: its hard disk, save states and caches move between its parts by renaming.
        if (addLinked(kHakuxNative, card + "/hakux/native", backup + "/hakux/native", kGroupHakux))
            mFileJobStopPkgs.push_back(kHakuxPkg);
    }

    if (mSdMoveQuiet) {
        // The look after a game: only an emulator that made files on internal storage since (one
        // that ran for the first time); the others were pointed at the card when it was adopted.
        char failedProp[PROPERTY_VALUE_MAX] = {};
        property_get(kEmuSdFailedProp, failedProp, "");
        const std::string failed = std::string(",") + failedProp + ",";
        std::set<int> keep;
        for (const auto& p : pairs)   // not one that failed to move this boot (tried again from the menu)
            if (p.group >= 0 && sdHasFiles(p.src, p.skipDirs)
                && failed.find("," + std::to_string(p.group) + ",") == std::string::npos)
                keep.insert(p.group);
        for (const auto& gp : kGroupPkg) {
            if (keep.count(gp.first)) continue;
            mFileJobStopPkgs.erase(std::remove(mFileJobStopPkgs.begin(), mFileJobStopPkgs.end(), std::string(gp.second)),
                                   mFileJobStopPkgs.end());
        }
        if (!keep.count(kGroupRetroArch)) mSdMoveRaIncluded = false;
        pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const FileCopyPair& p) { return !keep.count(p.group); }),
                    pairs.end());
    }
    if (pairs.empty()) {
        mFileJobStopPkgs.clear();
        if (!mSdMoveAdopt) sdMoveNotice(trDyn("The emulator settings are already on the microSD card."));
        return;
    }
    for (auto& p : pairs) {
        // Only from where the emulator keeps its own files: its settings name the folder, and root
        // must not take (or delete) another program's files on their word. The engine checks the
        // root again when it runs.
        p.srcRoot = sdSourceRoot(p.src, groupPkg(p.group));
        const bool notJustSaves = p.group == kGroupSeedless && !sdUnder(sdCanon(p.src), appdef::kSeedlessUserDir)
                                  && !seedlessSavesOnly(p.src);
        if (!p.srcRoot.empty() && !(p.move && sdTooBroad(p.src, false)) && !notJustSaves) continue;
        ALOGW("sdmove: %s is not a folder emulator files are moved from", p.src.c_str());
        char msg[600];
        snprintf(msg, sizeof(msg), trDyn("%s holds more than emulator settings, so nothing was moved. Give the emulator a folder of its own first."),
                 sdShown(p.src).c_str());
        mFileJobStopPkgs.clear();
        mSdMoveRaIncluded = false;
        mEmuMoveRefused = true;
        if (mSdMoveAuto) { sdMoveNotice(msg); return; }
        feInfoDialog("Move Emulator Files to microSD", msg);
        if (mEmuSdAdoptingId.empty()) mPs3DlgKeepOpen = true;   // opened from the confirm dialog's accept
        return;
    }
    // Not while a settings sync (this Nano's or the other's) reads or writes the same settings.
    const int moveFd = open(kEmuMoveLock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (moveFd < 0 || flock(moveFd, LOCK_EX | LOCK_NB) != 0) {
        if (moveFd >= 0) close(moveFd);
        mFileJobStopPkgs.clear();
        mSdMoveRaIncluded = false;
        mEmuMoveBusy = true;
        if (!mSdMoveAdopt && mEmuSdAdoptingId.empty()) sdMoveNotice(trDyn("Another copy or scrape is running"));
        return;
    }
    mFileJobBackupRoot = backup;
    if (!fileCopyStart(4, "Move Emulator Files to microSD", std::move(pairs))) {
        mEmuMoveRefused = true;
        close(moveFd);
        mFileJobStopPkgs.clear();
        mSdMoveRaIncluded = false;
    } else {
        if (mEmuMoveLockFd >= 0) close(mEmuMoveLockFd);
        mEmuMoveLockFd = moveFd;   // until the move ends (sdMoveFinish), or this process does
    }
    // Setup draws the progress itself; the look after a game shows none.
    if (mFileJobKind == 4 && ((mSdMoveAuto && !mSdMoveAdopt) || mSdMoveQuiet)) mScrapeProgActive = false;
    if (mFileJobKind == 4 && mSdMoveQuiet) mSdMoveQuietJob = true;
}

// Setup, Keep emulator files on the microSD card: true when a move job started. The card's own
// settings first (a reinstall gets them back), then the move, which also points the emulators the
// card has folders for at them.
bool NanoMenu::emuMoveAuto() {
    const std::string sd = emuSdSlotCard();
    if (sd.empty()) return false;
    property_set(kEmuSdProp, "1");
    mSdMoveRoot = sd;
    mEmuSdAdoptingId = sd.substr(sd.find_last_of('/') + 1);   // adopted once the move is done (the home doesn't again)
    emuMoveStart();
    if (mFileJobKind != 4) emuSdAdoptDone(!mEmuMoveBusy && !mEmuMoveRefused);
    return mFileJobKind == 4;
}

static void emuRepointAbsent(const std::string& card, const std::function<bool(const std::string&)>& installed,
                             std::set<std::pair<std::string, std::pair<std::string, std::string>>>* grants);

// The settings sync for a card on a thread of its own (a first sync can copy many files);
// mEmuSyncRunning until it is done. adopt: a card adopted, what names a card that is out first.
void NanoMenu::emuSettingsSyncAsync(const std::string& card, bool appWins, bool adopt) {
    mEmuSyncRunning++;
    std::thread([this, card, appWins, adopt]() {
        nanoThreadNormalPriority();   // not the render thread's real-time priority, nor its children's
        mEmuSyncRestored += emuSettingsSync(card, std::string(), false, appWins, adopt);
        mEmuSyncRunning--;
        mEmuSyncDone = true;   // the home looks at once (an adoption's move waits on it)
    }).detach();
}

// ---------------------------------------------------------------------------
// Emulator files kept on the card (kEmuSdProp, set by a move or by setup): the settings in app
// folders kept in step with the card's copies (NanoEmuSync.cpp), and a card that comes in adopted:
// the emulators it has folders for are pointed at them, the others' files move to it.
// ---------------------------------------------------------------------------

static const char* const kEmuSyncBase = "/data/system/nano_emu_sync.txt";
// Folder grants the settings that came from the card need, not yet given (pkg, volume, folder per line).
static const char* const kEmuSyncGrants = "/data/system/nano_emu_grants.txt";
// A game is starting: a sync running in either Nano stops between files so the game's own goes first.
// Its value is when it was asked (uptime ms); an ask older than a few seconds (a launch that gave up
// waiting) no longer stops anything.
static const char* const kEmuSyncStopProp = "sys.gammaos.nano.emu_sync_stop";
static bool emuSyncStopAsked() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSyncStopProp, v, "0");
    const long long at = atoll(v);
    return at > 0 && (long long)uptimeMillis() - at < 5000;
}
static std::mutex sEmuSyncMutex;   // the home and a worker thread: one sync at a time

bool NanoMenu::emuSdMode() {
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdProp, v, "");
    if (v[0]) return v[0] == '1';
    // Moved by a build before the setting: RetroArch's folder on a card says so.
    property_get("persist.gammaos.retroarch.dir", v, "");
    const bool moved = strncmp(v, "/storage/", 9) == 0 && strncmp(v, "/storage/emulated/", 18) != 0 &&
                       strstr(v, "/emulator_config/") != nullptr;
    if (moved) property_set(kEmuSdProp, "1");
    return moved;
}

// A file's modification time and size in one number, -1 when there is no file.
static int64_t sdSyncStamp(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return ((int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec) * 31 + (int64_t)st.st_size;
}

// The files under dir (relative), not below its top-level skip folders, never through a link.
static void sdSyncList(const std::string& dir, const std::string& rel, const std::vector<std::string>& skip,
                       std::set<std::string>* out, int depth = 0) {
    if (depth > 8) return;
    DIR* d = opendir((rel.empty() ? dir : dir + "/" + rel).c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == ".." || emusync::skipFile(n)) continue;
        if (rel.empty() && std::find(skip.begin(), skip.end(), n) != skip.end()) continue;
        const std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st;
        if (lstat((dir + "/" + r).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) sdSyncList(dir, r, skip, out, depth + 1);
        else if (S_ISREG(st.st_mode)) out->insert(r);
    }
    closedir(d);
}

// The folders of path inside an app's folder that are missing, made as the app's (owner of the
// folder above); *made the topmost one made, for its label.
static bool sdAppMkdirs(const std::string& appRoot, const std::string& dir, std::string* made) {
    if (dir.compare(0, appRoot.size(), appRoot) != 0) return false;
    struct stat st;
    if (lstat(appRoot.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;   // not installed, or a link
    std::string cur = appRoot;
    for (size_t p = appRoot.size(); p < dir.size();) {
        const size_t q = dir.find('/', p + 1);
        const std::string next = dir.substr(0, q == std::string::npos ? dir.size() : q);
        struct stat ps, ns;
        if (lstat(next.c_str(), &ns) == 0) {
            if (!S_ISDIR(ns.st_mode)) return false;   // a link or a file in the way: never written through
        } else {
            if (lstat(cur.c_str(), &ps) != 0 || mkdir(next.c_str(), 0771) != 0) return false;
            if (lchown(next.c_str(), ps.st_uid, ps.st_gid) != 0) ALOGW("emusync: chown %s failed", next.c_str());
            if (made && made->empty()) *made = next;
        }
        cur = next;
        p = q == std::string::npos ? dir.size() : q;
    }
    return true;
}

// Every folder from appRoot down to path's is a folder, not a link (the app controls its folder:
// root never deletes or writes through one of its links).
static bool sdAppPathSafe(const std::string& appRoot, const std::string& path) {
    if (path.compare(0, appRoot.size() + 1, appRoot + "/") != 0) return false;
    for (size_t p = path.find('/', appRoot.size() + 1); p != std::string::npos; p = path.find('/', p + 1)) {
        struct stat st;
        if (lstat(path.substr(0, p).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    }
    return true;
}

// sdWriteFile, on the disk before it takes the old one's place (a card pulled out soon after keeps
// the old file or the new one, never part of it).
static bool sdWriteFileSync(const std::string& path, const std::string& text) {
    sdMkdirs(path.substr(0, path.find_last_of('/')));
    const std::string tmp = path + ".nanotmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (fd < 0) return false;
    bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    const int dfd = open(path.substr(0, path.find_last_of('/')).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return true;
}

// A volume that is not in: its emulator_config is named in settings only because they came from it.
static bool sdVolumeAbsent(const std::string& id) {
    return nanoplat::get().volumes->absent(id);
}

// The folder grants settings name that Nano may give again (NanoNetBridge allows these apps only,
// and no protected folder).
static void sdGrantsFrom(const char* pkg, const std::string& text, const std::string& cardId,
                         std::set<std::pair<std::string, std::pair<std::string, std::string>>>* out) {
    static const char* const kApps[] = {kHakuxPkg, kAzaharPkg, kSeedlessPkg, kWatermelonPkg, kScummvmPkg};
    if (std::none_of(std::begin(kApps), std::end(kApps), [&](const char* a) { return strcmp(pkg, a) == 0; })) return;
    for (const auto& u : emusync::treeUris(text))
        if (u.first == cardId || u.first == "primary") out->insert({pkg, u});
}

// Keeps the settings in app folders in step with the card's copies (all emulators, or onlyPkg's).
// launching: onlyPkg is about to start, so it is stopped first if the card's copies change it (and
// nothing waits for a sync already running); any other emulator that is running gets nothing from
// the card until it is not. appWins: files never synced with this card go to it, not from it.
// adopt: a card adopted, what names a card that is out pointed at it first. Returns how many files
// came into apps.
int NanoMenu::emuSettingsSync(const std::string& card, const std::string& onlyPkg, bool launching, bool appWins,
                              bool adopt) {
    if (card.empty() || (!launching && biosWorkerBusy())) return 0;   // a BIOS install writes emulator settings too
    std::unique_lock<std::mutex> lk(sEmuSyncMutex, std::defer_lock);
    // And one at a time across the two Nano processes (the home, the resident overlay).
    const int lockFd = open("/data/system/nano_emu_sync.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockFd < 0) return 0;
    struct LockEnd { int fd; ~LockEnd() { close(fd); } } lockEnd{lockFd};
    if (launching) {
        // A sync running is asked to stop between files and waited for a moment.
        property_set(kEmuSyncStopProp, std::to_string(uptimeMillis()).c_str());
        bool got = false;
        for (int i = 0; i < 100 && !got; i++) {
            if (lk.try_lock()) {
                got = flock(lockFd, LOCK_EX | LOCK_NB) == 0;
                if (!got) lk.unlock();
            }
            if (!got) usleep(20000);
        }
        if (!got) return 0;   // the sync holding it stops at its next file (and says so)
        property_set(kEmuSyncStopProp, "0");
    } else {
        lk.lock();
        if (flock(lockFd, LOCK_EX) != 0) return 0;
    }
    // Not while a move rewrites the same settings, in this Nano or the other (emuMoveLock).
    const int moveFd = open(kEmuMoveLock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (moveFd < 0 || flock(moveFd, LOCK_SH | LOCK_NB) != 0) {
        if (moveFd >= 0) close(moveFd);
        return 0;
    }
    LockEnd moveEnd{moveFd};
    std::map<std::string, bool> installedPkgs;
    auto installed = [&](const std::string& pkg) {
        auto it = installedPkgs.find(pkg);
        return it != installedPkgs.end() ? it->second : (installedPkgs[pkg] = packageInstalled(pkg));
    };
    std::set<std::pair<std::string, std::pair<std::string, std::string>>> grants;
    std::string grantsText;   // those not given before, tried again
    sdReadFile(kEmuSyncGrants, &grantsText);
    for (size_t p = 0; p < grantsText.size();) {
        size_t nl = grantsText.find('\n', p);
        if (nl == std::string::npos) nl = grantsText.size();
        const std::string l = grantsText.substr(p, nl - p);
        p = nl + 1;
        const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1);
        if (t2 != std::string::npos) grants.insert({l.substr(0, t1), {l.substr(t1 + 1, t2 - t1 - 1), l.substr(t2 + 1)}});
    }
    if (adopt) emuRepointAbsent(card, installed, &grants);
    const std::string cardId = card.substr(card.find_last_of('/') + 1);
    const uint64_t cardTag = emusync::hash(cardId);
    char appWinsCard[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdAppWinsProp, appWinsCard, "");
    appWins = appWins || cardId == appWinsCard;
    bool skipped = false;   // a file left for next time
    auto cardThere = [&]() { struct stat cs; return stat(card.c_str(), &cs) == 0 && S_ISDIR(cs.st_mode); };
    std::string text;
    sdReadFile(kEmuSyncBase, &text);
    emusync::Base base = emusync::baseLoad(text);
    const emusync::Base before = base;
    auto run = [](const std::vector<std::string>& a, std::string* o, int t) { return runArgv(a, o, t); };
    std::set<std::string> stopped;
    std::map<std::string, bool> remade;   // app folder -> made again since this card's last sync
    std::map<std::string, int64_t> folderIds;   // app folder -> its inode now
    std::set<std::string> held;   // app folders something was not brought into this time
    int restoredFiles = 0;
    bool stopEarly = false;
    for (const auto& it : emusync::items()) {
        if (stopEarly) break;
        if (!onlyPkg.empty() && onlyPkg != it.pkg) continue;
        if (launching && it.many) continue;   // the game waits for this: left to the home screen's syncs
        if (!installed(it.pkg)) continue;
        const std::string appRoot = it.media ? std::string("/data/media/0/Android/data/") + it.pkg + "/files"
                                             : std::string("/data/data/") + it.pkg;
        struct stat rst;
        if (lstat(appRoot.c_str(), &rst) != 0 || !S_ISDIR(rst.st_mode)) continue;   // never started (media), or a link
        // An app installed again has a folder of its own: its files are not the ones this card's
        // last sync saw, so the card's copies win as on a card never synced.
        // The new folder counts as seen once everything came into it (below).
        if (!remade.count(appRoot)) {
            auto idIt = base.find(emusync::baseKey(cardId, "\x1e", appRoot));
            remade[appRoot] = idIt != base.end() && idIt->second.app != (int64_t)rst.st_ino;
            folderIds[appRoot] = (int64_t)rst.st_ino;
        }
        const std::string app = emusync::appPath(it), cardDir = emusync::cardPath(card, it);
        struct stat ast, cst;
        const bool appIsFile = lstat(app.c_str(), &ast) == 0 && S_ISREG(ast.st_mode);
        const bool cardIsFile = lstat(cardDir.c_str(), &cst) == 0 && S_ISREG(cst.st_mode);
        const bool single = strchr(it.rel, '.') != nullptr || appIsFile || cardIsFile;   // a file, not a folder
        std::set<std::string> rels;
        if (single) rels.insert("");
        else {
            sdSyncList(app, std::string(), it.skip, &rels);
            if (it.many) {   // what the app unpacked from its package is its own, not the user's
                std::vector<std::string> list(rels.begin(), rels.end());
                std::vector<int64_t> mtimes;
                for (const auto& r : list) {
                    struct stat fst;
                    mtimes.push_back(lstat((app + "/" + r).c_str(), &fst) == 0 ? (int64_t)fst.st_mtime : 0);
                }
                for (size_t i : emusync::unpackedTogether(mtimes)) rels.erase(list[i]);
            }
            sdSyncList(cardDir, std::string(), it.skip, &rels);
            const std::string prefix = emusync::baseKey(cardId, it.emu, it.rel) + "/";
            for (const auto& kv : base)   // deleted on both sides since: their entries go below
                if (kv.first.compare(0, prefix.size(), prefix) == 0) rels.insert(kv.first.substr(prefix.size()));
        }
        struct File { std::string rel, a, c, key; int64_t sa, sc; };
        std::vector<File> files;
        size_t baseApp = 0, baseCard = 0, nowApp = 0, nowCard = 0;
        for (const auto& r : rels) {
            File f{r, r.empty() ? app : app + "/" + r, r.empty() ? cardDir : cardDir + "/" + r,
                   emusync::baseKey(cardId, it.emu, r.empty() ? it.rel : std::string(it.rel) + "/" + r), 0, 0};
            f.sa = sdSyncStamp(f.a);
            f.sc = sdSyncStamp(f.c);
            // SharedPreferences sets the old file aside as .bak while it writes: neither there nor gone.
            if (f.sa < 0 && access((f.a + ".bak").c_str(), F_OK) == 0) { skipped = true; held.insert(appRoot); continue; }
            auto b = base.find(f.key);
            if (b != base.end()) { baseApp += b->second.app >= 0; baseCard += b->second.card >= 0; }
            nowApp += f.sa >= 0;
            nowCard += f.sc >= 0;
            files.push_back(f);
        }
        // All of one side's files gone at once is not the user deleting them: an app installed
        // again or cleared, a folder deleted on the card or a card that can't be read. The other
        // side's are what is left, taken as on a card never synced.
        const bool reset = remade[appRoot] || (baseApp > 0 && nowApp == 0) || (baseCard > 0 && nowCard == 0);
        for (const auto& f : files) {
            if (!launching && emuSyncStopAsked()) {   // every sync running stops (the request expires)
                stopEarly = true;
                break;
            }
            auto b = base.find(f.key);
            bool haveBase = b != base.end() && !reset;
            std::string cardText;
            bool cardRead = false;
            // A card copy whose time alone moved (its contents as synced: a FAT card's times are local) is unchanged.
            if (haveBase && f.sc >= 0 && f.sc != b->second.card && b->second.hash != 0) {
                cardRead = sdReadFile(f.c, &cardText);
                if (cardRead && emusync::hash(cardText) == b->second.hash) b->second.card = f.sc;
            }
            const std::string rel = f.rel.empty() ? it.rel : std::string(it.rel) + "/" + f.rel;
            const std::string ownerKey = emusync::baseKey(std::string(), it.emu, rel);
            auto owner = base.find(ownerKey);
            // The app's copy came from another card's sync, unchanged since: not this device's change,
            // so this card's copy wins as on a card never synced.
            if (owner != base.end() && owner->second.hash != cardTag && f.sa >= 0 && owner->second.app == f.sa) haveBase = false;
            emusync::Act act = emusync::decide(haveBase, haveBase ? b->second.app : -1, haveBase ? b->second.card : -1,
                                               f.sa, f.sc, appWins);
            if (!haveBase && f.sa >= 0 && f.sc >= 0 && (act == emusync::Act::ToApp || act == emusync::Act::ToCard)) {
                std::string ta;   // never synced: the same settings need nothing
                if (!cardRead) cardRead = sdReadFile(f.c, &cardText);
                if (cardRead && nanobios::appFileRead(f.a, &ta) && ta == emusync::repoint(cardText, cardId, sdVolumeAbsent))
                    act = emusync::Act::None;
            }
            // SharedPreferences sets the old file aside as .bak while it writes: not read meanwhile.
            if (act == emusync::Act::ToCard && access((f.a + ".bak").c_str(), F_OK) == 0) {
                skipped = true;
                held.insert(appRoot);
                continue;
            }
            const bool intoApp = act == emusync::Act::ToApp || act == emusync::Act::DeleteApp;
            // A game started meanwhile, or the card went (the file itself gone, not unreadable):
            // nothing goes into the apps.
            if (intoApp && !launching && property_get_bool("sys.gammaos.nano.app_launched", false)) { held.insert(appRoot); continue; }
            if (act == emusync::Act::DeleteApp) {
                struct stat cs;
                const bool gone = lstat(f.c.c_str(), &cs) != 0 && errno == ENOENT;
                if (!gone || !cardThere() || lstat(f.c.substr(0, f.c.find_last_of('/')).c_str(), &cs) != 0) { held.insert(appRoot); continue; }
            }
            if (intoApp && !stopped.count(it.pkg)) {
                if (sdPkgRunning(it.pkg)) {
                    if (!(launching && onlyPkg == it.pkg)) { held.insert(appRoot); continue; }   // later, once it has stopped
                    runArgv({"/system/bin/am", "force-stop", it.pkg}, nullptr, 15000);   // it rewrites its settings when it closes
                }
                stopped.insert(it.pkg);
            }
            bool ok = true;
            switch (act) {
                case emusync::Act::None: break;
                case emusync::Act::ToApp: {
                    if (!cardRead) cardRead = sdReadFile(f.c, &cardText);
                    // Read as it was (not mid-write), and no empty settings file (one written as the card
                    // was pulled out would reset the emulator).
                    const bool emptyXml = cardText.empty() && f.c.size() > 4 && f.c.compare(f.c.size() - 4, 4, ".xml") == 0;
                    if (!cardRead || sdSyncStamp(f.c) != f.sc || emptyXml) { ok = false; break; }
                    const std::string t = emusync::repoint(cardText, cardId, sdVolumeAbsent);
                    std::string made;
                    // Labelled as the app's at once (a launch may end this process before the sync ends).
                    ok = sdAppMkdirs(appRoot, f.a.substr(0, f.a.find_last_of('/')), &made);
                    if (!made.empty()) runArgv({"/system/bin/restorecon", "-R", made}, nullptr, 60000);
                    ok = ok && sdAppPathSafe(appRoot, f.a) && sdSyncStamp(f.a) == f.sa && nanobios::appFileWrite(f.a, t, run);
                    if (ok) {
                        restoredFiles++;
                        sdGrantsFrom(it.pkg, t, cardId, &grants);
                    }
                    break;
                }
                case emusync::Act::ToCard:
                    // Read as it was: a file the emulator writes in place is sent once it has written it.
                    ok = nanobios::appFileRead(f.a, &cardText) && sdSyncStamp(f.a) == f.sa && sdWriteFileSync(f.c, cardText);
                    cardRead = ok;
                    break;
                case emusync::Act::DeleteApp:
                    ok = sdAppPathSafe(appRoot, f.a) && (unlink(f.a.c_str()) == 0 || errno == ENOENT);
                    if (ok) unlink((f.a + ".bak").c_str());   // else SharedPreferences brings it back
                    break;
                case emusync::Act::DeleteCard: ok = unlink(f.c.c_str()) == 0 || errno == ENOENT; break;
            }
            if (!ok) {
                ALOGW("emusync: %s: %s could not be brought in step", it.emu, rel.c_str());
                if (intoApp) held.insert(appRoot);
                skipped = true;
                continue;   // tried again next time
            }
            // The side that was read as it was then: a change since is sent next time.
            const bool none = act == emusync::Act::None;
            const int64_t na = act == emusync::Act::ToCard || none ? f.sa : sdSyncStamp(f.a);
            const int64_t nc = act == emusync::Act::ToApp || none ? f.sc : sdSyncStamp(f.c);
            if (na < 0 && nc < 0) {
                base.erase(f.key);
            } else {
                emusync::State st{na, nc, 0};
                if (nc >= 0) {
                    b = base.find(f.key);
                    if (!cardRead && b != base.end() && b->second.card == nc && b->second.hash) st.hash = b->second.hash;
                    else if (cardRead || sdReadFile(f.c, &cardText)) st.hash = emusync::hash(cardText);
                }
                base[f.key] = st;
            }
            if (act == emusync::Act::ToApp) base[ownerKey] = {na, -1, cardTag};
            if (act != emusync::Act::None)
                ALOGI("emusync: %s %s %s", it.emu, rel.c_str(),
                      act == emusync::Act::ToApp ? "from the card" : act == emusync::Act::ToCard ? "to the card" : "deleted");
        }
    }
    // An app folder made again counts as this card's once all came into it; until then its files
    // are still taken as never synced (else its defaults would go over the card's copies).
    // Keep This Device's holds until every file of this card's has been synced once, or a few full
    // syncs have run (a file that always fails must not keep it on for good).
    // Not before the adoption's move went through: one that didn't is started again with it.
    static int sAppWinsSyncs = 0;
    char adoptedCard[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdCardProp, adoptedCard, "");
    if (cardId == appWinsCard && onlyPkg.empty() && !stopEarly && cardId == adoptedCard) {
        if ((!skipped && held.empty()) || ++sAppWinsSyncs >= 5) {
            property_set(kEmuSdAppWinsProp, "");
            sAppWinsSyncs = 0;
        }
    }
    if (adopt && !stopEarly) mEmuSdAdoptSynced = true;
    if (!stopEarly)
        for (const auto& f : folderIds)
            if (!held.count(f.first)) base[emusync::baseKey(cardId, "\x1e", f.first)] = {f.second, -1, 0};
    // Settings from the card name folders as content:// grants the app must hold (a reinstall has
    // none). Those not given are kept to be tried again; a launch gives only its emulator's.
    std::string notGiven;
    for (const auto& g : grants) {
        if (!installed(g.first)) continue;
        if ((onlyPkg.empty() || onlyPkg == g.first) && (launching || !emuSyncStopAsked())
            && sdUriGrant(g.first.c_str(), g.second.first, g.second.second)) continue;
        notGiven += g.first + "\t" + g.second.first + "\t" + g.second.second + "\n";
    }
    if (notGiven != grantsText) {
        if (notGiven.empty()) unlink(kEmuSyncGrants);
        else if (sdWriteFileSync(kEmuSyncGrants, notGiven)) chmod(kEmuSyncGrants, 0600);
    }
    if (base != before) {
        const std::string tmp = std::string(kEmuSyncBase) + ".tmp";
        if (sdWriteFileSync(tmp, emusync::baseSave(base))) {
            chmod(tmp.c_str(), 0600);
            if (rename(tmp.c_str(), kEmuSyncBase) == 0) {
                const int dfd = open("/data/system", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (dfd >= 0) { fsync(dfd); close(dfd); }
            }
        }
    }
    return restoredFiles;
}

// A card adopted (or setup): what names the emulator_config folder of a card that is not in (a copy
// of the card with another id, the card this one replaced) names this card's instead, before the move
// looks at where each emulator keeps its files. The emulators' settings in their folders, Nano's own
// for RetroArch and the DS player, the settings files on the card, and the links to it.
// The folder grants what was pointed at the card names go to grants (the sync gives them).
static void emuRepointAbsent(const std::string& card, const std::function<bool(const std::string&)>& installed,
                             std::set<std::pair<std::string, std::pair<std::string, std::string>>>* grants) {
    const std::string id = card.substr(card.find_last_of('/') + 1);
    std::vector<std::pair<std::string, std::string>> files;   // path, package
    for (const auto& it : emusync::items()) {
        if (it.many || !installed(it.pkg)) continue;
        const std::string app = emusync::appPath(it);
        std::set<std::string> rels;
        struct stat st;
        if (lstat(app.c_str(), &st) == 0 && S_ISREG(st.st_mode)) rels.insert("");
        else sdSyncList(app, std::string(), it.skip, &rels);
        for (const auto& r : rels) files.push_back({r.empty() ? app : app + "/" + r, it.pkg});
    }
    files.push_back({kPpssppMemstickFile, kPpssppPkg});
    files.push_back({kFlycastPrefs, kFlycastPkg});
    files.push_back({nanora::kRaDefCfgRaw, "com.retroarch.aarch64"});
    for (const auto& f : files) {
        std::string t;
        if (!nanobios::appFileRead(f.first, &t)) continue;
        const std::string n = emusync::repoint(t, id, sdVolumeAbsent);
        if (n == t) continue;
        sdStopIfRunning(f.second);
        auto run = [](const std::vector<std::string>& a, std::string* o, int t) { return runArgv(a, o, t); };
        if (!nanobios::appFileWrite(f.first, n, run)) { ALOGW("emusync: %s could not be pointed at the card", f.first.c_str()); continue; }
        sdGrantsFrom(f.second.c_str(), n, id, grants);
        ALOGI("emusync: %s now names this card", f.first.c_str());
    }
    for (const char* prop : {nanora::kRaDirProp, "persist.gammaos.drastic.data_dir", "persist.gammaos.drastic.cheats_dir"}) {
        char v[PROPERTY_VALUE_MAX] = {};
        property_get(prop, v, "");
        const std::string n = emusync::repoint(v, id, sdVolumeAbsent);
        if (n != v) property_set(prop, n.c_str());
    }
    // The settings files kept on the card itself (Flycast's emu.cfg, ARMSX2's inis, RetroArch's configs).
    std::function<void(const std::string&, int)> walk = [&](const std::string& dir, int depth) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name, path = dir + "/" + name;
            if (name[0] == '.' || (depth == 1 && name == "app")) continue;   // app: the sync's copies
            struct stat st;
            if (lstat(path.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) { if (depth < 3) walk(path, depth + 1); continue; }
            const size_t dot = name.find_last_of('.');
            const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
            if (!S_ISREG(st.st_mode) || st.st_size > (1 << 20)
                || (ext != ".cfg" && ext != ".ini" && ext != ".txt" && ext != ".xml" && ext != ".yml" && ext != ".json"))
                continue;
            std::string t;
            if (!sdReadFile(path, &t)) continue;
            const std::string n = emusync::repoint(t, id, sdVolumeAbsent);
            if (n != t && sdWriteFileSync(path, n)) ALOGI("emusync: %s now names this card", path.c_str());
        }
        closedir(d);
    };
    walk(card + "/emulator_config", 0);
    // Dolphin's and hakuX's folders are links to the card. Their folders are the apps' own: nothing
    // through a link in them, and only a link into this card's emulator_config is made.
    struct LinkDir { std::string appRoot, dir; };
    std::vector<std::pair<LinkDir, std::string>> links;   // folder, link name
    const LinkDir dolphin{"/data/media/0/Android/data/org.dolphinemu.dolphinemu", kDolphinUserDir};
    for (const char* part : {"Config", "GameSettings", "StateSaves", "GC", "Wii", "Load", "ResourcePacks", "ScreenShots", "GBA"})
        links.push_back({dolphin, part});
    const std::string hakux = kHakuxNative;
    links.push_back({{"/data/data/com.rfandango.haku_x", hakux.substr(0, hakux.find_last_of('/'))}, hakux.substr(hakux.find_last_of('/') + 1)});
    const std::string under = "/storage/" + id + "/emulator_config/";
    for (const auto& l : links) {
        const std::string path = l.first.dir + "/" + l.second;
        if (!sdAppPathSafe(l.first.appRoot, path)) continue;
        const int dfd = open(l.first.dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) continue;
        struct DirEnd { int fd; ~DirEnd() { close(fd); } } dirEnd{dfd};
        char buf[PATH_MAX];
        const ssize_t len = readlinkat(dfd, l.second.c_str(), buf, sizeof buf - 1);
        if (len <= 0) continue;
        buf[len] = 0;
        const std::string n = emusync::repoint(buf, id, sdVolumeAbsent);
        if (n == buf || n.compare(0, under.size(), under) != 0 || n.find("/.") != std::string::npos) continue;
        struct stat pst;
        if (fstat(dfd, &pst) != 0) continue;
        sdMkdirs(n);
        const std::string tmp = l.second + ".nanolink";
        unlinkat(dfd, tmp.c_str(), 0);
        if (symlinkat(n.c_str(), dfd, tmp.c_str()) != 0 || renameat(dfd, tmp.c_str(), dfd, l.second.c_str()) != 0) {
            unlinkat(dfd, tmp.c_str(), 0);
            continue;
        }
        if (fchownat(dfd, l.second.c_str(), pst.st_uid, pst.st_gid, AT_SYMLINK_NOFOLLOW) != 0)
            ALOGW("emusync: chown %s failed", path.c_str());
        runArgv({"/system/bin/restorecon", path}, nullptr, 15000);
        ALOGI("emusync: %s now links to %s", path.c_str(), n.c_str());
    }
}

// A move of emulator files running in this Nano or the other (it holds kEmuMoveLock).
bool emuMoveRunning() {
    const int fd = open(kEmuMoveLock, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool held = flock(fd, LOCK_SH | LOCK_NB) != 0;
    close(fd);
    return held;
}

// The microSD card emulator files are kept on: the slot's ("" none, or only a USB drive).
std::string NanoMenu::emuSdSlotCard() {
    const std::string card = sdCardRoot();
    return !card.empty() && sdIsSlot(card) ? card : std::string();
}

// Before a game or app starts: its settings as the card has them (the card adopted).
void NanoMenu::emuSettingsSyncLaunch(const std::string& pkg) {
    if (pkg.empty() || !emuSdMode()) return;
    const std::string card = emuSdSlotCard();
    if (card.empty()) return;
    char v[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdCardProp, v, "");
    if (card.substr(card.find_last_of('/') + 1) == v) emuSettingsSync(card, pkg, true, false);
}

// A card new to this device (never synced here) that has emulator files while this device kept its
// own with another card: whose settings win is asked (emuSdForeignDialog).
static bool emuSdForeign(const std::string& card, const std::string& cardId) {
    struct stat st;
    if (stat((card + "/emulator_config").c_str(), &st) != 0 || !S_ISDIR(st.st_mode)
        || !sdHasFiles(card + "/emulator_config", {}))
        return false;
    std::string text;
    if (!sdReadFile(kEmuSyncBase, &text)) {
        // Never synced here: a reinstall (its emulators' settings still empty) takes the card's; a
        // device that has been used asks.
        for (const auto& it : emusync::items()) {
            if (it.many) continue;   // RetroArch's profiles: mostly what it unpacked itself
            std::set<std::string> rels;
            sdSyncList(emusync::appPath(it), std::string(), it.skip, &rels);
            struct stat fst;
            if (!rels.empty() || (lstat(emusync::appPath(it).c_str(), &fst) == 0 && S_ISREG(fst.st_mode))) return true;
        }
        return false;
    }
    bool other = false;
    for (const auto& kv : emusync::baseLoad(text)) {
        const std::string id = kv.first.substr(0, kv.first.find('\x1f'));
        if (id == cardId) return false;
        other = other || !id.empty();
    }
    return other;
}

void NanoMenu::emuSdForeignDialog() {
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 1; mPs3DlgThemeKey = 67; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    mPs3DlgTitle = "This microSD Card Has Emulator Files";
    mPs3DlgBody = trDyn("It has emulator settings and saves from another device or an older copy. Use the card's in "
                        "place of this device's, or keep this device's and copy them to the card?");
    mPs3DlgOptions.push_back("Not Now");            mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Use the Card's");     mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Keep This Device's"); mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(62);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
    mEmuSdAsking = true;
}

// The card question (theme key 67): 0 Not Now (this boot), 1 Use the Card's, 2 Keep This Device's.
void NanoMenu::emuSdForeignChoice(int sel) {
    mEmuSdAsking = false;
    const std::string card = emuSdSlotCard();
    if (card.empty() || card != mEmuSdCard) return;   // taken out meanwhile: asked again when it is back
    if (sel != 1 && sel != 2) {
        property_set(kEmuSdSkipProp, card.substr(card.find_last_of('/') + 1).c_str());
        return;
    }
    if (biosWorkerBusy()) { mEmuSdCard.clear(); return; }   // its settings are being written: asked again after
    if (sel == 1) property_set(kEmuSdAppWinsProp, "");   // the choice made now, not one held from before
    emuSdAdoptBegin(card, sel == 2);
}

// An adoption's (or setup's) move ended, or found nothing to do: the card counts as adopted (kept in
// step from now on) when it went through; else it is not adopted again this boot.
void NanoMenu::emuSdAdoptDone(bool ok) {
    if (mEmuSdAdoptingId.empty()) return;
    property_set(ok ? kEmuSdCardProp : kEmuSdAdoptFailedProp, mEmuSdAdoptingId.c_str());
    mEmuSdAdoptingId.clear();
}

// Adopting a card: what names a card that is out points at it, its settings are synced, then the
// move points the emulators at it. Launches wait while that runs (emuSdAdopting).
void NanoMenu::emuSdAdoptBegin(const std::string& card, bool appWins) {
    if (mEmuSdLockFd < 0) {   // one Nano process adopts a card (two would start two moves)
        const int fd = open("/data/system/nano_emu_sd.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); mEmuSdCard.clear(); return; }   // tried again
        mEmuSdLockFd = fd;
    }
    // Keep This Device's, chosen for this card before an adoption that didn't finish, still holds.
    const std::string cardId = card.substr(card.find_last_of('/') + 1);
    char held[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdAppWinsProp, held, "");
    appWins = appWins || cardId == held;
    mEmuSdAdoptPending = true;
    mEmuSdAdoptCard = card;
    mEmuSdAppWins = appWins;
    mEmuSdAdoptSynced = false;
    property_set(kEmuSdAppWinsProp, appWins ? cardId.c_str() : "");
    emuSettingsSyncAsync(card, appWins, true);
}

// At the home screen: a card new to this device is adopted, its settings first; the card adopted
// before is kept in step, every 10 s, and looked at again after each game for emulators that ran
// for the first time. The syncs run on a thread of their own.
void NanoMenu::emuSdTick() {
    const int64_t now = elapsedRealtime();
    if (now - mEmuSdCheckMs < 3000 && !mEmuSyncDone.exchange(false)) return;
    mEmuSdCheckMs = now;
    if (mEmuSdLockFd >= 0 && !mEmuSdAdoptPending && !emuSdAdopting()) { close(mEmuSdLockFd); mEmuSdLockFd = -1; }
    // The card question closed without an answer (Home held, another dialog over it): asked again.
    if (mEmuSdAsking && !(mPs3DlgActive && mPs3DlgThemeKey == 67)) { mEmuSdAsking = false; mEmuSdCard.clear(); }
    if (property_get_bool("sys.gammaos.nano.app_launched", false)) {
        mEmuSdRecheck = true;
        mLaunchHandOffMs = 0;   // handed off
        return;
    }
    if (const int64_t h = mLaunchHandOffMs.load(); h != 0 && now - h < 30000) return;
    if (mFileJobKind != 0 || mEmuSyncRunning.load() > 0) return;
    // Held up by other work: an adoption waiting on it lets games start meanwhile.
    mEmuSdAdoptStalled = mSetupWizardActive || sdAutoDeleting() || !mScrapeMoveDst.empty() || biosWorkerBusy()
                         || !property_get_bool("sys.boot_completed", false);
    { std::lock_guard<std::mutex> lk(mScrapeMutex); mEmuSdAdoptStalled = mEmuSdAdoptStalled || mScrapeRunning; }
    if (mEmuSdAdoptStalled) return;
    if (!emuSdMode()) {   // turned off: a card is adopted again once it is turned on
        if (!mEmuSdCard.empty()) {
            property_set(kEmuSdCardProp, "");
            property_set(kEmuSdSkipProp, "");
            property_set(kEmuSdAppWinsProp, "");
        }
        mEmuSdCard.clear();
        mEmuSdAdoptPending = false;
        return;
    }
    const std::string card = emuSdSlotCard();   // a USB drive is never adopted
    if (card != mEmuSdCard) mEmuSdAdoptPending = false;   // the card being adopted went
    if (card.empty()) { mEmuSdCard.clear(); return; }
    const std::string cardId = card.substr(card.find_last_of('/') + 1);
    char adopted[PROPERTY_VALUE_MAX] = {}, skipped[PROPERTY_VALUE_MAX] = {}, failed[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdCardProp, adopted, "");
    property_get(kEmuSdSkipProp, skipped, "");
    property_get(kEmuSdAdoptFailedProp, failed, "");
    if (card != mEmuSdCard) {
        if (mPs3DlgActive) return;   // asked once the dialog open is closed
        mEmuSdCard = card;
        mEmuSdSyncMs = now;
        if (cardId == skipped || (cardId != adopted && cardId == failed)) return;
        if (cardId == adopted) {   // the card adopted before: kept in step, nothing waits for it
            emuSettingsSyncAsync(card);
            mEmuSdRecheck = true;
            return;
        }
        if (emuSdForeign(card, cardId)) { emuSdForeignDialog(); return; }
        emuSdAdoptBegin(card, false);
        return;
    }
    // Not under a dialog: the clashes one asks about need the move's state as it left it.
    if (mPs3DlgActive) return;
    if (mEmuSdAdoptPending && card == mEmuSdAdoptCard) {
        if (!mEmuSdAdoptSynced) {   // its sync stopped early (the other Nano's move): run again
            if (now - mEmuSdSyncMs >= 3000) {
                mEmuSdSyncMs = now;
                emuSettingsSyncAsync(card, mEmuSdAppWins, true);
            }
            return;
        }
        // Points every emulator at the card (its folders there, else its files moved there), as
        // setup does; nothing to do says nothing. Keeping this device's files is a move as from the
        // menu: the clashes are asked about.
        mSdMoveRoot = card;
        mSdMoveAuto = !mEmuSdAppWins;
        mSdMoveAdopt = !mEmuSdAppWins;
        mEmuSdAdoptingId = cardId;
        emuMoveStart();
        if (mFileJobKind == 4 || !mEmuMoveBusy) {   // started, or nothing to do; else tried again
            mEmuSdAdoptPending = false;
            mEmuSdAppWins = false;
        }
        if (mFileJobKind == 4) {
            mLaunchPending = false;   // its progress is on screen; the game is started again after
        } else {
            mSdMoveAuto = false;
            mSdMoveAdopt = false;
            if (mEmuMoveBusy) mEmuSdAdoptingId.clear();
            else emuSdAdoptDone(!mEmuMoveRefused);   // nothing to move: adopted as it is
        }
        return;
    }
    if (cardId != adopted) return;
    char failedGroups[PROPERTY_VALUE_MAX] = {};
    property_get(kEmuSdFailedProp, failedGroups, "");
    if (mEmuSdRecheck && strcmp(failedGroups, "all") != 0) {
        // After a game (or in a Nano started since): an emulator that ran for the first time made
        // its settings and files on internal storage; they move to the card and it is pointed there.
        // Nothing to do says nothing and stops nothing; launches only wait for a move.
        mEmuSdRecheck = false;
        mSdMoveRoot = card;
        mSdMoveAuto = mSdMoveAdopt = mSdMoveQuiet = true;
        emuMoveStart();
        mSdMoveQuiet = false;
        if (mFileJobKind != 4) {
            mSdMoveAuto = false;
            mSdMoveAdopt = false;
            mEmuSdRecheck = mEmuMoveBusy;   // another move or a sync: looked at again
            if (mEmuMoveRefused) property_set(kEmuSdFailedProp, "all");
        }
        return;
    }
    if (now - mEmuSdSyncMs < 10000) return;
    mEmuSdSyncMs = now;
    emuSettingsSyncAsync(card);
}

// The settings files the move rewrites by full path, besides the emulators' own settings: Flycast's
// emu.cfg and ARMSX2's inis, on the card (emuMoveStart's mSdMoveEmuDirs).
static const char* const kFlycastCfgFiles[] = {"/emu.cfg"};
static const char* const kArmsx2IniFiles[] = {"/PCSX2-Android.ini", "/inis/PCSX2.ini"};

static std::string sdEmuDir(const std::map<std::string, std::string>& dirs, const char* key) {
    auto it = dirs.find(key);
    return it == dirs.end() ? std::string() : it->second;
}

// Flycast's emu.cfg names folders by full path once one is chosen in its settings: the ones in what moved.
static void sdFlycastCfgFollow(const std::map<std::string, std::string>& dirs) {
    const std::string from = sdEmuDir(dirs, "flycast-from"), to = sdEmuDir(dirs, "flycast");
    if (from.empty() || to.empty()) return;
    std::vector<std::pair<std::string, std::string>> map;
    if (!sdEmuDir(dirs, "flycast-own").empty()) map.push_back({from, to});
    else for (const char* part : {"/data", "/mappings", "/textures"}) map.push_back({from + part, to + part});
    for (const char* f : kFlycastCfgFiles) {
        const int err = nanora::raEditFile(to + f, [&](std::string* t) {
            const std::string n = flycastRewriteCfg(*t, map);
            if (n == *t) return false;
            *t = n;
            return true;
        });
        if (err && err != ENOENT) ALOGW("sdmove: Flycast's %s: %s", f + 1, strerror(err));
    }
}

// ARMSX2's settings name a folder by full path only when one was chosen; the BIOS stays in its app
// folder (ARMSX2 sets that path itself when it starts).
static void sdArmsx2IniFollow(const std::map<std::string, std::string>& dirs) {
    const std::string from = sdEmuDir(dirs, "armsx2-from"), to = sdEmuDir(dirs, "armsx2");
    if (from.empty() || to.empty()) return;
    const std::string bios = std::string(appdef::kArmsx2DefDir) + "/bios";
    for (const char* ini : kArmsx2IniFiles) {
        const int err = nanora::raEditFile(to + ini, [&](std::string* t) {
            const std::string n = armsx2RewriteIni(*t, from, to, bios);
            if (n == *t) return false;
            *t = n;
            return true;
        });
        if (err && err != ENOENT) ALOGW("sdmove: ARMSX2's %s: %s", ini + 1, strerror(err));
    }
}

// Worker thread, after a clean copy: points a group's emulator at its folder on the card.
bool NanoMenu::fileJobRelocate(int group) {
    if (mFileJobKind == 3 && group == kGroupEsde) {
        // Nano on the card before an original goes: a crash after this finds it there.
        property_set("persist.gammaos.nano.esde.mediadir", "");
        property_set("persist.gammaos.scraper.dir", (mSdMoveCard + "/nano").c_str());
        property_set("persist.gammaos.nano.esde.dir", mSdMoveCard.c_str());
        return true;
    }
    if (mFileJobKind == 5) {   // Overwrite on SD Card: the restored settings files name the internal folders
        if (group == kGroupRetroArch) return retroarchFolderRewrite();
        if (group == kGroupFlycast) sdFlycastCfgFollow(mSdMoveEmuDirs);
        if (group == kGroupArmsx2) sdArmsx2IniFollow(mSdMoveEmuDirs);
        return true;
    }
    if (mFileJobKind != 4) return true;
    switch (group) {
        case kGroupRetroArch:
            if (!retroarchFolderRewrite()) return false;
            property_set("persist.gammaos.retroarch.dir", mFileJobTo.c_str());
            retroarchConfigFile();   // published for the next home-screen launch
            system("appops set --uid com.retroarch.aarch64 MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1");
            return true;
        case kGroupDrasticNano: {
            auto it = mSdMoveEmuDirs.find("drastic-nano");
            if (it == mSdMoveEmuDirs.end()) return true;
            auto ch = mSdMoveEmuDirs.find("drastic-cheats");
            if (it->second.size() >= PROPERTY_VALUE_MAX
                || (ch != mSdMoveEmuDirs.end() && ch->second.size() >= PROPERTY_VALUE_MAX)) {
                ALOGW("sdmove: the DS emulator's folder is too long a path for its setting");
                return false;
            }
            if (property_set("persist.gammaos.drastic.data_dir", it->second.c_str()) != 0) return false;
            if (ch != mSdMoveEmuDirs.end()) property_set("persist.gammaos.drastic.cheats_dir", ch->second.c_str());
            return true;
        }
        case kGroupPpsspp: {
            auto it = mSdMoveEmuDirs.find("ppsspp");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("org.ppsspp.ppsspp");
            sdMkdirs(it->second + "/PSP");   // PPSSPP asks for a folder again when it is not there
            if (!sdWriteAppFile(kPpssppMemstickFile, it->second)) {
                ALOGW("sdmove: PPSSPP's memory stick setting could not be written");
                return false;
            }
            system("appops set --uid org.ppsspp.ppsspp MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1");
            return true;
        }
        case kGroupFlycast: {
            auto it = mSdMoveEmuDirs.find("flycast");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("com.flycast.emulator");
            sdMkdirs(it->second);
            std::string prefs;
            sdReadPrefs(kFlycastPrefs, &prefs);
            if (!sdPrefsSet(&prefs, "home_directory", it->second + "/") || !sdWriteAppFile(kFlycastPrefs, prefs)) return false;
            system("appops set --uid com.flycast.emulator MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1");
            sdFlycastCfgFollow(mSdMoveEmuDirs);
            return true;
        }
        // The two below reach the card through GammaOS's MediaProvider, which lets every app use any
        // folder by its path; they are pointed at plain folders, not ones picked through Android.
        case kGroupDrastic: {
            auto it = mSdMoveEmuDirs.find("drastic");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("com.dsemu.drastic");
            sdMkdirs(it->second);
            std::string prefs;
            sdReadPrefs(kDrasticFsPrefs, &prefs);
            return sdPrefsSet(&prefs, "_SystemPath", drasticSystemPathValue(it->second)) && sdWriteAppFile(kDrasticFsPrefs, prefs);
        }
        case kGroupMupen: {
            auto it = mSdMoveEmuDirs.find("m64plus");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("org.mupen64plusae.v3.fzurita");
            sdMkdirs(it->second + "/GameData");
            std::string prefs;
            sdReadPrefs(kMupenPrefs, &prefs);
            return sdPrefsSet(&prefs, "gameDataStorageType", "external")
                   && sdPrefsSet(&prefs, "gameDataStoragePath", sdFileUri(it->second))
                   && sdWriteAppFile(kMupenPrefs, prefs);
        }
        case kGroupArmsx2: {
            auto it = mSdMoveEmuDirs.find("armsx2");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("com.armsx2");
            sdMkdirs(it->second);
            std::string prefs;
            sdReadPrefs(kArmsx2Prefs, &prefs);
            if (!sdPrefsSet(&prefs, "systemDir", it->second) || !sdWriteAppFile(kArmsx2Prefs, prefs)) return false;
            sdArmsx2IniFollow(mSdMoveEmuDirs);
            return true;
        }
        case kGroupScummvm: {
            auto it = mSdMoveEmuDirs.find("scummvm");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("org.scummvm.scummvm");
            sdMkdirs(it->second);
            std::string ini;
            if (!nanobios::appFileRead(kScummvmIni, &ini)) return false;
            return sdWriteAppFile(kScummvmIni, scummvmSetSavePath(ini, mSdMoveEmuDirs["scummvm-from"], it->second));
        }
        case kGroupVita3k: {
            auto it = mSdMoveEmuDirs.find("vita3k");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("org.vita3k.emulator");
            sdMkdirs(it->second);
            std::string cfg;
            if (!nanobios::appFileRead(kVita3kConfig, &cfg)) return false;
            return sdWriteAppFile(kVita3kConfig, vita3kSetPrefPath(cfg, it->second));
        }
        case kGroupAzahar: {   // only uses a folder picked through Android: given it as if picked
            auto it = mSdMoveEmuDirs.find("azahar");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("org.azahar_emu.azahar");
            sdMkdirs(it->second);
            const std::string volume = mSdMoveRoot.substr(mSdMoveRoot.find_last_of('/') + 1);
            if (!sdUriGrant(kAzaharPkg, volume, "emulator_config/azahar")) return false;
            std::string prefs;
            sdReadPrefs(kAzaharPrefs, &prefs);
            sdPrefsRemove(&prefs, "LIME3DS_DIRECTORY");   // a different one asks to update the folder
            return sdPrefsSet(&prefs, "CITRA_DIRECTORY", azaharTreeUri(volume, "emulator_config/azahar"))
                   && sdWriteAppFile(kAzaharPrefs, prefs);
        }
        case kGroupSeedless: {   // a folder of its own only as one picked through Android, like Azahar
            auto it = mSdMoveEmuDirs.find("seedlessds");
            if (it == mSdMoveEmuDirs.end()) return true;
            sdStopIfRunning("com.seedlessds.app");
            sdMkdirs(it->second);
            const std::string volume = mSdMoveRoot.substr(mSdMoveRoot.find_last_of('/') + 1);
            if (!sdUriGrant(kSeedlessPkg, volume, "emulator_config/seedlessds")) return false;
            std::string prefs;
            sdReadPrefs(kSeedlessPrefs, &prefs);
            return sdPrefsSetInt(&prefs, "_SaveLocation", kSeedlessSaveFolder)
                   && sdPrefsSet(&prefs, "_SaveFolder", azaharTreeUri(volume, "emulator_config/seedlessds"))
                   && sdWriteAppFile(kSeedlessPrefs, prefs);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// After a move: switch over, then the clashes.
// ---------------------------------------------------------------------------

// The card's copy of a clash is still there as it was kept: only then may its internal copy go.
static bool sdCardCopyThere(const nanofilejob::Conflict& c) {
    struct stat st;
    if (lstat(c.dst.c_str(), &st) != 0) return false;
    return c.dstSize < 0 || (S_ISREG(st.st_mode) && (int64_t)st.st_size == c.dstSize);
}

void NanoMenu::sdMoveFinish(int kind, bool cancelled, int failed) {
    struct AdoptEnd {   // a card adopted (emuSdTick): back to asking once this move is done
        NanoMenu* m;
        ~AdoptEnd() {
            if (m->mSdMoveAdopt) { m->mSdMoveAdopt = false; m->mSdMoveAuto = false; }
            m->mSdMoveQuietJob = false;
        }
    } adoptEnd{this};
    if (kind == 4 && mEmuMoveLockFd >= 0) { close(mEmuMoveLockFd); mEmuMoveLockFd = -1; }   // syncs may run again
    const bool committed = mFileJobCommitted;   // past its copy: the programs use the card now
    // A move from the menu to the slot's card adopts it too (else that card would be asked about),
    // keeping this device's settings: the move promised nothing is overwritten.
    if (kind == 4 && mEmuSdAdoptingId.empty() && !mSdMoveQuietJob && committed && !cancelled && !mSdMoveRoot.empty()
        && mSdMoveRoot == emuSdSlotCard()) {
        mEmuSdAdoptingId = mSdMoveRoot.substr(mSdMoveRoot.find_last_of('/') + 1);
        char adopted[PROPERTY_VALUE_MAX] = {};
        property_get(kEmuSdCardProp, adopted, "");
        if (mEmuSdAdoptingId != adopted) property_set(kEmuSdAppWinsProp, mEmuSdAdoptingId.c_str());
    }
    if (kind == 4) emuSdAdoptDone(committed && !cancelled);
    if (kind == 4 && mSdMoveQuietJob) {   // the look after a game: what failed is not tried again this boot
        char cur[PROPERTY_VALUE_MAX] = {};
        property_get(kEmuSdFailedProp, cur, "");
        std::string list = cur;
        if (cancelled || !committed) list = "all";
        else
            for (int g : mFileJobFailedGroups)
                if (list != "all" && ("," + list + ",").find("," + std::to_string(g) + ",") == std::string::npos)
                    list += (list.empty() ? "" : ",") + std::to_string(g);
        if (list != cur) property_set(kEmuSdFailedProp, list.c_str());
    }
    if (kind == 5) {   // Overwrite on SD Card
        sdPruneEmpty(kBackupRoot);
        if (mSdMoveOverwriteEsde) esdeStoreReload(true);
        photoShowBanner(cancelled || failed ? trDyn("Some files could not be copied to the microSD card.")
                                            : trDyn("The microSD card's copies were replaced."));
        return;
    }
    bool incomplete;
    { std::lock_guard<std::mutex> lk(mScrapeMutex); incomplete = mFileJobIncomplete; }
    if (kind == 3 && !(committed && incomplete)) {   // else what did not reach the card stays there
        sdRemoveTree(kEsdeStage);
        sdRemoveTree(kEsdeStageMerged);
    }
    if (kind == 3) {   // the card's files the merged copies replaced: back, unless the move went through
        if (cancelled || !committed) {
            std::vector<std::string> files;
            sdListFiles(kEsdeCardOrig, std::string(), &files);
            bool restored = true;
            for (const auto& f : files) {
                std::string text;
                restored = sdReadFile(std::string(kEsdeCardOrig) + "/" + f, &text) && sdWriteFile(mSdMoveCard + "/" + f, text)
                           && restored;
            }
            if (restored) sdRemoveTree(kEsdeCardOrig);
            else ALOGW("sdmove: some of the card's ES-DE files could not be put back; they are in %s", kEsdeCardOrig);
        } else {
            sdRemoveTree(kEsdeCardOrig);
        }
    }
    if (cancelled || !committed) {   // the card got back what it had; nothing else changed
        std::string why;
        if (mSdMoveAuto) { std::lock_guard<std::mutex> lk(mScrapeMutex); why = mScrapeError; }   // e.g. no room on the card
        sdMoveNotice(!why.empty() ? why
                     : cancelled ? trDyn("Moving was cancelled. Nothing was changed.")
                                 : trDyn("Some files could not be copied. Nothing was changed."));
        return;
    }
    if (kind == 3 && incomplete) {
        ALOGW("sdmove: ES-DE moved in part; Nano's own files and %s kept", kEsdeStage);
        esdeStoreReload(false);
        esdeAddCardSystems();
    } else if (kind == 3) {
        // Nano's own copies of what is now in the ES-DE folder.
        for (const char* f : {"/data/system/nano_favorites.txt", "/data/system/nano_esde_unfavorites.txt",
                              "/data/system/nano_hidden_games.txt", "/data/system/nano_collections.txt",
                              "/data/system/nano_scrape/names.json", "/data/system/nano_game_emulators.json"})
            unlink(f);
        if (mSdMoveOldScrapeDir != mSdMoveCard + "/nano") unlink((mSdMoveOldScrapeDir + "/index.json").c_str());
        esdeStoreReload(false);
        esdeAddCardSystems();
        ALOGI("sdmove: ES-DE folder now %s (%zu clashes)", mSdMoveCard.c_str(), mFileJobConflicts.size());
    } else if (kind == 4) {
        property_set(kEmuSdProp, "1");   // from now on kept there: a card that comes in is adopted
        const bool raFailed = std::find(mFileJobFailedGroups.begin(), mFileJobFailedGroups.end(), kGroupRetroArch)
                              != mFileJobFailedGroups.end();
        if (mSdMoveRaIncluded && !raFailed) retroarchFolderFinish();
        // BIOS files installed before land where the emulators now look (only if any were installed).
        if (access(kBiosManifestPath, F_OK) == 0) biosReinstallFromManifest();
    }
    std::string problem;   // the move went through, but not all of it
    if (failed > 0) {
        std::lock_guard<std::mutex> lk(mScrapeMutex);
        problem = mScrapeError;
    }
    if (!mFileJobConflicts.empty() && mSdMoveAuto && incomplete) {
        // Setup, a move that stopped part way: the internal copies stay where they are.
        mFileJobConflicts.clear();
        char msg[600];
        snprintf(msg, sizeof(msg), trDyn("Some files did not reach the microSD card. Their internal copies are in %s."),
                 sdShown(mFileJobBackupRoot).c_str());
        sdMoveNotice(msg);
        ALOGW("sdmove: setup: move incomplete, internal copies kept in %s", mFileJobBackupRoot.c_str());
    } else if (!mFileJobConflicts.empty() && mSdMoveQuietJob) {
        // The look after a game: an emulator that ran before it was pointed at the card (its folder
        // did not exist yet) may have saved a game the card also has: the card's copy is used, and
        // the internal one is kept in the backup rather than deleted.
        ALOGW("sdmove: %zu clashes with the card kept in %s", mFileJobConflicts.size(), mFileJobBackupRoot.c_str());
        mFileJobConflicts.clear();
    } else if (!mFileJobConflicts.empty() && mSdMoveAuto) {
        // Setup: the card's copies stay and the internal ones go, without asking; one only goes
        // when the card's copy is there. A clash between two files of this move (sameJob) keeps its
        // backup: that is the only copy of a different file. The next move waits for this.
        std::vector<FileConflict> conflicts;
        conflicts.swap(mFileJobConflicts);
        std::vector<FileConflict> drop;
        size_t kept = 0;
        for (auto& c : conflicts) { if (c.sameJob) kept++; else drop.push_back(std::move(c)); }
        ALOGI("sdmove: setup: %zu clashes with the card deleted, %zu within the move kept", drop.size(), kept);
        if (kept > 0) {
            char msg[600];
            snprintf(msg, sizeof(msg), trDyn("%zu files with the same name as another moved file were kept in %s."),
                     kept, sdShown(mFileJobBackupRoot).c_str());
            sdMoveNotice(msg);
        }
        const std::string root = mFileJobBackupRoot;
        auto state = std::make_shared<SdAutoDelState>();
        mSdAutoDel.st = state;
        std::thread([state, drop, root]() {
            for (const auto& c : drop) {
                if (state->cancel.load()) break;   // Nano is going: the rest stays in the backup
                if (sdCardCopyThere(c)) unlink(c.backup.c_str());
            }
            if (!state->cancel.load()) {
                sdPruneEmpty(root);              // this move's part of Nano Backup
                rmdir(kBackupRoot);              // and Nano Backup itself once nothing is left in it
            }
            state->done.store(true);
        }).detach();
    }
    if (!mFileJobConflicts.empty()) {
        sdMoveConflictDialog(kind, problem);
        return;
    }
    if (mSdMoveAuto) {
        sdMoveNotice(kind == 3 ? trDyn("ES-DE moved to the microSD card") : trDyn("Emulator settings moved to the microSD card"));
        sdMoveNotice(problem);
    } else if (!problem.empty()) {
        photoShowBanner(problem.c_str());
    } else if (kind == 3 && packageInstalled(kEsdePkg)) {
        feInfoDialog("ES-DE Moved", trDyn("ES-DE asks for its folder the next time it starts: choose ES-DE on the microSD card."));
    } else {
        photoShowBanner(kind == 3 ? trDyn("ES-DE moved to the microSD card") : trDyn("Emulator settings moved to the microSD card"));
    }
}

// The clashes of a move (mFileJobConflicts): where their internal copies are, and what to do with them.
void NanoMenu::sdMoveConflictDialog(int kind, const std::string& problem) {
    mSdMoveConflictKind = kind;
    // Clashes with a file the card had, and between two files of this move (sameJob: the backup is
    // the only copy of the other one, which no choice here deletes).
    size_t same = 0;
    for (const auto& c : mFileJobConflicts) if (c.sameJob) same++;
    const size_t card = mFileJobConflicts.size() - same;
    char body[700];
    std::string text;
    if (card > 0) {
        snprintf(body, sizeof(body),
                 trDyn("Not all files were moved: %zu clashed with a file on the microSD card, so the card's copy was "
                       "kept. The internal copies are in %s."),
                 card, sdShown(mFileJobBackupRoot).c_str());
        text = body;
    }
    if (same > 0) {
        snprintf(body, sizeof(body),
                 trDyn("%zu files had the same name as another moved file. They are kept in %s, whatever you choose."),
                 same, sdShown(mFileJobBackupRoot).c_str());
        text += (text.empty() ? "" : " ") + std::string(body);
    }
    if (!problem.empty()) text += " " + problem;
    if (kind == 3 && packageInstalled(kEsdePkg))
        text += std::string(" ") + trDyn("ES-DE asks for its folder the next time it starts: choose ES-DE on the microSD card.");
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 1; mPs3DlgThemeKey = 65; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    mPs3DlgTitle = "Not All Files Were Moved"; mPs3DlgBody = text;
    mPs3DlgOptions.push_back("Keep Files");             mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Delete Internal Copies"); mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back("Overwrite on SD Card");   mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(62);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
}

// The clash prompt (theme key 65): 0 Keep Files, 1 Delete Internal Copies, 2 Overwrite on SD Card.
void NanoMenu::sdMoveConflictChoice(int sel) {
    std::vector<FileConflict> conflicts;
    conflicts.swap(mFileJobConflicts);
    if (sel == 1) {
        std::thread([conflicts]() {
            // A clash within the move keeps its backup: the only copy of a different file. So does
            // one whose card copy has gone or changed since (the card pulled out meanwhile?).
            for (const auto& c : conflicts) if (!c.sameJob && sdCardCopyThere(c)) unlink(c.backup.c_str());
            sdPruneEmpty(kBackupRoot);   // the backup folders left empty
        }).detach();
        photoShowBanner(trDyn("Internal copies deleted"));
    } else if (sel == 2) {
        // Only over what the card had before: a clash between two files of this move keeps the one
        // copied (its original is gone), and the other stays in the backup.
        mSdMoveOverwriteEsde = mSdMoveConflictKind == 3;
        mSdMoveOverwriteRa = mSdMoveConflictKind == 4 && mSdMoveRaIncluded
                             && std::find(mFileJobFailedGroups.begin(), mFileJobFailedGroups.end(), kGroupRetroArch)
                                    == mFileJobFailedGroups.end();
        std::vector<FileCopyPair> pairs;
        std::vector<FileConflict> kept;
        for (const auto& c : conflicts) {
            if (c.sameJob) { kept.push_back(c); continue; }
            FileCopyPair p{c.backup, c.dst};
            p.overwrite = true;
            p.move = true;
            // A restored RetroArch file names the internal folders: the relocation rewrites its configs
            // again (worker thread) before the backup copies go.
            if (mSdMoveOverwriteRa && !mFileJobTo.empty() && c.dst.compare(0, mFileJobTo.size() + 1, mFileJobTo + "/") == 0)
                p.group = kGroupRetroArch;
            if (mSdMoveConflictKind == 4) {   // so do Flycast's and ARMSX2's
                const std::string fly = sdEmuDir(mSdMoveEmuDirs, "flycast"), ps2 = sdEmuDir(mSdMoveEmuDirs, "armsx2");
                for (const char* f : kFlycastCfgFiles) if (!fly.empty() && c.dst == fly + f) p.group = kGroupFlycast;
                for (const char* f : kArmsx2IniFiles) if (!ps2.empty() && c.dst == ps2 + f) p.group = kGroupArmsx2;
            }
            pairs.push_back(p);
        }
        if (pairs.empty()) { photoShowBanner(trDyn("Internal copies kept in Nano Backup")); return; }
        mFileJobStopPkgs.clear();
        if (mSdMoveConflictKind == 4)
            for (const char* pkg : {"com.retroarch.aarch64", kPpssppPkg, kFlycastPkg, kDrasticPkg, kMupenPkg, kArmsx2Pkg,
                                    kScummvmPkg, kVita3kPkg, kDolphinPkg, kAzaharPkg, kHakuxPkg, kSeedlessPkg, kWatermelonPkg})
                mFileJobStopPkgs.push_back(pkg);
        else
            mFileJobStopPkgs.push_back(kEsdePkg);
        if (!fileCopyStart(5, "Overwrite on SD Card", std::move(pairs))) {   // the choice again
            mFileJobStopPkgs.clear();
            mFileJobConflicts.swap(conflicts);
            sdMoveConflictDialog(mSdMoveConflictKind, std::string());
            mPs3DlgKeepOpen = true;   // opened from the clash dialog's accept
        } else if (!kept.empty()) ALOGI("sdmove: %zu clashes within the move stay in the backup", kept.size());
    } else {
        photoShowBanner(trDyn("Internal copies kept in Nano Backup"));
    }
}

void NanoMenu::openConfirmDialog(int key, const char* title, const std::string& body, const char* yes) {
    mPs3DlgOptions.clear(); mPs3DlgSwatch.clear();
    mPs3DlgSlider = false; mPs3DlgKind = 0; mPs3DlgType = 1; mPs3DlgThemeKey = key; mPs3DlgBinding = nullptr;
    mPs3DlgIllust = 0; mPs3DlgNotice.clear(); mPs3DlgRomInfo = false; mPs3DlgAppInfo = false;
    mPs3DlgTitle = title; mPs3DlgBody = body;
    mPs3DlgOptions.push_back("Cancel"); mPs3DlgSwatch.push_back(-1);
    mPs3DlgOptions.push_back(yes);      mPs3DlgSwatch.push_back(-1);
    mPs3DlgSel = 0; mPs3DlgOrigSel = 0;
    mPs3DlgIconTex = 0; mPs3DlgIconNmap = nmapForIcon(62);
    mPs3DlgIconR = mPs3DlgIconG = mPs3DlgIconB = 1.0f;
    mPs3DlgActive = true; mPs3DlgAnim = 0.0f; mPs3DlgClosing = false; mPs3DlgBlurValid = false;
}

}  // namespace android
