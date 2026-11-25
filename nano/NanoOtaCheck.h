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
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Self-contained GammaOS OTA online update-check and download for gammaos-nano.
//
// Ports the check/compare logic from the in-tree LineageOS-derived Updater app
// (packages/apps/Updater: misc/Utils.java getServerURL()/isCompatible()/canInstall()
// and misc/Constants.java). The OTA server speaks the LineageOS Updater JSON API:
//
//   GET https://ota.gammaos.sh/api/v1/{ro.gammaos.device}/{ro.gammaos.variant}
//   -> {"response":[{"datetime":<utc seconds>,"filename":"...","id":"...",
//                    "romtype":"unofficial","size":<bytes>,"url":"...","version":"1.3.2"}]}
//
// Dependency-light by design: fetches with a fork+exec of the on-device curl and
// hand-rolls a tiny JSON scan (no libcurl / no JSON library), mirroring how nano
// already shells out to curl elsewhere.

#pragma once

#include <algorithm>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <functional>

namespace nano {

// Off until Thimsy has its own update server (the GammaOS one above is kept in NanoOtaCheck.cpp
// for then). While off, nano never contacts an update server: the two calls below fail without
// doing anything, and System Update goes straight to choosing a package from storage media.
constexpr bool kOnlineOtaEnabled = false;

struct OtaUpdateInfo {
    bool        available = false;      // a build newer than current exists for this device/variant
    std::string version;                // e.g. "1.3.2"
    std::string filename;
    std::string url;                    // download URL
    uint64_t    size = 0;               // bytes
    std::string currentVersion;         // human-readable current OS version
    std::string error;                  // non-empty on query failure
};

// Query the OTA server for the latest build for this device+variant.
// Returns true if the query SUCCEEDED (info.available says whether an update exists).
// Returns false if the query failed (info.error set).
bool otaCheckForUpdate(OtaUpdateInfo& info);

// Download info.url into /data/gammaos_ota/ and extract into /data/gammaos_ota/package/
// (so that /data/gammaos_ota/package/manifest.json exists). Calls progress(percent 0..100,
// phaseText) periodically. Returns true on success (package staged), false on failure.
bool otaDownloadAndStage(const OtaUpdateInfo& info,
                         const std::function<void(int, const char*)>& progress);

// Compare dotted versions numerically, part by part ("1.10" is newer than "1.9"; "1.2" equals
// "1.2.0"). A part's leading digits are its number; what follows them ("1.3b") breaks a tie as
// text. Returns <0, 0 or >0.
inline int otaCompareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        const size_t ie = std::min(a.find('.', i), a.size()), je = std::min(b.find('.', j), b.size());
        const std::string pa = i < a.size() ? a.substr(i, ie - i) : std::string();
        const std::string pb = j < b.size() ? b.substr(j, je - j) : std::string();
        size_t da = 0, db = 0;
        while (da < pa.size() && pa[da] >= '0' && pa[da] <= '9') da++;
        while (db < pb.size() && pb[db] >= '0' && pb[db] <= '9') db++;
        // Compared as digit strings, so a part longer than an integer cannot overflow.
        std::string na = pa.substr(0, da), nb = pb.substr(0, db);
        na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
        nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
        if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
        if (int c = na.compare(nb)) return c < 0 ? -1 : 1;
        if (int c = pa.compare(da, std::string::npos, pb, db, std::string::npos)) return c < 0 ? -1 : 1;
        i = ie + 1;
        j = je + 1;
    }
    return 0;
}

// The private, root-only folder an update package is unpacked into before it is confirmed and
// flashed (the flasher reads it as a pre-extracted package and checks every image against its
// manifest.json). A package on storage media could otherwise be replaced between the check
// shown to the user and the flash.
extern const char* const kOtaPackageDir;

enum class OtaUnpackResult { Ok, NoSpace, Invalid, Cancelled, Failed };
struct OtaUnpackStatus {
    OtaUnpackResult result = OtaUnpackResult::Failed;
    uint64_t needBytes = 0;   // NoSpace: what the unpacked package needs
    uint64_t freeBytes = 0;   // NoSpace: what /data has
};

// Unpack zip into kOtaPackageDir, replacing what is there. progress gets 0..100; cancelled is
// polled and stops the copy. On anything but Ok the folder is left empty. Worker thread only.
OtaUnpackStatus otaUnpackPackage(const std::string& zip, const std::function<bool()>& cancelled,
                                 const std::function<void(int)>& progress);

// Delete kOtaPackageDir's contents.
void otaRemovePackage();

// Startup: delete a package a failed or abandoned update left behind, unless the flasher is
// running (it may still be reading it).
void otaDropStalePackage();

}  // namespace nano
