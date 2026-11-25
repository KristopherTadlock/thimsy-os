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

// Where each emulator keeps its folders, read from its settings text, and which folders a microSD
// move may take files from (NanoSdMove.cpp; the BIOS installer reads the same settings). No Android
// UI code, so host tests can use it.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace android {

// Emulator settings files and default folders.
namespace appdef {
inline constexpr const char* kBackupRoot = "/storage/emulated/0/Nano Backup";
inline constexpr const char* kEsdeInternal = "/storage/emulated/0/ES-DE";
inline constexpr const char* kPpssppMemstickFile = "/data/data/org.ppsspp.ppsspp/files/memstick_dir.txt";
inline constexpr const char* kFlycastPrefs = "/data/data/com.flycast.emulator/shared_prefs/com.flycast.emulator_preferences.xml";
inline constexpr const char* kFlycastDefHome = "/storage/emulated/0/Android/data/com.flycast.emulator/files";
inline constexpr const char* kDrasticFsPrefs = "/data/data/com.dsemu.drastic/shared_prefs/_Dra$t1c_FS$_.xml";
inline constexpr const char* kDrasticDefDir = "/data/user/0/com.dsemu.drastic/files/DraStic";
inline constexpr const char* kMupenPrefs = "/data/data/org.mupen64plusae.v3.fzurita/shared_prefs/org.mupen64plusae.v3.fzurita_preferences.xml";
inline constexpr const char* kMupenDefDir = "/storage/emulated/0/M64Plus";
inline constexpr const char* kArmsx2Prefs = "/data/data/com.armsx2/shared_prefs/ARMSX2.xml";
inline constexpr const char* kArmsx2DefDir = "/storage/emulated/0/Android/data/com.armsx2/files";
inline constexpr const char* kScummvmIni = "/data/data/org.scummvm.scummvm/files/scummvm.ini";
inline constexpr const char* kScummvmDefSaves = "/data/data/org.scummvm.scummvm/files/saves";
inline constexpr const char* kVita3kConfig = "/data/media/0/Android/data/org.vita3k.emulator/files/config.yml";
inline constexpr const char* kVita3kDefPref = "/storage/emulated/0/Android/data/org.vita3k.emulator/files/vita";
inline constexpr const char* kAzaharPrefs = "/data/data/org.azahar_emu.azahar/shared_prefs/org.azahar_emu.azahar_preferences.xml";
inline constexpr const char* kSeedlessPrefs = "/data/data/com.seedlessds.app/shared_prefs/seedless_config.xml";
inline constexpr const char* kSeedlessUserDir = "/data/data/com.seedlessds.app/files/SeedlessDS/user";
inline constexpr const char* kExternalStorageTree = "content://com.android.externalstorage.documents/tree/";
}  // namespace appdef

// A path as its raw folder (/data/media/0, /mnt/media_rw/<volume>, /data/data), links resolved (of
// the part that exists), no trailing slash. Every spelling apps use is mapped first: the mounts
// apps see need not resolve to the raw folder for root.
std::string sdCanon(const std::string& p);
bool sdUnder(const std::string& a, const std::string& b);   // a is b or inside it
// A folder that holds more than one program's files, never moved whole: a storage root, one of its
// standard folders, Android's app folders, or one holding Nano Backup (or, unless it is the one
// moved, the ES-DE folder).
bool sdTooBroad(const std::string& src, bool esdeItself);
// Where a move may take an emulator's files from: inside shared storage (/data/media/0, but another
// app's Android/data or any Android/obb), a card (/mnt/media_rw/<volume>), or pkg's own app folder
// (/data/data/<pkg>). The root the source is confined to (the engine checks it again when it runs),
// or "" when it is none of them. root: as in NanoBiosUtil.h.
std::string sdSourceRoot(const std::string& src, const std::string& pkg, const std::string& root = std::string());

