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

#define LOG_TAG "NanoMenu"

#include "NanoBiosUtil.h"
#include "NanoAppFiles.h"
#include "NanoBiosCatalog.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>   // AID_EXT_DATA_RW
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <vector>

namespace android {
namespace nanobios {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string baseName(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
std::string dirName(const std::string& p) {
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos || sl == 0 ? std::string("/") : p.substr(0, sl);
}
bool pathExists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

std::string appView(const std::string& p) {
    if (p == "/data/media/0" || p.compare(0, 14, "/data/media/0/") == 0) return "/storage/emulated/0" + p.substr(13);
    if (p.compare(0, 14, "/mnt/media_rw/") == 0) return "/storage/" + p.substr(14);
    return p;
}

std::string rawView(const std::string& p) {
    auto starts = [&](const char* pre) {   // pre itself, or under it
        const size_t n = strlen(pre);
        return p.compare(0, n, pre) == 0 && (p.size() == n || p[n] == '/');
    };
    for (const char* pre : {"/storage/emulated/0", "/storage/self/primary", "/sdcard"})
        if (starts(pre)) return "/data/media/0" + p.substr(strlen(pre));
    if (starts("/storage/emulated")) return "/data/media" + p.substr(17);   // another user
    if (starts("/data/user/0")) return "/data/data" + p.substr(12);
    if (p.compare(0, 9, "/storage/") == 0 && !starts("/storage/self") && p.size() > 9) return "/mnt/media_rw/" + p.substr(9);
    return p;
}

std::string canonical(const std::string& p) {
    std::string head = p, tail;
    while (head.size() > 1 && !pathExists(head)) {
        tail = "/" + baseName(head) + tail;
        head = dirName(head);
    }
    char buf[PATH_MAX];
    if (realpath(head.c_str(), buf)) head = buf;
    return (head == "/" ? std::string() : head) + tail;
}

bool under(const std::string& a, const std::string& b) {
    return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/';
}

// No path here reaches a shell (programs run through runArgv), so a quote, a space or a & is fine.
bool safePath(const std::string& p) {
    if (p.empty() || p[0] != '/' || p.find_first_of(std::string("\t\n\r\0", 4)) != std::string::npos) return false;
    for (size_t a = 1; a <= p.size();) {
        size_t b = p.find('/', a);
        if (b == std::string::npos) b = p.size();
        const std::string part = p.substr(a, b - a);
        if (part == "." || part == "..") return false;
        a = b + 1;
    }
    return true;
}

bool inSystemFs(const std::string& c) {
    for (const char* r : {"/proc", "/sys", "/dev"}) if (c == r || under(c, r)) return true;
    return false;
}

std::string pkgAfter(const std::string& c, const std::string& prefix) {
    const size_t n = prefix.size() + 1, sl = c.find('/', n);
    return c.substr(n, sl == std::string::npos ? std::string::npos : sl - n);
}

uint64_t roundUp(uint64_t n, uint64_t b) { return b ? (n + b - 1) / b * b : n; }

bool admit(uint64_t* admitted, uint64_t n, uint64_t spare, uint64_t avail) {
    // Each term is bounded by what a drive can report; compared without an overflowing sum.
    if (n > avail || spare > avail - n || *admitted > avail - n - spare) return false;
    *admitted += n;
    return true;
}

Owner ownerFor(const std::string& c, const std::string& pkg, const std::string& root) {
    Owner o;
    o.pkg = pkg;
    const std::string media = root + "/data/media/0", ext = media + "/Android/data", obb = media + "/Android/obb";
    const std::string data = root + "/data/data", sys = root + "/data/system/drastic-nano";
    if (c == ext || c == obb || under(c, obb)) {
        o.kind = OWN_REFUSED;
    } else if (under(c, ext)) {
        o.kind = !pkg.empty() && pkgAfter(c, ext) == pkg ? OWN_APPEXT : OWN_REFUSED;
    } else if (c == data || under(c, data)) {
        o.kind = !pkg.empty() && under(c, data) && pkgAfter(c, data) == pkg ? OWN_APPDATA : OWN_REFUSED;
    } else if (under(c, media)) {
        o.kind = OWN_SHARED;
    } else if (c == sys || under(c, sys)) {
        o.kind = OWN_SYSTEM;
    } else if (under(c, root + "/mnt/media_rw")) {
        o.kind = OWN_NONE;
    }
    return o;
}

void Packages::load(const std::string& path) {
    std::string text;
    const int fd = openRegular(path, true);
    if (fd < 0) return;
    char buf[8192];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || text.size() > (16u << 20)) break;
        text.append(buf, (size_t)n);
    }
    close(fd);
    readable = true;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        const size_t a = line.find(' ');
        if (a == std::string::npos) continue;
        uid[line.substr(0, a)] = (uid_t)strtoul(line.c_str() + a + 1, nullptr, 10);
    }
}

