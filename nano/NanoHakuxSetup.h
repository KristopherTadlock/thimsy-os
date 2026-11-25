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

// hakuX's games folders kept to the Xbox folders of Game Settings. hakuX has no storage permission:
// it reads only folders given to it as if picked in Android's folder picker (NanoNetBridge
// uri_grant), named in its settings, and shows its first-start screen while it has none. Thimsy's
// folders are each ROMs/<folder> of a ROM root the Xbox system scans, on internal storage or a card
// (only those can be given); folders the user added in hakuX stay, after them. No Android UI code,
// so host tests can use it (NanoMenuHakux.cpp runs it on the device).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NanoBiosUtil.h"

namespace android {
namespace nanohakux {

inline constexpr const char* kPkg = "com.rfandango.haku_x";
inline constexpr const char* kAppDir = "/data/data/com.rfandango.haku_x";
inline constexpr const char* kPrefs = "/data/data/com.rfandango.haku_x/shared_prefs/x1box_prefs.xml";
// The folders Thimsy last wrote, one URI a line: the ones it may take out again.
inline constexpr const char* kThimsyKey = "thimsy_folders";

// A folder hakuX can be given: ROMs/<folder> on "primary" or on a card (its volume uuid).
struct Folder {
    std::string volume;
    std::string rel;   // "ROMs/<[A-Za-z0-9_-]+>"
    bool operator==(const Folder& o) const { return volume == o.volume && rel == o.rel; }
};

// A ROM folder path as the scanner lists it (/data/media/0, /sdcard, /storage/emulated/0,
// /storage/<uuid> or /mnt/media_rw/<uuid>, then ROMs in any case and one folder). False for any other,
// and for one whose grant is too long for the property it goes to system_server in (sdUriCommand), so
// a long folder name leaves only that folder out.
bool folderOf(const std::string& path, Folder* out);
// As Uri.toString() writes DocumentsContract.buildTreeDocumentUri's: the string hakuX compares with
// its persisted grants. uriFolder reads one back (false: not one of Thimsy's kind).
std::string folderUri(const Folder& f);
bool uriFolder(const std::string& uri, Folder* out);

struct Settings {
    // The scan candidates (NanoMenu::buildScanCandidates) of every enabled system hakuX plays, in order.
    std::vector<std::string> candidates;
    // One of those systems scans the default folders: a card's ROMs/<alias> is listed with internal
    // storage's whenever the card is in.
    bool defaultScan = false;
    // Folders made when missing: internal storage's ROMs/<system folder>, where setup makes the
    // system folders. Only while they are candidates too.
    std::vector<std::string> makeable;
};

// Thimsy's folders: every candidate folder on a mounted volume that is there (or makeable), and every
// one written before that the settings still name. One on a card that is out stays while the card,
// put back, would be scanned for it. Internal storage first, then the cards by volume.
std::vector<Folder> resolveFolders(const Settings& s, const std::vector<Folder>& previous,
                                   const std::function<bool(const std::string& volume)>& mounted,
                                   const std::function<bool(const std::string& path)>& exists);

// The settings text, as SharedPreferences reads and writes it (control characters such as the
// lists' line breaks as character references).
std::string prefsText(const std::string& prefs, const std::string& name);   // a <string>; "" = not there
std::vector<std::string> prefsLines(const std::string& prefs, const std::string& name);   // its lines, not blank
bool prefsSetText(std::string* prefs, const std::string& name, const std::string& value);

struct Plan {
    bool write = false;
    std::string prefs;                // the new text, when write
    std::vector<std::string> grant;   // Thimsy's folders to give first
    std::vector<std::string> revoke;  // Thimsy's folders the settings no longer name
};
// gamesFolders = Thimsy's folders, then the user's (every other one hakuX lists); gamesFolderUri the
// first; kThimsyKey Thimsy's, less any the user had picked in hakuX before Thimsy named it (those are
// never revoked); setup_complete true while Thimsy has one. No write when all is so already.
Plan planSync(const std::string& prefs, const std::vector<std::string>& thimsy);

// pkg has a process ("<pkg>" or "<pkg>:<name>"), read from procRoot's <pid>/cmdline.
bool packageRunning(const std::string& pkg, const std::string& procRoot = "/proc");

// On the device: sdReadPrefs, sdWriteAppFile, sdUriGrant and sdUriRevoke (NanoSdMove.h), as Move
// Emulator Files to microSD gives Azahar its folder.
struct Env {
    std::string root;                                          // as in NanoBiosUtil.h; "" on the device
    std::function<bool(const std::string& path, std::string* out)> read;
    std::function<bool(const std::string& path, const std::string& text)> write;
    std::function<bool(const Folder&)> grant;                  // to hakuX, answered "ok"
    std::function<bool(const Folder&)> revoke;
    std::function<bool(const std::string&)> running;           // packageRunning
    std::function<bool(const std::string& volume)> mounted;
    std::function<bool(const std::string& path)> exists;       // a folder
};

enum class Outcome {
    kDone,          // hakuX's settings name these folders (now, or already)
    kNotInstalled,
    kWait,          // hakuX is running, or its settings are being written: later
    kFailed,        // a folder, a grant or the write failed: later
};
Outcome sync(const Env& env, const Settings& settings);

}  // namespace nanohakux
}  // namespace android
