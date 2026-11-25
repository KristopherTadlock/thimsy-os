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

#include "NanoRaConfig.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <initializer_list>
#include <set>

namespace android {
namespace nanora {

namespace {
// The folders RetroArch keeps in its app folder (never copied into it: files written there by
// nano would not belong to RetroArch).
const char* const kRaAppParts[] = {"shaders", "overlays", "filters", "autoconfig"};
}  // namespace

std::string raPart(const std::string& loc, const char* part) {
    if (loc.empty()) return *part ? std::string(kRaPkgData) + "/" + part : std::string(kRaDefSd);
    return *part ? loc + "/" + part : loc;
}

std::string raCfgDir(const std::string& loc) { return loc.empty() ? std::string(kRaDefCfgDir) : loc; }

PathMap raDirKeys(const std::string& loc) {
    const std::string sd = raPart(loc, "");
    return {
        {"system_directory", sd + "/system"},          {"savefile_directory", sd + "/saves"},
        {"savestate_directory", sd + "/states"},       {"screenshot_directory", sd + "/screenshots"},
        {"recording_output_directory", sd + "/recordings"}, {"recording_config_directory", sd + "/records_config"},
        {"rgui_config_directory", sd + "/config"},     {"input_remapping_directory", sd + "/config/remaps"},
        {"cheat_database_path", sd + "/cheats"},       {"playlist_directory", sd + "/playlists"},
        {"thumbnails_directory", sd + "/thumbnails"},  {"core_assets_directory", sd + "/downloads"},
        {"log_dir", sd + "/logs"},                     {"cache_directory", sd + "/temp"},
        {"content_favorites_path", sd + "/playlists/builtin/content_favorites.lpl"},
        {"content_history_path", sd + "/playlists/builtin/content_history.lpl"},
        {"content_image_history_path", sd + "/playlists/builtin/content_image_history.lpl"},
        {"content_music_history_path", sd + "/playlists/builtin/content_music_history.lpl"},
        {"content_video_history_path", sd + "/playlists/builtin/content_video_history.lpl"},
        {"video_shader_dir", raPart("", "shaders")},  {"overlay_directory", raPart("", "overlays")},
        {"osk_overlay_directory", raPart("", "overlays") + "/keyboards"},
        {"audio_filter_dir", raPart("", "filters") + "/audio"},
        {"video_filter_dir", raPart("", "filters") + "/video"},
        {"joypad_autoconfig_dir", raPart("", "autoconfig")},
    };
}

PathMap raMoveKeys(const std::string& from, const std::string& to) {
    PathMap keys = raDirKeys(to);
    if (from.empty())
        for (auto& k : keys) if (k.first == "thumbnails_directory") k.second = kRaDefThumbs;
    return keys;
}

// For every path in a config or shader preset (an overlay or shader picked from the old folder).
PathMap raPrefixMap(const std::string& from, const std::string& to) {
    PathMap m;
    for (const char* part : kRaAppParts) {
        // The app folder's paths stay. An earlier version's T/shaders moves with T; back to the
        // defaults it lands in the RetroArch folder (RetroArch/shaders), not the app folder.
        if (from.empty()) break;
        m.push_back({raPart(from, part), to.empty() ? std::string(kRaDefSd) + "/" + part : raPart(to, part)});
    }
    if (from.empty()) m.push_back({kRaDefThumbs, kRaDefThumbs});   // stays on internal storage (retroarchMovePairs)
    m.push_back({raPart(from, ""), raPart(to, "")});
    std::sort(m.begin(), m.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    return m;
}

namespace {

// Bigger than any real config, preset or playlist by far; a file over it is not RetroArch's to rewrite.
constexpr off_t kRaMaxCfgBytes = 16 << 20;
// Deeper than any core/game/preset layout.
constexpr int kRaMaxTreeDepth = 16;

// The folder holding `path`, opened without following a link in its place; *name gets the file name.
int openParent(const std::string& path, std::string* name) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 == path.size()) {
        errno = EINVAL;
        return -1;
    }
    *name = path.substr(slash + 1);
    const std::string dir = slash == 0 ? std::string("/") : path.substr(0, slash);
    return open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

bool writeAll(int fd, const std::string& text) {
    for (size_t off = 0; off < text.size();) {
        const ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

// The file `name` in dfd, whole: 0 or the errno. *st gets what was read.
int readAt(int dfd, const std::string& name, std::string* text, struct stat* st) {
    const int in = openat(dfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (in < 0) return errno;
    int err = 0;
    if (fstat(in, st) != 0) err = errno;
    else if (!S_ISREG(st->st_mode)) err = EINVAL;
    else if (st->st_size > kRaMaxCfgBytes) err = EFBIG;
    if (err) {
        close(in);
        return err;
    }
    text->clear();
    text->reserve((size_t)st->st_size);
    char buf[16384];
    for (;;) {
        const ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) err = errno;
        if (n <= 0) break;
        text->append(buf, (size_t)n);
        if ((off_t)text->size() > kRaMaxCfgBytes) { err = EFBIG; break; }   // grew while read
    }
    close(in);
    return err;
}

// Root runs this in folders RetroArch (and the cores it downloads) can write: nothing in dfd is
// followed through a link, a FIFO cannot stall it, and the old file stays until the new one is
// complete on disk. A missing file is made (`create`) with the folder's owner.
int editAt(int dfd, const std::string& name, const std::function<bool(std::string*)>& edit, bool create) {
    std::string text;
    struct stat old;
    int err = readAt(dfd, name, &text, &old);
    const bool fresh = err == ENOENT && create;
    if (fresh) {
        struct stat dir;
        if (fstat(dfd, &dir) != 0) return errno;
        old = dir;
        old.st_mode = S_IFREG | 0660;
        text.clear();
    } else if (err) {
        return err;
    }
    if (!edit(&text) && !fresh) return 0;

    const std::string tmp = name + ".nanotmp";
    unlinkat(dfd, tmp.c_str(), 0);
    const int fd = openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    bool ok = writeAll(fd, text) && fsync(fd) == 0;
    // Root's new file (umask 077) would lock RetroArch out of the config in its own folder: it would
    // start with its defaults and fail to save. Only what differs is set: on a card every file has
    // the same owner and mode, which cannot be changed.
    struct stat st;
    ok = ok && fstat(fd, &st) == 0;
    if (ok && (st.st_uid != old.st_uid || st.st_gid != old.st_gid) && fchown(fd, old.st_uid, old.st_gid) != 0) ok = false;
    if (ok && (st.st_mode & 0777) != (old.st_mode & 0777) && fchmod(fd, old.st_mode & 0777) != 0) ok = false;
    err = ok ? 0 : (errno ? errno : EIO);
    if (close(fd) != 0 && !err) err = errno ? errno : EIO;
    struct stat now;
    if (!err && fresh && fstatat(dfd, name.c_str(), &now, AT_SYMLINK_NOFOLLOW) == 0) err = EEXIST;   // made meanwhile
    if (!err && renameat(dfd, tmp.c_str(), dfd, name.c_str()) != 0) err = errno;
    if (err) unlinkat(dfd, tmp.c_str(), 0);
    return err;
}

// Android's other spellings of a storage folder, as the /storage path apps use.
std::string storageSpelling(const std::string& p) {
    static const char* const kPrimary[] = {"/sdcard", "/storage/self/primary", "/data/media/0", "/mnt/sdcard"};
    for (const char* a : kPrimary) {
        const size_t n = strlen(a);
        if (p.compare(0, n, a) == 0 && (p.size() == n || p[n] == '/')) return "/storage/emulated/0" + p.substr(n);
    }
    static const std::string kRaw = "/mnt/media_rw/";
    if (p.compare(0, kRaw.size(), kRaw) == 0 && p.size() > kRaw.size()) return "/storage/" + p.substr(kRaw.size());
    return p;
}

// v is prefix or inside it; storage paths in any letter case.
bool underPrefix(const std::string& v, const std::string& prefix) {
    if (prefix.empty() || v.size() < prefix.size()) return false;
    const bool storage = prefix.compare(0, 9, "/storage/") == 0;
    if (storage ? strncasecmp(v.c_str(), prefix.c_str(), prefix.size()) != 0 : v.compare(0, prefix.size(), prefix) != 0)
        return false;
    return v.size() == prefix.size() || v[prefix.size()] == '/';
}

// The map entry v falls under, -1 when none; *out gets v's new value. An entry that keeps its folder
// (box art) leaves v exactly as it was.
int mapValue(const std::string& v, const PathMap& map, std::string* out) {
    if (v.empty() || v == "default") return -1;
    const std::string c = storageSpelling(v);
    for (size_t i = 0; i < map.size(); i++) {
        const std::string from = storageSpelling(map[i].first);
        if (!underPrefix(c, from)) continue;
        *out = map[i].first == map[i].second ? v : map[i].second + c.substr(from.size());
        return (int)i;
    }
    return -1;
}

bool movesFolder(const PathMap& map, int i) { return i >= 0 && map[(size_t)i].first != map[(size_t)i].second; }

// A `key = "value"` line: its key, and where the quoted value starts and ends (q1, q2). false for
// any other line.
bool cfgLine(const std::string& l, std::string* key, size_t* q1, size_t* q2) {
    *q1 = l.find('"');
    *q2 = l.rfind('"');
    if (*q1 == std::string::npos || *q2 <= *q1) return false;
    const size_t eq = l.find('=');
    *key = l.substr(0, eq == std::string::npos || eq > *q1 ? 0 : eq);
    while (!key->empty() && (key->back() == ' ' || key->back() == '\t')) key->pop_back();
    return true;
}

// A playlist's `"key": "value",` line whose value is a content path: where the value starts and
// ends. RetroArch's JSON writer leaves '/' unescaped, so a path is compared as it is.
bool lplPathLine(const std::string& l, size_t* q1, size_t* q2) {
    const size_t k1 = l.find('"');
    if (k1 == std::string::npos || l.find_first_not_of(" \t") != k1) return false;
    const size_t k2 = l.find('"', k1 + 1);
    if (k2 == std::string::npos) return false;
    const std::string key = l.substr(k1 + 1, k2 - k1 - 1);
    if (key != "path" && key != "scan_content_dir" && key != "scan_dat_file_path") return false;
    const size_t colon = l.find_first_not_of(" \t", k2 + 1);
    if (colon == std::string::npos || l[colon] != ':') return false;
    *q1 = l.find_first_not_of(" \t", colon + 1);
    *q2 = l.rfind('"');
    return *q1 != std::string::npos && l[*q1] == '"' && *q2 > *q1;
}

// One config's text rewritten (see raRewriteFileEx); playlist: only its content paths. true when changed.
bool rewriteText(std::string* text, const PathMap& map, const PathMap* forceKeys, bool playlist) {
    std::set<std::string> seen;
    bool changed = false;
    std::string out;
    out.reserve(text->size() + 4096);
    for (size_t pos = 0; pos < text->size();) {
        size_t eol = text->find('\n', pos);
        const bool lastLine = eol == std::string::npos;
        if (lastLine) eol = text->size();
        std::string l = text->substr(pos, eol - pos);
        pos = eol + 1;
        bool cr = false;
        while (!l.empty() && l.back() == '\r') { l.pop_back(); cr = true; }
        std::string key;
        size_t q1 = 0, q2 = 0;
        if (playlist ? lplPathLine(l, &q1, &q2) : cfgLine(l, &key, &q1, &q2)) {
            std::string v = l.substr(q1 + 1, q2 - q1 - 1), nv;
            if (mapValue(v, map, &nv) >= 0) v = nv;
            if (forceKeys && !key.empty()) {   // an empty folder setting gets the new folder; the user's own paths stay
                for (const auto& fk : *forceKeys)
                    if (key == fk.first) { seen.insert(key); if (v.empty() || v == "default") v = fk.second; }   // "default": RetroArch's folder, the one moved
            }
            const std::string nl = l.substr(0, q1 + 1) + v + l.substr(q2);
            if (nl != l) { l = nl; changed = true; }
        }
        out += l;
        if (cr) out += '\r';
        if (!lastLine || playlist) out += '\n';
    }
    if (!playlist && !out.empty() && out.back() != '\n') out += '\n';
    if (forceKeys)
        for (const auto& fk : *forceKeys)
            if (!seen.count(fk.first)) { out += fk.first + " = \"" + fk.second + "\"\n"; changed = true; }
    if (changed) text->swap(out);
    return changed;
}

bool extIs(const char* name, std::initializer_list<const char*> exts) {
    const char* ext = strrchr(name, '.');
    if (!ext) return false;
    for (const char* e : exts) if (!strcasecmp(ext, e)) return true;
    return false;
}

// dfd is closed.
void rewriteTreeAt(int dfd, const PathMap& map, int depth) {
    DIR* d = fdopendir(dfd);
    if (!d) {
        close(dfd);
        return;
    }
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth >= kRaMaxTreeDepth) continue;
            const int sub = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub >= 0) rewriteTreeAt(sub, map, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;   // never written through a link
        const bool playlist = extIs(e->d_name, {".lpl"});
        if (playlist || extIs(e->d_name, {".cfg", ".opt", ".slangp", ".glslp", ".cgp"}))
            editAt(dirfd(d), e->d_name, [&](std::string* t) { return rewriteText(t, map, nullptr, playlist); }, false);
    }
    closedir(d);
}

}  // namespace

RaConfigLock::RaConfigLock() {
    mFd = open(kRaConfigLock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (mFd >= 0 && flock(mFd, LOCK_EX) != 0) {
        close(mFd);
        mFd = -1;
    }
}

RaConfigLock::~RaConfigLock() {
    if (mFd >= 0) close(mFd);   // releases the lock
}

int raEditFile(const std::string& path, const std::function<bool(std::string*)>& edit) {
    std::string name;
    const int dfd = openParent(path, &name);
    if (dfd < 0) return errno;
    const int err = editAt(dfd, name, edit, false);
    close(dfd);
    return err;
}

int raRewriteFileEx(const std::string& path, const PathMap& map, const PathMap* forceKeys, bool create) {
    RaConfigLock lock;
    std::string name;
    const int dfd = openParent(path, &name);
    if (dfd < 0) return errno;
    const int err = editAt(dfd, name, [&](std::string* t) { return rewriteText(t, map, forceKeys, false); }, create);
    close(dfd);
    return err;
}

bool raRewriteFile(const std::string& path, const PathMap& map, const PathMap* forceKeys) {
    return raRewriteFileEx(path, map, forceKeys) == 0;
}

void raRewriteTree(const std::string& dir, const PathMap& map) {
    RaConfigLock lock;
    const int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd >= 0) rewriteTreeAt(dfd, map, 0);
}

std::vector<std::string> raStaleKeys(const std::string& text, const PathMap& map, const PathMap* forceKeys) {
    std::vector<std::string> stale;
    std::set<std::string> seen;
    for (size_t pos = 0; pos < text.size();) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string l = text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!l.empty() && l.back() == '\r') l.pop_back();
        std::string key, nv;
        size_t q1 = 0, q2 = 0;
        if (!cfgLine(l, &key, &q1, &q2) || key.empty()) continue;
        const std::string v = l.substr(q1 + 1, q2 - q1 - 1);
        bool bad = movesFolder(map, mapValue(v, map, &nv));
        if (forceKeys)
            for (const auto& fk : *forceKeys)
                if (key == fk.first) { seen.insert(key); bad = bad || v.empty() || v == "default"; }
        if (bad && std::find(stale.begin(), stale.end(), key) == stale.end()) stale.push_back(key);
    }
    if (forceKeys)
        for (const auto& fk : *forceKeys)
            if (!seen.count(fk.first)) stale.push_back(fk.first);
    return stale;
}

