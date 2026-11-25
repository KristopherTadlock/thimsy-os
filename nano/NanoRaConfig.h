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

// RetroArch's folders and the rewrite of its config files when they move (NanoMenuPS3Folder.cpp).
// No Android UI code, so host tests can use it.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace nanora {

using PathMap = std::vector<std::pair<std::string, std::string>>;

inline constexpr const char* kRaPkgData = "/data/user/0/com.retroarch.aarch64";
inline constexpr const char* kRaDefSd = "/storage/emulated/0/RetroArch";
inline constexpr const char* kRaDefCfgDir = "/storage/emulated/0/Android/data/com.retroarch.aarch64/files";
inline constexpr const char* kRaDefThumbs = "/storage/emulated/0/RetroArch/thumbnails";
// The default config as root reaches it without FUSE.
inline constexpr const char* kRaDefCfgRaw = "/data/media/0/Android/data/com.retroarch.aarch64/files/retroarch.cfg";

// A part of RetroArch's folder at location `loc` ("" = its defaults); part "" = the folder itself.
std::string raPart(const std::string& loc, const char* part);
std::string raCfgDir(const std::string& loc);   // the folder holding retroarch.cfg
// The folder settings every RetroArch config gets for location `loc`.
PathMap raDirKeys(const std::string& loc);
// The folder settings a move from `from` to `to` writes (box art stays on internal storage).
PathMap raMoveKeys(const std::string& from, const std::string& to);
// Old path prefix -> new, longest first.
PathMap raPrefixMap(const std::string& from, const std::string& to);
// Rewrites a config or shader preset in place: every quoted path under an old prefix moves to the new
// one, and (forceKeys) the folder settings are set. A path matches in any of Android's spellings of
// the same storage folder (/sdcard, /storage/self/primary, /data/media/0, /mnt/media_rw/<vol>), in
// any letter case (storage is case-insensitive), and is written in the /storage form apps use.
// The file keeps its owner, group and mode. 0 (also when nothing changed), or the errno: ENOENT for
// a missing file (created from forceKeys alone when `create`), ELOOP for a link in the file's place
// or its folder's, EINVAL for anything but a file, EFBIG for one over 16 MiB.
int raRewriteFileEx(const std::string& path, const PathMap& map, const PathMap* forceKeys, bool create = false);
bool raRewriteFile(const std::string& path, const PathMap& map, const PathMap* forceKeys);
// Every config, preset, core options file and playlist (its content paths) under dir.
void raRewriteTree(const std::string& dir, const PathMap& map);
// The keys of a config that still name a folder the map moves, and (forceKeys) the folder settings
// that are missing, empty or "default". Empty once raRewriteFileEx has run with the same arguments.
std::vector<std::string> raStaleKeys(const std::string& text, const PathMap& map, const PathMap* forceKeys);
// The same for a file: 0, or the errno of reading it (as raRewriteFileEx).
int raStaleKeysIn(const std::string& path, const PathMap& map, const PathMap* forceKeys, std::vector<std::string>* keys);
// A config whose folder settings still point at RetroArch's internal folders (a session that could
// not read it saved RetroArch's defaults there, say) pointed at `dir` again. 0 or the errno; *fixed
// gets the keys that were stale.
int raHealConfig(const std::string& path, const std::string& dir, std::vector<std::string>* fixed);
// Rewrites a text file in place as raRewriteFileEx does (same checks, owner, group and mode kept):
// edit changes the text and returns whether it did. 0 or the errno.
int raEditFile(const std::string& path, const std::function<bool(std::string*)>& edit);
// Gives a root-owned config in RetroArch's own folder back to the folder's owner. true when changed.
bool raRepairOwner(const std::string& path);

// Held by everything that rewrites RetroArch's configs (these and the hotkey provisioning,
// NanoHotkeys.cpp), across the home and overlay processes. Not held when the lock file cannot be
// opened (host tests).
inline constexpr const char* kRaConfigLock = "/data/system/gammaos-hotkeys.lock";
class RaConfigLock {
  public:
    RaConfigLock();
    ~RaConfigLock();
    RaConfigLock(const RaConfigLock&) = delete;
    RaConfigLock& operator=(const RaConfigLock&) = delete;

  private:
    int mFd = -1;
};

// Set by a move that put RetroArch's folder on the card, cleared once a RetroArch session after it
// left both configs pointing there (NanoMenu::retroarchHealStart).
inline constexpr const char* kRaHealPendingProp = "persist.gammaos.retroarch.heal_pending";
inline constexpr const char* kRaHealMtimeProp = "persist.gammaos.retroarch.heal_mtime";   // the configs' newest mtime then

inline constexpr const char* kRaDirProp = "persist.gammaos.retroarch.dir";   // "" = RetroArch's defaults
// The folder whose retroarch.cfg Nano last found there (as root), "" when none: system_server
// cannot look on the card itself.
inline constexpr const char* kRaCfgDirProp = "sys.gammaos.retroarch.cfg_dir";
// The volume's mount point as apps see it ("/storage/<vol>", "/storage/emulated"), "" when dir is
// not under /storage (nothing to wait for).
std::string raVolumeRoot(const std::string& dir);
// Whether a /proc/self/mounts listing has a mount at exactly `root`.
bool raMountListed(const std::string& mounts, const std::string& root);
// The retroarch.cfg every launch passes for folder setting `dir`, Nano's and RootWindowContainer's
// from the home screen alike (NanoRetroArchConfig.java mirrors these three): dir/retroarch.cfg
// while the folder's volume is mounted and the file is there, else the default config.
std::string raLaunchConfig(const std::string& dir, bool volumeMounted, bool cfgThere);
// raLaunchConfig checked now, as root and never through FUSE (storage may be mounting): this
// process's mount table and the file at its raw path. *verifiedDir (when given) gets dir when the
// config there is the one, else "".
std::string raLaunchConfigNow(const std::string& dir, std::string* verifiedDir);
// A /storage path as root reaches it without FUSE: /storage/emulated/0 is /data/media/0, a drive's
// /storage/<vol> is /mnt/media_rw/<vol>. Other paths are returned as they are.
std::string raRawPath(const std::string& p);

}  // namespace nanora
}  // namespace android
