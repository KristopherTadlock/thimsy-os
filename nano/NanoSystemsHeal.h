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

// The game systems Nano falls back to, and the repair of a systems config that holds them where the
// build ships other cores (NanoMenuXmb.cpp). No Android or NanoMenu dependencies, so host tests can
// use it.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace nanosys {

// The build's game systems, which first-boot setup copies to /data/system/nano_systems.json. Only
// some builds have it; without it Nano's own table is the default.
inline constexpr const char* kBundledSystems = "/system/etc/gammaos/nano_systems.json";

// Equivalent ROM folder names across nano / Daijishou / ES-DE / RetroArch, so a system's ROMs are
// found regardless of which folder-naming convention the user organized by. ES-DE for example uses
// "megadrive" for Genesis and "n3ds" for 3DS. Returns romDir as given plus the other names of its
// group in lowercase. (Folder list from retrogamecorps/ES-DE-Directories + the libretro/Daijishou
// conventions.)
std::vector<std::string> romFolderAliases(const std::string& romDir);

// The catalog platform the folder auto-add makes of a ROM folder: the platform named like the folder,
// else the first (in platformIds order) of its romFolderAliases. platformIds are lowercase; the
// result indexes them, -1 when none matches.
int platformForFolder(const std::string& folder, const std::vector<std::string>& platformIds);

// Folder names Nano's own systems scanned as another's through test 27 (Satellaview, Sufami Turbo and
// Super Game Boy games as SNES ones, 64DD as N64, Neo Geo CD as Neo Geo, SuperGrafx as TurboGrafx-16,
// ZX81 as ZX Spectrum) and add-ons without a system of their own then, each now in an alias group of
// its own (its own catalog platform's), with the names of those groups.
const std::vector<std::string>& splitOffFolders();

// True when the folder holds, directly or one folder down, a file with one of acceptExts
// (comma-separated, dotted: ".sfc,.bs", any case) or a common ROM archive (.zip, .7z, .chd). Bounded,
// so a large or networked folder cannot stall: ES-DE's rule that a folder is a system only when it
// holds games.
bool folderHasRom(const std::string& path, const std::string& acceptExts);

// Every drive vold has mounted (a folder in mediaRw, /mnt/media_rw) is mounted for apps too: its
// /storage/<vol> is a mount point in mounts (/proc/self/mounts' text). The apps' mount can come
// seconds after the raw one, and until then the drive's folders are not seen under /storage.
bool drivesServed(const std::string& mediaRw, const std::string& mounts);

// A split-off folder right under a ROM root, and its catalog platform (platformForFolder).
struct FoundFolder {
    std::string name;
    std::string path;
    int platform = -1;
};

// The split-off folders (any case) right under roots whose platform is in platformIds and which hold
// its games (folderHasRom with extsFor(platform)).
std::vector<FoundFolder> findSplitOffFolders(const std::vector<std::string>& roots,
                                             const std::vector<std::string>& platformIds,
                                             const std::function<std::string(int)>& extsFor);

// What the repair needs to know of a system.
struct SysCore {
    std::string id;
    std::string romDir;
    std::string shortname;
    bool builtin = false;
    bool libretro = false;   // a RetroArch core launch (else an app's)
    std::string coreSo;
    std::string package;                // the app it starts (not libretro)
    std::vector<std::string> folders;   // the folders it scans, when chosen (absolute paths)
};

// Two systems for the same games: the same id, folders of one alias group (romFolderAliases of the
// id or the folder, so a system the emulator catalog made keeps matching: "coleco" is ColecoVision,
// "tgcd" TurboGrafx-CD), the same ES-DE system (esdesys::esdePlatformSystemName: "3ds" is n3ds), a
// chosen folder named like the other's, or chosen folders one inside the other.
bool sameSystem(const SysCore& a, const SysCore& b);

struct CoreRepair {
    size_t index;         // into the systems given
    std::string coreSo;   // the build's core for it
};

// Nano's own table names cores a build need not ship (Nestopia for NES, Gambatte for Game Boy, where
// Thimsy ships Mesen and SameBoy). A Nano that started before setup copied the build's config held
// that table, and anything it saved then replaced the build's config with it. A built-in system
// still on its core from Nano's table (builtinCores: id -> core), with that core not installed, gets
// the build's core for the same system, when that one is installed. coreInstalled is asked only
// about the cores of such systems.
std::vector<CoreRepair> staleCoreRepairs(const std::vector<SysCore>& systems, const std::vector<SysCore>& bundled,
                                         const std::vector<std::pair<std::string, std::string>>& builtinCores,
                                         const std::function<bool(const std::string&)>& coreInstalled);

// The build's built-in systems that no system is the same as (sameSystem), as indexes into bundled.
std::vector<size_t> missingBundledSystems(const std::vector<SysCore>& systems, const std::vector<SysCore>& bundled);

struct SplitAdd {
    int platform;                       // into the platformIds given
    std::vector<std::string> folders;   // its found folders, each a scan source
};

// The systems the split-off folders heal adds: one per platform of found, with its folders that no
// system scans already (sameSystem with a system of the platform choosing the folder: one of the same
// platform, one that chose the folder or a folder around it). Platforms in order of first appearance.
std::vector<SplitAdd> splitFolderAdds(const std::vector<SysCore>& systems, const std::vector<FoundFolder>& found,
                                      const std::vector<std::string>& platformIds);

// One of a platform's players in Nano's emulator catalog (ps3xmb/daijishou), as it launches.
struct Player {
    std::string name;   // the catalog's player name ("RetroArch 64 - mednafen_wswan", "SwanEmu")
    bool libretro = false;
    std::string coreSo;
    std::string package;
};

// How the emulator picker shows a player: "RetroArch 64 - mgba" -> "mgba (RetroArch)".
std::string playerLabel(const std::string& name);

// A platform's players in the emulator picker's order: those that run (all of them, *noneReady set,
// when none does), RetroArch cores first, then by label (case-insensitive).
std::vector<size_t> playerChoices(const std::vector<Player>& players, const std::function<bool(const Player&)>& runs,
                                  bool* noneReady);

// A system the emulator catalog made (not built in) that starts one of its platform's players, which
// cannot run here (its core is not in RetroArch's cores, its app is not installed): the player it
// gets, the first in playerChoices order that runs, as an index into players. -1 for any other system,
// and when no player runs. coreInstalled is false for every core before the user is unlocked, so the
// caller waits for that first.
int playerRepair(const SysCore& system, const std::vector<Player>& players,
                 const std::function<bool(const std::string&)>& coreInstalled,
                 const std::function<bool(const std::string&)>& packageInstalled);

}  // namespace nanosys
}  // namespace android
