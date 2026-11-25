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

// Path, ownership and settings-file helpers of the BIOS installer (NanoBiosWorker.cpp), shared with
// the microSD moves (NanoSdMove.cpp). No Android UI code, so host tests can use them.
//
// root: a folder every fixed path (/data/media/0, /data/data, /mnt/media_rw, ...) is taken under;
// "" on the device.
#pragma once

#include <stdint.h>
#include <sys/types.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace android {
namespace nanobios {

// Runs a program without a shell (argv[0] a full path) and waits up to timeoutMs: its exit status,
// or -1 (runArgv in NanoSdMove.cpp on the device).
using RunFn = std::function<int(const std::vector<std::string>& args, std::string* out, int timeoutMs)>;

std::string lowerStr(std::string s);
std::string baseName(const std::string& p);
std::string dirName(const std::string& p);
bool pathExists(const std::string& p);   // lstat
bool isDir(const std::string& p);        // stat
std::string appView(const std::string& p);   // a raw path as apps see it
// The raw spelling of a path apps write (/storage/emulated/0, /sdcard, /storage/self/primary,
// /storage/<volume>, /data/user/0): /data/media/0, /mnt/media_rw/<volume>, /data/data.
std::string rawView(const std::string& p);
std::string canonical(const std::string& p);   // its deepest existing folder with links resolved, and the rest
bool under(const std::string& a, const std::string& b);   // a is inside b
bool safePath(const std::string& p);   // absolute, no tab, line break or NUL, no "." or ".." part
bool inSystemFs(const std::string& c);   // /proc, /sys, /dev
std::string pkgAfter(const std::string& c, const std::string& prefix);   // c is under prefix
uint64_t roundUp(uint64_t n, uint64_t b);
// Takes n more bytes on a drive with avail free when spare would still be left: true, *admitted grown.
bool admit(uint64_t* admitted, uint64_t n, uint64_t spare, uint64_t avail);

// ---- ownership ----
// Android/data/<pkg> (the slot's own app only): the app, group ext_data_rw. /data/data/<pkg> (the
// slot's own app only): as that folder, then restorecon. Other shared storage: as the parent folder.
// /data/system/drastic-nano (the built-in DS player): as the parent. A card (/mnt/media_rw): nothing.
// Anything in another app's folders, and anywhere else: refused.
enum { OWN_NONE = 0, OWN_APPEXT, OWN_APPDATA, OWN_SHARED, OWN_SYSTEM, OWN_REFUSED };
struct Owner { int kind = OWN_REFUSED; std::string pkg; };
Owner ownerFor(const std::string& c, const std::string& pkg, const std::string& root = std::string());   // c: a real path

struct Packages {
    bool readable = false;
    std::map<std::string, uid_t> uid;
    void load(const std::string& path);   // packages.list
    bool installed(const std::string& pkg) const { return pkg.empty() || !readable || uid.count(pkg); }
};

// Owner, mode and label for a file or folder just made, through its descriptor and its folder's.
// False when it must not be there.
bool applyOwnerFd(int fd, bool dir, const Owner& o, const Packages& pk, int parentFd, const std::string& root = std::string());
// Opens dir, a real path, one part at a time from "/" without following a link (an O_PATH
// descriptor). pk set: missing parts are made, each owned as its place wants (pkg: the slot's app),
// the topmost made going to *top. 0 and *out, or the errno.
int openDir(const std::string& dir, int* out, const Packages* pk = nullptr, const std::string& pkg = std::string(),
            std::string* top = nullptr, const std::string& root = std::string());
// Removes a file (flags AT_REMOVEDIR: an empty folder) by its real path, its folder reached as openDir does.
void removeAt(const std::string& path, int flags);
// Copies src to dst (a real path) through a temp file in dst's folder, fsync, then a rename that never
// replaces a file. 0, or the errno (EEXIST: a file is there; ECANCELED: *cancel was set).
int copyNew(const std::string& src, const std::string& dst, const std::string& pkg, const Packages& pk,
            const std::atomic<bool>* cancel, std::string* top, const std::string& root = std::string());

// ---- settings text ----
bool prefsSetBool(std::string* prefs, const std::string& name, bool v);    // a <boolean> in SharedPreferences text
int prefsGetBool(const std::string& prefs, const std::string& name);       // 1 true, 0 false, -1 not there
void prefsRemoveBool(std::string* prefs, const std::string& name);
std::string iniMainGet(const std::string& ini, const char* key);           // key in [scummvm] ("" = not set)
std::string iniMainSet(const std::string& ini, const char* key, const std::string& value);

// ---- an app's settings files, as root ----
// The owner of the app folder (/data/data/<pkg> or Android/data/<pkg>) a path is in: -1 when it is in
// neither, -2 when that folder is not there or is a link.
int appOwnerUid(const std::string& path, const std::string& root = std::string());
// Reads it: no link followed anywhere on the path, a regular file, the app's folder a real one. Up to 64 MB.
bool appFileRead(const std::string& path, std::string* out, const std::string& root = std::string());
// A SharedPreferences file as the app would load it: its backup when there is one.
bool appPrefsRead(const std::string& path, std::string* out, const std::string& root = std::string());
// Replaces it: a temp file renamed over it in its folder, the folder reached without following a link.
// Owned like its folder, the old file's mode; the .bak SharedPreferences would
// restore removed; restorecon through run (when set).
bool appFileWrite(const std::string& path, const std::string& text, const RunFn& run, const std::string& root = std::string());

}  // namespace nanobios
}  // namespace android