bool applyOwnerFd(int fd, bool dir, const Owner& o, const Packages& pk, int parentFd, const std::string& root) {
    if (o.kind == OWN_REFUSED) return false;
    if (o.kind == OWN_NONE) return true;
    struct stat pst;
    if (fstat(parentFd, &pst) != 0) return false;
    auto label = [&]() {
        // The folder's fd may be O_PATH, which fgetxattr refuses; its /proc link reaches the same inode.
        char ctx[256];
        const std::string pp = "/proc/self/fd/" + std::to_string(parentFd);
        const ssize_t n = getxattr(pp.c_str(), "security.selinux", ctx, sizeof(ctx));
        if (n > 0 && fsetxattr(fd, "security.selinux", ctx, (size_t)n, 0) != 0) ALOGW("bios: label failed: %s", strerror(errno));
    };
    switch (o.kind) {
        case OWN_APPEXT: {
            auto it = pk.uid.find(o.pkg);
            if (fchown(fd, it != pk.uid.end() ? it->second : pst.st_uid, AID_EXT_DATA_RW) != 0) ALOGW("bios: chown failed: %s", strerror(errno));
            fchmod(fd, dir ? 02770 : 0660);
            label();
            break;
        }
        case OWN_APPDATA: {
            struct stat ast;
            if (stat((root + "/data/data/" + o.pkg).c_str(), &ast) != 0) return false;
            if (fchown(fd, ast.st_uid, ast.st_gid) != 0) ALOGW("bios: chown failed: %s", strerror(errno));
            fchmod(fd, dir ? 0771 : 0660);
            break;
        }
        case OWN_SHARED:
            if (fchown(fd, pst.st_uid, pst.st_gid) != 0) ALOGW("bios: chown failed: %s", strerror(errno));
            fchmod(fd, dir ? (pst.st_mode & 07777) : (pst.st_mode & 0666));
            label();
            break;
        case OWN_SYSTEM:
            if (fchown(fd, pst.st_uid, pst.st_gid) != 0) ALOGW("bios: chown failed: %s", strerror(errno));
            fchmod(fd, dir ? 0755 : 0644);
            break;
    }
    return true;
}