int raStaleKeysIn(const std::string& path, const PathMap& map, const PathMap* forceKeys, std::vector<std::string>* keys) {
    keys->clear();
    std::string name, text;
    const int dfd = openParent(path, &name);
    if (dfd < 0) return errno;
    struct stat st;
    const int err = readAt(dfd, name, &text, &st);
    close(dfd);
    if (!err) *keys = raStaleKeys(text, map, forceKeys);
    return err;
}

int raHealConfig(const std::string& path, const std::string& dir, std::vector<std::string>* fixed) {
    const PathMap map = raPrefixMap("", dir);
    const PathMap keys = raMoveKeys("", dir);
    const int err = raStaleKeysIn(path, map, &keys, fixed);
    if (err || fixed->empty()) return err;
    return raRewriteFileEx(path, map, &keys);
}

bool raRepairOwner(const std::string& path) {
    std::string name;
    const int dfd = openParent(path, &name);
    if (dfd < 0) return false;
    struct stat dir, st;
    if (fstat(dfd, &dir) != 0 || dir.st_uid == 0) {
        close(dfd);
        return false;
    }
    const int fd = openat(dfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(dfd);
    if (fd < 0) return false;
    bool changed = false;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1
        && fchown(fd, dir.st_uid, dir.st_gid) == 0) {
        changed = true;
        if ((st.st_mode & 0600) != 0600) (void)fchmod(fd, (st.st_mode & 0777) | 0600);
    }
    close(fd);
    return changed;
}

