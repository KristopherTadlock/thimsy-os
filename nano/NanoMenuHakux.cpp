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

// hakuX's games folders kept to Game Settings (NanoHakuxSetup.h). First-boot setup gives hakuX its
// Xbox folder (the build's mods/47-hakux-folder.sh); this follows the Xbox system's folders after:
// after every systems save, and when the home starts (at boot and after every game: a card put in
// or taken out since), as retroarchHealStart does for RetroArch's config.

#define LOG_TAG "NanoMenu"

#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cutils/properties.h>
#include <utils/Log.h>

#include "NanoHakuxSetup.h"
#include "NanoMenu.h"
#include "NanoMenuUtils.h"
#include "NanoSdMove.h"
#include "NanoSystemsHeal.h"

namespace android {

bool nanoNetBridgeUp();   // NanoMenuSettings.cpp

namespace {

struct Request {
    std::vector<NanoMenu::XmbSystem> systems;   // the enabled systems hakuX plays
    std::string key;
};

std::mutex sLock;
bool sBusy = false;            // guarded by sLock
bool sPending = false;         // a request came while one ran: sNext runs after it
Request sNext;
std::string sDoneKey;          // the settings last followed in this process

// What the folders follow of a system: an edit to any of it changes the key.
std::string systemKey(const NanoMenu::XmbSystem& s) {
    std::string k = s.id + "|" + s.romDir + "|";
    for (const auto& src : s.scanSources) k += std::to_string(src.type) + ":" + src.value + ":" + src.rawHint + ";";
    k += "|";
    for (const auto& d : s.disabledDefaultFolders) k += d + ";";
    return k + "\n";
}

// Storage, the drives apps see and the bridge are up (a cold boot: the home starts before them).
bool readyForFolders() {
    for (int i = 0; i < 120; i++) {
        if (property_get_bool("sys.boot_completed", false) && isPrimaryStorageReady()
            && nanosys::drivesServed("/mnt/media_rw", readSelfMounts()) && nanoNetBridgeUp())
            return true;
        sleep(1);
    }
    return false;
}

}  // namespace

void NanoMenu::hakuxFoldersFollow() {
    struct stat st;
    if (lstat(nanohakux::kAppDir, &st) != 0 || !S_ISDIR(st.st_mode)) return;   // no hakuX
    Request req;
    for (const auto& s : mXmbSystems) {
        if (!s.enabled || s.launchPkg != nanohakux::kPkg) continue;
        req.systems.push_back(s);
        req.key += systemKey(s);
    }
    req.key += readSelfMounts();   // a card put in or taken out since
    {
        std::lock_guard<std::mutex> lk(sLock);
        if (req.key == sDoneKey) return;
        if (sBusy) {
            sNext = std::move(req);
            sPending = true;
            return;
        }
        sBusy = true;
    }
    std::thread([this, req]() mutable {
        nanoThreadNormalPriority();
        for (;;) {
            if (readyForFolders()) {
                nanohakux::Settings settings;
                for (const auto& s : req.systems) {
                    for (const auto& c : buildScanCandidates(s)) settings.candidates.push_back(c);
                    if (s.scanSources.empty()) {
                        settings.defaultScan = true;
                        settings.makeable.push_back("/data/media/0/ROMs/" + s.romDir);
                    }
                }
                nanohakux::Env env;
                env.read = [](const std::string& p, std::string* out) { return sdReadPrefs(p, out); };
                env.write = [](const std::string& p, const std::string& text) { return sdWriteAppFile(p, text); };
                env.grant = [](const nanohakux::Folder& f) { return sdUriGrant(nanohakux::kPkg, f.volume, f.rel); };
                env.revoke = [](const nanohakux::Folder& f) { return sdUriRevoke(nanohakux::kPkg, f.volume, f.rel); };
                env.running = [](const std::string& pkg) { return nanohakux::packageRunning(pkg); };
                env.mounted = [](const std::string& v) {
                    struct stat vs;
                    return v == "primary" || (stat(("/mnt/media_rw/" + v).c_str(), &vs) == 0 && S_ISDIR(vs.st_mode));
                };
                env.exists = [](const std::string& p) {
                    struct stat ps;
                    return stat(p.c_str(), &ps) == 0 && S_ISDIR(ps.st_mode);
                };
                const nanohakux::Outcome r = nanohakux::sync(env, settings);
                // Done or not installed: nothing more until the settings or the drives change. hakuX
                // open, or a failure: the next save or home start tries again.
                if (r == nanohakux::Outcome::kDone || r == nanohakux::Outcome::kNotInstalled) {
                    std::lock_guard<std::mutex> lk(sLock);
                    sDoneKey = req.key;
                }
            }
            std::lock_guard<std::mutex> lk(sLock);
            if (!sPending) {
                sBusy = false;
                return;
            }
            req = std::move(sNext);
            sPending = false;
        }
    }).detach();
}

}  // namespace android