// The place was checked with its links resolved, so a link met now was swapped in since and must not
// redirect root's writes.
int openDir(const std::string& dir, int* out, const Packages* pk, const std::string& pkg, std::string* top,
            const std::string& root) {
    *out = -1;
    if (!safePath(dir)) return EINVAL;
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno ? errno : EIO;
    std::string cur;
    for (size_t a = 1; a < dir.size();) {
        size_t b = dir.find('/', a);
        if (b == std::string::npos) b = dir.size();
        const std::string part = dir.substr(a, b - a);
        a = b + 1;
        if (part.empty()) continue;
        const std::string path = cur + "/" + part;
        int next = openat(fd, part.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int err = next < 0 ? (errno ? errno : EIO) : 0;
        // A card that is not in, or the storage itself: never made here.
        if (err == ENOENT && pk && dirName(path) != root + "/mnt/media_rw" && path != root + "/data/media/0"
            && dirName(path) != root + "/data/data") {
            const Owner o = ownerFor(path, pkg, root);
            if (o.kind == OWN_REFUSED) {
                err = EPERM;
            } else if (mkdirat(fd, part.c_str(), 0700) != 0) {
                err = errno ? errno : EIO;
                if (err == EEXIST) {   // made meanwhile by someone else: used, not owned
                    next = openat(fd, part.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    err = next < 0 ? (errno ? errno : EIO) : 0;
                }
            } else if ((next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
                err = errno ? errno : EIO;
            } else if (!applyOwnerFd(next, true, o, *pk, fd, root)) {
                err = EPERM;
            } else {
                err = 0;
                if (top && top->empty()) *top = path;
            }
        }
        if (err) {
            if (next >= 0) close(next);
            close(fd);
            return err;
        }
        close(fd);
        fd = next;
        cur = path;
    }
    *out = fd;
    return 0;
}

void removeAt(const std::string& path, int flags) {
    int fd;
    if (openDir(dirName(path), &fd) != 0) return;
    unlinkat(fd, baseName(path).c_str(), flags);
    close(fd);
}

int copyNew(const std::string& src, const std::string& dst, const std::string& pkg, const Packages& pk,
            const std::atomic<bool>* cancel, std::string* top, const std::string& root) {
    const Owner o = ownerFor(dst, pkg, root);
    if (o.kind == OWN_REFUSED) return EPERM;
    int dfd;
    int err = openDir(dirName(dst), &dfd, &pk, pkg, top, root);
    if (err) return err;
    const std::string name = baseName(dst), tmp = ".nanobios-" + name + ".tmp";
    unlinkat(dfd, tmp.c_str(), 0);
    const int in = openRegular(src, true);
    if (in < 0) { err = errno ? errno : EIO; close(dfd); return err; }
    const int out = openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) { err = errno ? errno : EIO; close(in); close(dfd); return err; }
    std::vector<char> buf(1 << 16);
    for (;;) {
        if (cancel && cancel->load()) { err = ECANCELED; break; }
        const ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) { err = errno; break; }
        if (n == 0) break;
        for (ssize_t w = 0; w < n;) {
            const ssize_t k = write(out, buf.data() + w, (size_t)(n - w));
            if (k <= 0) { err = k < 0 ? errno : EIO; break; }
            w += k;
        }
        if (err) break;
    }
    if (!err && fsync(out) != 0) err = errno;
    if (!err && !applyOwnerFd(out, false, o, pk, dfd, root)) err = EPERM;
    if (close(out) != 0 && !err) err = errno;
    close(in);
    if (!err) {
        int r = -1;
        errno = ENOSYS;
#ifdef __NR_renameat2
        r = (int)syscall(__NR_renameat2, dfd, tmp.c_str(), dfd, name.c_str(), 1 /* RENAME_NOREPLACE */);
#endif
        if (r != 0 && (errno == EINVAL || errno == ENOSYS)) {   // a filesystem without it: check, then rename
            struct stat st;
            if (fstatat(dfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) err = EEXIST;
            else if (renameat(dfd, tmp.c_str(), dfd, name.c_str()) != 0) err = errno;
        } else if (r != 0) {
            err = errno;
        }
    }
    if (err) unlinkat(dfd, tmp.c_str(), 0);
    close(dfd);
    return err;
}

bool prefsSetBool(std::string* prefs, const std::string& name, bool v) {
    if (prefs->find("<map") == std::string::npos)
        *prefs = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n</map>\n";
    const size_t empty = prefs->find("<map />");
    if (empty != std::string::npos) prefs->replace(empty, 7, "<map>\n</map>");
    const std::string tag = std::string("<boolean name=\"") + name + "\" value=\"" + (v ? "true" : "false") + "\" />";
    const size_t a = prefs->find("<boolean name=\"" + name + "\"");
    if (a != std::string::npos) {
        const size_t b = prefs->find("/>", a);
        if (b == std::string::npos) return false;
        prefs->replace(a, b + 2 - a, tag);
        return true;
    }
    const size_t end = prefs->rfind("</map>");
    if (end == std::string::npos) return false;
    prefs->insert(end, "    " + tag + "\n");
    return true;
}

int prefsGetBool(const std::string& prefs, const std::string& name) {
    const size_t a = prefs.find("<boolean name=\"" + name + "\"");
    if (a == std::string::npos) return -1;
    const size_t b = prefs.find("/>", a);
    return prefs.substr(a, b == std::string::npos ? std::string::npos : b - a).find("value=\"true\"") != std::string::npos;
}

void prefsRemoveBool(std::string* prefs, const std::string& name) {
    const size_t a = prefs->find("<boolean name=\"" + name + "\"");
    if (a == std::string::npos) return;
    const size_t b = prefs->find("/>", a);
    if (b == std::string::npos) return;
    size_t from = a, to = b + 2;
    while (from > 0 && ((*prefs)[from - 1] == ' ' || (*prefs)[from - 1] == '\t')) from--;
    if (to < prefs->size() && (*prefs)[to] == '\n') to++;
    prefs->erase(from, to - from);
}

std::string iniMainGet(const std::string& ini, const char* key) {
    std::string section;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        if (end == std::string::npos) end = ini.size();
        std::string line = ini.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') { section = lowerStr(line.substr(1, line.find(']') - 1)); continue; }
        const size_t eq = line.find('=');
        if (section == "scummvm" && eq != std::string::npos && lowerStr(sdTrimDir(line.substr(0, eq))) == key)
            return sdTrimDir(line.substr(eq + 1));
    }
    return std::string();
}