std::string raVolumeRoot(const std::string& dir) {
    static const std::string kStorage = "/storage/";
    if (dir.compare(0, kStorage.size(), kStorage) != 0) return std::string();
    const size_t end = dir.find('/', kStorage.size());
    const std::string vol = dir.substr(kStorage.size(), end == std::string::npos ? std::string::npos : end - kStorage.size());
    if (vol.empty()) return std::string();
    // /storage/emulated/0 and its /storage/self/primary link are one FUSE mount.
    return kStorage + (vol == "self" ? "emulated" : vol);
}

bool raMountListed(const std::string& mounts, const std::string& root) {
    if (root.empty()) return false;
    size_t pos = 0;
    while (pos < mounts.size()) {
        size_t eol = mounts.find('\n', pos);
        if (eol == std::string::npos) eol = mounts.size();
        // "<source> <mount point> <type> ...": roots never hold the spaces /proc escapes.
        const size_t a = mounts.find(' ', pos);
        if (a != std::string::npos && a < eol) {
            size_t b = mounts.find(' ', a + 1);
            if (b == std::string::npos || b > eol) b = eol;
            if (b - a - 1 == root.size() && mounts.compare(a + 1, root.size(), root) == 0) return true;
        }
        pos = eol + 1;
    }
    return false;
}