// ---- settings text ----
std::string sdPrefsGet(const std::string& prefs, const std::string& name);   // a <string>, "" = not set
bool sdPrefsSet(std::string* prefs, const std::string& name, const std::string& value);
void sdPrefsRemove(std::string* prefs, const std::string& name);
int sdPrefsGetInt(const std::string& prefs, const std::string& name, int def);   // an <int>
bool sdPrefsSetInt(std::string* prefs, const std::string& name, int value);
std::string sdFileUri(const std::string& path);   // as Uri.fromFile writes it
std::string sdUriDecode(const std::string& s);
std::string sdTrimDir(std::string d);   // no quotes, spaces or trailing slash; /sdcard as /storage/emulated/0
bool sdIniKey(const std::string& line, const char* key, std::string* value);

// Each emulator's folder from its settings text ("" = not a plain folder, unless said otherwise).
std::string flycastHomeFrom(const std::string& prefs);                 // home_directory, else its default
std::string drasticSystemDirFrom(const std::string& prefs);            // _SystemPath, else its default
std::string drasticSystemPathValue(const std::string& dir);            // _SystemPath for a folder
std::string mupenDataDirFrom(const std::string& prefs, bool* internal);   // gameDataStoragePath
std::string armsx2SystemDirFrom(const std::string& prefs, bool* chosen);  // systemDir, else its default
std::string ppssppMemstickFrom(const std::string& text, bool* set);    // memstick_dir.txt
std::string vita3kPrefPath(const std::string& cfg);                    // "" = not set
std::string vita3kSetPrefPath(const std::string& cfg, const std::string& dir);
std::string scummvmSavePath(const std::string& ini);                   // savepath, else its default
std::string scummvmSetSavePath(const std::string& ini, const std::string& from, const std::string& dir);
std::string azaharDir(const std::string& prefs, std::string* uri);     // "" = unset or not a storage folder
std::string azaharTreeUri(const std::string& volume, const std::string& rel);
// SeedlessDS's saves and states, by _SaveLocation (*location): kSeedlessSaveBesideGames (its default) beside
// each game; kSeedlessSaveApp in its app folder (kSeedlessUserDir: backup/ and savestates/);
// kSeedlessSaveFolder side by side in _SaveFolder (*uri), a folder picked through Android. The folder of the
// last two ("" when the picked one is not a storage folder), else "".
enum { kSeedlessSaveBesideGames = 0, kSeedlessSaveApp = 1, kSeedlessSaveFolder = 2 };
std::string seedlessSaveDir(const std::string& prefs, int* location, std::string* uri);
// The folder holds only SeedlessDS's files: game saves (.dsv, .sav), states (.dss) and their
// RetroAchievements progress (.ra), at the top or in a profile's folder (p<number>). Only then is a picked
// save folder moved whole: it may be the games' own folder.
bool seedlessSavesOnly(const std::string& dir);

// Flycast's emu.cfg with every folder of its Dreamcast.*Path settings (a list split by ';') that is
// under an old prefix of `map` moved to the new one. Unchanged when none is.
std::string flycastRewriteCfg(const std::string& cfg, const std::vector<std::pair<std::string, std::string>>& map);
// ARMSX2's settings (PCSX2-Android.ini) after its data folder moved from `from` to `to`, all but the
// folders the move leaves (bios, cache, resources, logs): an absolute [Folders] entry under `from`
// follows the folder (a relative one already does); Bios, which stays, is set to `bios` when it is
// relative or was under `from`.
std::string armsx2RewriteIni(const std::string& ini, const std::string& from, const std::string& to, const std::string& bios);

// The same, read from the app's files under root ("" on the device). Paths as the app writes them.
std::string ppssppMemstickAt(const std::string& root);   // "" = a folder picked through Android
std::string flycastHomeAt(const std::string& root, std::string* prefs);
std::string drasticSystemDirAt(const std::string& root);

}  // namespace android