std::string iniMainSet(const std::string& ini, const char* key, const std::string& value) {
    std::string out, section;
    bool done = false, inMain = false;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t end = ini.find('\n', pos);
        const bool last = end == std::string::npos;
        if (last) end = ini.size();
        std::string line = ini.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line[0] == '[') {
            if (inMain && !done) { out += std::string(key) + "=" + value + "\n"; done = true; }
            inMain = lowerStr(line.substr(1, line.find(']') - 1)) == "scummvm";
        }
        out += line;
        if (!last) out += "\n";
    }
    if (inMain && !done) { out += std::string(out.empty() || out.back() == '\n' ? "" : "\n") + key + "=" + value + "\n"; done = true; }
    if (!done) out = "[scummvm]\n" + std::string(key) + "=" + value + "\n" + out;
    return out;
}

int appOwnerUid(const std::string& path, const std::string& root) {
    for (const std::string& base : {root + "/data/data", root + "/data/media/0/Android/data"}) {
        if (!under(path, base)) continue;
        const std::string pkg = pkgAfter(path, base);
        struct stat st;
        if (pkg.empty() || pkg == "." || pkg == ".." || lstat((base + "/" + pkg).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return -2;   // the app's folder is not there (or is a link): nothing of it is trusted
        return (int)st.st_uid;
    }
    return -1;
}

bool appFileRead(const std::string& path, std::string* out, const std::string& root) {
    if (appOwnerUid(path, root) == -2) return false;   // the app's folder is a link, or not there
    int dfd;
    if (openDir(dirName(path), &dfd) != 0) return false;
    const int fd = openat(dfd, baseName(path).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(dfd);
    if (fd < 0) return false;
    struct stat st;
    // No owner check: in the app's own folder its files may be MediaProvider's (written over MTP),
    // and the walk that follows no link is what keeps root's read inside that folder.
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size < 64 * 1024 * 1024;
    if (ok) {
        out->resize((size_t)st.st_size);
        size_t got = 0;
        while (got < out->size()) {
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

bool appPrefsRead(const std::string& path, std::string* out, const std::string& root) {
    return pathExists(path + ".bak") ? appFileRead(path + ".bak", out, root) : appFileRead(path, out, root);
}

bool appFileWrite(const std::string& path, const std::string& text, const RunFn& run, const std::string& root) {
    if (appOwnerUid(path, root) == -2) return false;   // the app's folder is a link, or not there
    const std::string dir = dirName(path), name = baseName(path);
    int dfd;
    int err = openDir(dir, &dfd);
    if (err == ENOENT) {   // the app never made it (never started): as its app folder
        int pfd;
        if (openDir(dirName(dir), &pfd) != 0) return false;
        struct stat pst;
        const std::string leaf = baseName(dir);
        if (fstat(pfd, &pst) != 0 || mkdirat(pfd, leaf.c_str(), 0771) != 0) {
            close(pfd);
            return false;
        }
        const int nd = openat(pfd, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (nd < 0 || fchown(nd, pst.st_uid, pst.st_gid) != 0) ALOGW("appfile: chown %s failed", dir.c_str());
        if (nd >= 0) close(nd);
        close(pfd);
        if (run) run({"/system/bin/restorecon", dir}, nullptr, 60000);
        err = openDir(dir, &dfd);
    }
    if (err) return false;
    struct stat dst, old;
    if (fstat(dfd, &dst) != 0) {
        close(dfd);
        return false;
    }
    const bool had = fstatat(dfd, name.c_str(), &old, AT_SYMLINK_NOFOLLOW) == 0;
    if (had && !S_ISREG(old.st_mode)) {   // a link (or anything else) in its place is never written through
        ALOGW("appfile: %s is not a file", path.c_str());
        close(dfd);
        return false;
    }
    const std::string tmp = name + ".nanotmp";
    unlinkat(dfd, tmp.c_str(), 0);
    const int fd = openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(dfd); return false; }
    bool ok = true;
    for (size_t off = 0; off < text.size() && ok;) {
        const ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else off += (size_t)n;
    }
    ok = ok && fsync(fd) == 0;
    // A file the app cannot read is as bad as none: it would ask for its folder again.
    if (fchown(fd, dst.st_uid, dst.st_gid) != 0) { ALOGW("appfile: chown %s failed", path.c_str()); ok = false; }
    if (fchmod(fd, had ? (old.st_mode & 0777) : 0600) != 0) ok = false;
    ok = close(fd) == 0 && ok;
    if (!ok || renameat(dfd, tmp.c_str(), dfd, name.c_str()) != 0) {
        unlinkat(dfd, tmp.c_str(), 0);
        close(dfd);
        return false;
    }
    unlinkat(dfd, (name + ".bak").c_str(), 0);   // SharedPreferences would restore its backup over it
    close(dfd);
    if (run && run({"/system/bin/restorecon", path}, nullptr, 60000) != 0) ALOGW("appfile: restorecon %s failed", path.c_str());
    return true;
}

}  // namespace nanobios
}  // namespace android