std::string raLaunchConfig(const std::string& dir, bool volumeMounted, bool cfgThere) {
    if (!dir.empty() && dir[0] == '/' && volumeMounted && cfgThere) return dir + "/retroarch.cfg";
    return std::string(kRaDefCfgDir) + "/retroarch.cfg";
}

std::string raRawPath(const std::string& p) {
    const std::string emu = "/storage/emulated/0";
    if (p == emu || p.compare(0, emu.size() + 1, emu + "/") == 0) return "/data/media/0" + p.substr(emu.size());
    if (p.compare(0, 9, "/storage/") == 0 && p.compare(0, 14, "/storage/self/") != 0) return "/mnt/media_rw/" + p.substr(9);
    return p;
}

std::string raLaunchConfigNow(const std::string& dir, std::string* verifiedDir) {
    const std::string root = raVolumeRoot(dir);
    bool mounted = root.empty();
    if (!mounted) {
        std::string mounts;
        const int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) mounts.append(buf, (size_t)n);
            close(fd);
        }
        mounted = raMountListed(mounts, root);
    }
    const bool there = mounted && !dir.empty() && dir[0] == '/'
                       && access((raRawPath(dir) + "/retroarch.cfg").c_str(), R_OK) == 0;
    if (verifiedDir) *verifiedDir = there ? dir : std::string();
    return raLaunchConfig(dir, mounted, there);
}

}  // namespace nanora
}  // namespace android
