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

// The BIOS installer: every file of a chosen folder whose size is a catalogue entry's is hashed,
// recognised by its SHA-1 (whatever its name), and copied where each emulator reads it, under the
// name it needs. Arcade BIOS zips are recognised by their members, a PS2 BIOS also by its ROMDIR, a
// PS Vita update package by its header (Vita3K installs it itself), and a few console-unique files
// by name and size. Nothing is overwritten. A worker thread does it all; the UI reads
// biosInstallProgress().
//
// Side effects outside Nano (am, prefs and config edits, chown/restorecon, an emulator's folders)
// happen only for an emulator that gets a new file in this run, and never for one not installed.
// /data/system/nano_bios/installed.json remembers what went where, so biosReinstallFromManifest()
// can put back files that are missing after Move Emulator Files to microSD.

#define LOG_TAG "NanoMenu"

#include "NanoMenu.h"
#include "NanoBiosCatalog.h"
#include "NanoI18n.h"
#include "NanoJson.h"
#include "NanoMenuStrings.h"
#include "NanoBiosWorker.h"
#include "NanoSdMove.h"

#include <cutils/properties.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <thread>

namespace android {

namespace {

const char* const kManifest = nanobios::kManifestPath;
const char* const kReinstallMarker = nanobios::kReinstallMarker;

bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

// An English format string, translated, filled in.
std::string tfmt(const char* english, ...) __attribute__((format(printf, 1, 2)));
std::string tfmt(const char* english, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, english);
    vsnprintf(buf, sizeof(buf), trDyn(english), ap);
    va_end(ap);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// The dialog
// ---------------------------------------------------------------------------

static nanobios::Region biosRegion() {
    char loc[PROPERTY_VALUE_MAX] = {};
    property_get("persist.sys.locale", loc, "");
    std::string lang = loc, region;
    const size_t dash = lang.find('-');
    if (dash != std::string::npos) {
        region = lang.substr(dash + 1);
        lang = lang.substr(0, dash);
        const size_t more = region.find('-');   // zh-Hant-TW: the last part is the region
        if (more != std::string::npos) region = region.substr(region.find_last_of('-') + 1);
    }
    if (lang.empty()) {
        const LocaleInfo& li = nanoGetLocaleInfo(nanoGetLocale());
        lang = li.code;
        region = li.regionCode;
    }
    return nanobios::regionForLocale(lang, region);
}

std::vector<NanoMenu::BiosDialogRow> NanoMenu::biosDialogRows() const {
    const int bit = nanobios::regionBit(biosRegion());
    std::vector<BiosDialogRow> out;
    for (int pass = 0; pass < 2; pass++) {   // required (and for some games), then optional
        for (const auto& r : nanobios::rows()) {
            if (!r.listed || (pass == 0) != (r.section != nanobios::SEC_OPTIONAL)) continue;
            BiosDialogRow row;
            row.required = r.section != nanobios::SEC_OPTIONAL;
            row.section = r.section;
            row.id = r.id;
            row.qualifier = r.qualifier;
            row.system = r.system;
            row.emulators = r.emulators;
            row.note = r.note;
            for (const auto& f : nanobios::files()) {
                if (strcmp(f.row, r.id) || f.match == nanobios::MATCH_COMPANION) continue;
                BiosDialogFile df;
                df.name = f.name;
                df.sha1 = f.sha1;
                df.desc = f.desc;
                if (f.match == nanobios::MATCH_PS2ROMDIR) { df.name = "Any other PS2 BIOS"; df.desc = "Checked by ARMSX2"; }
                (f.rec & bit ? row.recommended : row.alternates).push_back(df);
            }
            out.push_back(std::move(row));
        }
    }
    return out;
}

std::string NanoMenu::biosNeedNothingLine() const { return nanobios::needNothingLine(); }

std::vector<NanoMenu::BiosAllGroup> NanoMenu::biosAllFiles() const {
    std::vector<BiosAllGroup> out;
    for (const auto& g : nanobios::allGroups()) {
        BiosAllGroup bg;
        bg.system = trDyn(g.system.c_str());
        bg.emulators = trDyn(g.emulators.c_str());
        int lastSec = -1;
        for (const nanobios::Row* r : g.rows) {   // "Required. <note> Optional. <note>"
            std::string part;
            if (r->section == nanobios::SEC_SOME_GAMES) part = tfmt("Required for %s.", trDyn(r->qualifier));
            else if (r->section != lastSec) part = trDyn(r->section == nanobios::SEC_REQUIRED ? "Required." : "Optional.");
            lastSec = r->section;
            if (*r->note) part += (part.empty() ? "" : " ") + std::string(trDyn(r->note));
            if (!part.empty()) bg.note += (bg.note.empty() ? "" : " ") + part;
        }
        for (const auto& f : g.files) {
            BiosAllFile bf;
            bf.name = f.name + (f.desc.empty() ? std::string() : " (" + std::string(trDyn(f.desc.c_str())) + ")");
            bf.size = trDyn(f.size.c_str());
            if (f.file->match == nanobios::MATCH_NAME || f.file->match == nanobios::MATCH_SHA1 || f.file->match == nanobios::MATCH_MD5)
                bf.size = f.size;   // a number and unit
            bf.check = f.file->match == nanobios::MATCH_SHA1 ? f.check
                     : f.file->match == nanobios::MATCH_MD5 ? f.check : std::string(trDyn(f.check.c_str()));
            bf.how = trDyn(f.how.c_str());
            bg.files.push_back(std::move(bf));
        }
        out.push_back(std::move(bg));
    }
    return out;
}

std::string NanoMenu::biosAllFilesIntro() const { return trDyn(nanobios::allFilesIntro()); }

std::string NanoMenu::biosDefaultFolder() const {
    const std::string card = sdCardRoot();
    if (card.empty()) return std::string();
    std::string found;
    if (DIR* d = opendir(card.c_str())) {
        while (dirent* e = readdir(d)) {
            if (strcasecmp(e->d_name, "bios") == 0 && isDir(card + "/" + e->d_name)) { found = card + "/" + e->d_name; break; }
        }
        closedir(d);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Starting
// ---------------------------------------------------------------------------

bool NanoMenu::biosInstallStart(const std::string& folder) {
    std::lock_guard<std::mutex> lk(mBios->mu);
    if (mBios->busy) return false;
    mBios->busy = true;
    mBios->running = true;
    mBios->cancel = false;
    mBios->progress = BiosInstallProgress();
    mBios->progress.running = true;
    std::thread(&NanoMenu::biosWorker, mBios, folder, false, retroarchConfigFile()).detach();
    return true;
}

void NanoMenu::biosInstallCancel() { mBios->cancel = true; }

void NanoMenu::biosReinstallFromManifest() {
    // Remembered until it has run, so one an exit cuts short runs at the next start.
    if (access(kManifest, F_OK) == 0 && access(kReinstallMarker, F_OK) != 0) {
        const int fd = open(kReinstallMarker, O_WRONLY | O_CREAT | O_CLOEXEC, 0660);
        if (fd >= 0) close(fd);
    }
    const std::string ra = retroarchConfigFile();
    std::lock_guard<std::mutex> lk(mBios->mu);
    if (mBios->closing) return;
    if (mBios->busy) {   // runs when the current one ends
        mBios->reinstallQueued = true;
        mBios->queuedRaConfig = ra;
        return;
    }
    mBios->busy = true;
    mBios->running = true;
    mBios->cancel = false;
    std::thread(&NanoMenu::biosWorker, mBios, std::string(), true, ra).detach();
}

// Once per start, when storage is up: a reinstall left pending runs (copies only what is missing).
void NanoMenu::biosStartupTick() {
    if (mBiosStartupChecked || mOverlayMode) return;
    if (access(kReinstallMarker, F_OK) != 0) { mBiosStartupChecked = true; return; }
    if (!isLaunchReady()) return;
    mBiosStartupChecked = true;
    if (access(kManifest, F_OK) == 0) biosReinstallFromManifest();
    else unlink(kReinstallMarker);
}

NanoMenu::BiosInstallProgress NanoMenu::biosInstallProgress() {
    std::lock_guard<std::mutex> lk(mBios->mu);
    return mBios->progress;
}


// ---------------------------------------------------------------------------
// The worker (NanoBiosWorker.cpp)
// ---------------------------------------------------------------------------

void NanoMenu::biosWorker(std::shared_ptr<BiosShared> sh, std::string folder, bool fromManifest, std::string raConfig) {
    nanobios::Env env;
    env.run = [](const std::vector<std::string>& args, std::string* out, int timeoutMs) { return runArgv(args, out, timeoutMs); };
    env.region = biosRegion();
    env.wake = []() { FrameFlag::wake(); };
    env.wakeThrottled = []() { FrameFlag::wakeThrottled(); };
    std::string next;
    // A queued reinstall runs next on a thread of its own, as a new run.
    if (nanobios::runInstall(env, *sh, folder, fromManifest, raConfig, &next))
        std::thread(&NanoMenu::biosWorker, sh, std::string(), true, next).detach();
}

}  // namespace android
