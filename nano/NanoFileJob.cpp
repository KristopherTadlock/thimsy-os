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

#define LOG_TAG "GammaOSNano"

#include "NanoFileJob.h"
#include "NanoAppFiles.h"
#include "NanoI18n.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "NanoScraper.h"   // isVolumeRoot (needs <string.h> first)

namespace android {
namespace nanofilejob {

namespace {

std::string dirOf(const std::string& p) {
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos || sl == 0 ? std::string("/") : p.substr(0, sl);
}
std::string nameOf(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }
std::string join(const std::string& dir, const std::string& rel) { return rel.empty() ? dir : dir + "/" + rel; }

bool under(const std::string& a, const std::string& b) {   // a is inside b
    return a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/';
}

// Each missing folder of dir, made (never a volume's own folder: with the card out that would put a
// folder where it mounts); the ones made go to *made.
void makeDirs(const std::string& dir, std::vector<std::string>* made) {
    for (size_t i = 1; i <= dir.size(); i++) {
        if ((i != dir.size() && dir[i] != '/') || nanoscraper::isVolumeRoot(dir.substr(0, i))) continue;
        if (::mkdir(dir.substr(0, i).c_str(), 0775) == 0 && made) made->push_back(dir.substr(0, i));
    }
}

std::string sizeText(uint64_t bytes) {
    char buf[32];
    const double b = (double)bytes;
    if (b >= 1024.0 * 1024.0 * 1024.0) snprintf(buf, sizeof(buf), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024.0 * 1024.0) snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    return buf;
}

// A regular file, opened without following a link anywhere on its path (O_NONBLOCK while opening,
// so a FIFO swapped in never blocks).
int openSource(const std::string& path) {
    const int fd = openNoFollow(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    const int fl = fcntl(fd, F_GETFL);
    if (fl != -1) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    return fd;
}

bool isTempName(const char* name) {   // copyFd's
    const size_t n = strlen(name), k = sizeof(".nanocopy") - 1;
    return n > k && strcmp(name + n - k, ".nanocopy") == 0;
}

// Two destinations that may be one file on a card that ignores case (FAT, exFAT) share this.
std::string sameFileKey(std::string p) {
    for (auto& ch : p) ch = (unsigned char)ch >= 0x80 ? '\x80' : (char)tolower((unsigned char)ch);
    return p;
}

// The same bytes (a move tried before copied it already).
bool sameContent(int in, const std::string& b) {
    struct stat sa, sb;
    if (fstat(in, &sa) != 0 || stat(b.c_str(), &sb) != 0 || sa.st_size != sb.st_size) return false;
    const int fb = open(b.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fb < 0) return false;
    static thread_local char ba[64 * 1024], bb[64 * 1024];
    bool same = true;
    for (off_t off = 0; same;) {
        const ssize_t ra = pread(in, ba, sizeof(ba), off), rb = pread(fb, bb, sizeof(bb), off);
        if (ra != rb || ra < 0) { same = false; break; }
        if (ra == 0) break;
        same = memcmp(ba, bb, (size_t)ra) == 0;
        off += ra;
    }
    close(fb);
    return same;
}

// The empty folders under a folder (and it), removed: entered and removed by descriptor, never
// through a link.
bool pruneAt(int dfd, const char* name) {
    const int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    DIR* d = fdopendir(fd);
    if (!d) { close(fd); return false; }
    bool empty = true;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode) && pruneAt(fd, e->d_name)) continue;
        empty = false;
    }
    closedir(d);
    return empty && unlinkat(dfd, name, AT_REMOVEDIR) == 0;
}

bool removeTreeAt(int dfd, const char* name) {
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        const int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return false;
        DIR* d = fdopendir(fd);
        if (!d) { close(fd); return false; }
        bool ok = true;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
        for (const auto& n : names) if (!removeTreeAt(fd, n.c_str())) ok = false;
        closedir(d);
        return unlinkat(dfd, name, AT_REMOVEDIR) == 0 && ok;
    }
    return unlinkat(dfd, name, 0) == 0;
}

// A move's original folder set aside as <folder>.nanomove (where the job then empties it) and a link
// to its copy in its place, owned like its parent; *path becomes the folder set aside.
bool linkFolder(std::string* path, const std::string& linkTo, const Hooks& h) {
    const std::string src = *path;
    std::string aside = src + ".nanomove";
    struct stat st, pst;
    for (int n = 2; lstat(aside.c_str(), &st) == 0; n++) aside = src + ".nanomove." + std::to_string(n);   // an earlier one's
    const bool had = lstat(src.c_str(), &st) == 0;
    if ((had && !S_ISDIR(st.st_mode)) || stat(dirOf(src).c_str(), &pst) != 0) {
        ALOGW("ps3menu: move: %s is not a folder to link", src.c_str());
        return false;
    }
    makeDirs(linkTo, nullptr);
    if (stat(linkTo.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {   // the card went
        ALOGW("ps3menu: move: %s is not there to link to", linkTo.c_str());
        return false;
    }
    if (had && rename(src.c_str(), aside.c_str()) != 0) {
        ALOGW("ps3menu: move: %s could not be set aside: %s", src.c_str(), strerror(errno));
        return false;
    }
    if (symlink(linkTo.c_str(), src.c_str()) != 0) {
        ALOGW("ps3menu: move: link %s -> %s failed: %s", src.c_str(), linkTo.c_str(), strerror(errno));
        if (had) rename(aside.c_str(), src.c_str());
        return false;
    }
    if (lchown(src.c_str(), pst.st_uid, pst.st_gid) != 0) ALOGW("ps3menu: move: chown %s failed", src.c_str());
    if (h.restorecon) h.restorecon(src);
    if (had) *path = aside;
    return true;
}

bool treeCopyRec(const std::string& src, const std::string& dst, const std::string& rel, const CopyIo* io,
                 std::vector<std::pair<std::string, bool>>* copied) {
    const std::string from = join(src, rel), to = join(dst, rel);
    struct stat st;
    if (lstat(from.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) return false;
        if (copied) copied->emplace_back(rel, true);
        DIR* d = opendir(from.c_str());
        if (!d) return false;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
        closedir(d);
        bool ok = true;
        for (const auto& n : names)
            if (!treeCopyRec(src, dst, rel.empty() ? n : rel + "/" + n, io, copied)) ok = false;
        return ok;
    }
    if (S_ISLNK(st.st_mode)) {
        // Copied as a link (cp -P): followed, a Move would delete the link after copying nothing.
        char buf[4096];
        const ssize_t n = readlink(from.c_str(), buf, sizeof(buf) - 1);
        if (n < 0) return false;
        buf[n] = '\0';
        if (symlink(buf, to.c_str()) != 0) return false;
        if (copied) copied->emplace_back(rel, false);
        return true;
    }
    if (S_ISREG(st.st_mode)) {
        if (!copyFile(from, to, COPY_EXCL_DIRECT, io)) return false;
        if (copied) copied->emplace_back(rel, false);
        return true;
    }
    return true;   // a socket, FIFO or device: left out (and so never deleted by a Move)
}

}  // namespace

int openNoFollow(const std::string& path, int flags) {
    if (path.empty() || path[0] != '/') { errno = EINVAL; return -1; }
    if (path == "/") return open("/", flags | O_CLOEXEC);
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t a = 1;
    for (;;) {
        size_t b = path.find('/', a);
        const bool last = b == std::string::npos || path.find_first_not_of('/', b) == std::string::npos;
        if (b == std::string::npos) b = path.size();
        const std::string part = path.substr(a, b - a);
        if (part == "." || part == "..") { close(fd); errno = EINVAL; return -1; }
        if (!part.empty()) {
            const int next = last ? openat(fd, part.c_str(), flags | O_NOFOLLOW | O_CLOEXEC)
                                  : openat(fd, part.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const int err = errno;
            close(fd);
            if (next < 0) { errno = err; return -1; }
            fd = next;
        }
        if (last) return fd;
        a = b + 1;
    }
}

bool unlinkNoFollow(const std::string& path, int flags) {
    const int pfd = openNoFollow(dirOf(path), O_PATH | O_DIRECTORY);
    if (pfd < 0) return false;
    const bool ok = unlinkat(pfd, nameOf(path).c_str(), flags) == 0;
    const int err = errno;
    close(pfd);
    errno = err;
    return ok;
}

int renameNoReplace(const std::string& from, const std::string& to, bool* unsupported) {
    if (!unsupported || !*unsupported) {
        int r = -1;
        errno = ENOSYS;
#ifdef __NR_renameat2
        r = (int)syscall(__NR_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1 /* RENAME_NOREPLACE */);
#endif
        if (r == 0) return 0;
        if (errno != EINVAL && errno != ENOSYS) return errno;
        if (unsupported) *unsupported = true;
    }
    struct stat st;   // a filesystem without it: check, then rename
    if (lstat(to.c_str(), &st) == 0) return EEXIST;
    return rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
}

int syncDir(const std::string& path, const CopyIo* io) {
    struct stat st;
    const std::string dir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? path : dirOf(path);
    const int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno ? errno : EIO;
    errno = 0;
    const int r = io && io->syncfs ? io->syncfs(fd) : syncfs(fd);
    const int err = r != 0 ? (errno ? errno : EIO) : 0;
    close(fd);
    return err;
}

std::string realParent(const std::string& p) {
    std::string q = p;
    while (q.size() > 1 && q.back() == '/') q.pop_back();
    char buf[PATH_MAX];
    const std::string dir = dirOf(q);
    if (!realpath(dir.c_str(), buf)) return q;
    const std::string r = buf;
    return (r == "/" ? std::string() : r) + "/" + nameOf(q);
}

bool copyFd(int in, const std::string& dst, int flags, const CopyIo* io) {
    const int mode = flags & 0xff;
    struct stat sst;
    if (fstat(in, &sst) != 0) return false;
    const std::string target = mode == COPY_EXCL_DIRECT ? dst : dst + ".nanocopy";
    const int oflags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, perm = mode == COPY_EXCL_DIRECT ? 0644 : 0664;
    int out = open(target.c_str(), oflags, perm);
    if (out < 0 && errno == EEXIST && mode != COPY_EXCL_DIRECT) {   // an earlier copy's, cut short
        unlink(target.c_str());
        out = open(target.c_str(), oflags, perm);
    }
    if (out < 0) return false;
    static thread_local char buf[256 * 1024];
    bool ok = true;
    int err = 0;
    uint64_t total = 0;
    for (off_t off = 0; ok;) {
        const ssize_t rd = pread(in, buf, sizeof(buf), off);
        if (rd < 0 && errno == EINTR) continue;
        if (rd < 0) { ok = false; err = errno; break; }
        if (rd == 0) break;
        for (ssize_t w = 0; w < rd;) {
            const ssize_t wr = io && io->write ? io->write(out, buf + w, (size_t)(rd - w)) : write(out, buf + w, (size_t)(rd - w));
            if (wr < 0 && errno == EINTR) continue;
            if (wr <= 0) { ok = false; err = wr < 0 ? errno : EIO; break; }
            w += wr;
        }
        off += rd;
        total += (uint64_t)rd;
    }
    // A full card or a write error often shows only when the data is flushed. Each file flushed alone
    // would flush a card's cache once per file: the caller flushes the whole filesystem once instead
    // (syncDir) before anything is deleted. A big file is flushed here, so a failure names it.
    const bool flush = (flags & COPY_FSYNC) || total > (256ull << 20);
    if (ok && flush && (io && io->fsync ? io->fsync(out) : fsync(out)) != 0) { ok = false; err = errno ? errno : EIO; }
    struct stat ost;
    if (ok && (fstat(out, &ost) != 0 || (uint64_t)ost.st_size != total || total != (uint64_t)sst.st_size)) {
        ALOGW("ps3menu: copy: %s is %llu bytes of %llu", dst.c_str(), (unsigned long long)total, (unsigned long long)sst.st_size);
        ok = false;
        err = EIO;
    }
    if (ok && mode == COPY_EXCL_DIRECT && fchmod(out, sst.st_mode & 07777) != 0) ALOGW("ps3menu: copy: chmod %s failed", dst.c_str());
    if (close(out) != 0 && ok) { ok = false; err = errno ? errno : EIO; }
    if (ok && mode != COPY_EXCL_DIRECT) {
        const int r = mode == COPY_REPLACE ? (rename(target.c_str(), dst.c_str()) == 0 ? 0 : errno)
                                           : renameNoReplace(target, dst, io ? &io->noReplaceUnsupported : nullptr);
        if (r != 0) { ok = false; err = r; }
    }
    if (!ok) {
        unlink(target.c_str());
        errno = err;
    }
    return ok;
}

bool copyFile(const std::string& src, const std::string& dst, int flags, const CopyIo* io) {
    const int in = open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (in < 0) return false;
    struct stat st;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) { close(in); errno = EINVAL; return false; }
    const bool ok = copyFd(in, dst, flags, io);
    const int err = errno;
    close(in);
    errno = err;
    return ok;
}

bool treeCopy(const std::string& src, const std::string& dst, const CopyIo* io) {
    return treeCopyRec(src, dst, std::string(), io, nullptr);
}

bool treeMove(const std::string& src, const std::string& dst, const CopyIo* io) {
    const std::string real = realParent(src);
    std::vector<std::pair<std::string, bool>> copied;   // relative path, a folder (in the order made)
    if (!treeCopyRec(real, dst, std::string(), io, &copied)) return false;
    // The copies on the drive before any source goes.
    if (const int err = syncDir(dst, io)) {
        ALOGW("fe: move: flushing %s failed: %s", dst.c_str(), strerror(err));
        return false;
    }
    bool ok = true;
    for (auto it = copied.rbegin(); it != copied.rend(); ++it) {
        if (unlinkNoFollow(join(real, it->first), it->second ? AT_REMOVEDIR : 0)) continue;
        // A folder still holding what was left out (a FIFO, say) stays with it.
        if (it->second && (errno == ENOTEMPTY || errno == EEXIST)) continue;
        ALOGW("fe: move: %s could not be removed: %s", join(real, it->first).c_str(), strerror(errno));
        ok = false;
    }
    return ok;
}

bool treeRemove(const std::string& p) {
    const std::string real = realParent(p);
    const int pfd = openNoFollow(dirOf(real), O_PATH | O_DIRECTORY);
    if (pfd < 0) return false;
    const bool ok = removeTreeAt(pfd, nameOf(real).c_str());
    close(pfd);
    return ok;
}

Result run(std::vector<Pair> pairs, const Hooks& h) {
    Result res;
    Counters& c = res.counters;
    auto cancelled = [&]() { return h.cancelled && h.cancelled(); };
    auto report = [&]() { if (h.progress) h.progress(c); };
    auto rawPath = [&](const std::string& p) { return h.rawPath ? h.rawPath(p) : p; };
    const CopyIo* io = &h.io;
    io->noReplaceUnsupported = false;

    // Every regular file (relative path) first, so the modal can show a total. A folder or file that
    // is there but cannot be read (a card pulled mid-copy), a required source that is missing, or a
    // link (a move takes nothing through one) counts as a failure, so the setting is not switched
    // to an incomplete copy; an optional source that does not exist (an unused default folder) is
    // simply skipped. Each source is walked by descriptor from its real folder, links never followed.
    // result: 0 copied, 1 already there (a different file), 2 failed, 3 merged, 4 already there (the same)
    struct Item { size_t pair; std::string rel; int result = -1; int err = 0; bool created = false; };
    std::vector<Item> items;
    std::vector<std::pair<size_t, std::string>> emptyDirs;   // recreated at the destination
    std::vector<std::pair<size_t, std::string>> junk;        // a move's leftover temporary files
    std::vector<std::string> srcReal(pairs.size());
    int unreadable = 0, missingRequired = 0;
    std::function<void(size_t, int, const std::string&)> walk = [&](size_t pi, int fd, const std::string& rel) {
        const int dfd = dup(fd);
        DIR* d = dfd >= 0 ? fdopendir(dfd) : nullptr;
        if (!d) { if (dfd >= 0) close(dfd); unreadable++; return; }
        bool any = false;
        for (;;) {
            errno = 0;
            dirent* e = readdir(d);
            if (!e) { if (errno != 0) unreadable++; break; }
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            const std::string r = rel.empty() ? std::string(e->d_name) : rel + "/" + e->d_name;
            struct stat st;
            const bool there = fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0;
            // An interrupted copy's temporary file: copied, it would be the temporary file of its own
            // name's copy, which removes it. A move deletes it with the originals.
            if (there && S_ISREG(st.st_mode) && isTempName(e->d_name)) {
                if (pairs[pi].move) junk.emplace_back(pi, r);
                continue;
            }
            any = true;
            if (!there) { if (errno != ENOENT) unreadable++; continue; }
            const auto& skip = pairs[pi].skipDirs;
            if (S_ISDIR(st.st_mode)) {
                if (std::find(skip.begin(), skip.end(), r) != skip.end()) continue;
                const int child = openat(fd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (child < 0) { if (errno != ENOENT) unreadable++; continue; }
                walk(pi, child, r);
                close(child);
            } else if (S_ISREG(st.st_mode)) {
                items.push_back({pi, r});
            } else if (S_ISLNK(st.st_mode) && pairs[pi].move) {
                unreadable++;   // would be lost: nothing moves
            }
        }
        closedir(d);
        if (!any) emptyDirs.emplace_back(pi, rel);
    };
    auto collect = [&]() {
        items.clear();
        emptyDirs.clear();
        junk.clear();
        unreadable = 0; missingRequired = 0;
        for (size_t i = 0; i < pairs.size(); i++) {
            const Pair& pr = pairs[i];
            srcReal[i].clear();
            if (pr.src == pr.dst) continue;
            struct stat st;
            if (lstat(pr.src.c_str(), &st) != 0) {
                if (errno != ENOENT) unreadable++;
                else if (pr.required) missingRequired++;
                continue;
            }
            std::string real = realParent(pr.src);
            if (lstat(real.c_str(), &st) != 0) { unreadable++; continue; }
            if (S_ISLNK(st.st_mode)) {
                char buf[PATH_MAX];
                // A move's source that is a link: what it points at is not the program's to give away.
                if (pr.move || !realpath(real.c_str(), buf) || stat(buf, &st) != 0) {
                    ALOGW("ps3menu: copy: %s is a link; not %s", pr.src.c_str(), pr.move ? "moved" : "copied");
                    unreadable++;
                    continue;
                }
                real = buf;
            }
            if (!pr.srcRoot.empty() && !under(sdCanon(real), pr.srcRoot)) {
                ALOGW("ps3menu: move: %s is not inside %s", real.c_str(), pr.srcRoot.c_str());
                unreadable++;
                continue;
            }
            srcReal[i] = real;
            if (S_ISDIR(st.st_mode)) {
                const int fd = openNoFollow(real, O_RDONLY | O_DIRECTORY);
                if (fd < 0) { unreadable++; continue; }
                walk(i, fd, std::string());
                close(fd);
            } else if (S_ISREG(st.st_mode)) {
                items.push_back({i, std::string()});   // a single file
            }
        }
    };
    collect();
    std::vector<std::string> canonDst(pairs.size());
    for (size_t i = 0; i < pairs.size(); i++) canonDst[i] = sdCanon(pairs[i].dst);

    // Room for what will be copied, on each destination drive, before anything is closed or copied:
    // every file the job writes (not the ones it leaves because one is there), rounded up to the
    // drive's cluster, a cluster per new folder, and 8 MB to spare.
    {
        // An overwrite needs only what it adds, but its copy sits beside the old file until the rename.
        struct Need { std::string path; uint64_t want = 0, avail = 0, cluster = 4096, peak = 0; bool known = false; };
        std::map<dev_t, Need> need;
        std::set<std::string> newDirs, counted;
        std::map<std::string, std::pair<dev_t, std::string>> parentOf;   // a folder: its drive and nearest existing folder
        auto drive = [&](dev_t dev, const std::string& path) -> Need& {
            Need& n = need[dev];
            if (n.path.empty()) {
                n.path = path;
                struct statvfs vfs;
                if ((h.statvfs ? h.statvfs(path.c_str(), &vfs) : ::statvfs(path.c_str(), &vfs)) == 0) {
                    n.known = true;
                    if (vfs.f_bsize) n.cluster = (uint64_t)vfs.f_bsize;
                    n.avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
                    n.want = 8ull * 1024 * 1024;
                }
            }
            return n;
        };
        // The drive a new folder lands on, its missing parents counted a cluster each.
        auto place = [&](const std::string& dirPath) -> std::map<std::string, std::pair<dev_t, std::string>>::iterator {
            auto pit = parentOf.find(dirPath);
            if (pit != parentOf.end()) return pit;
            std::string dir = dirPath;
            std::vector<std::string> made;
            struct stat dirSt{};
            while (dir.size() > 1 && stat(dir.c_str(), &dirSt) != 0) {
                made.push_back(dir);
                dir = dirOf(dir);
            }
            pit = parentOf.emplace(dirPath, std::make_pair(dirSt.st_dev, dir)).first;
            if (dir.size() > 1) {
                Need& n = drive(dirSt.st_dev, dir);
                for (const auto& m : made) if (newDirs.insert(m).second) n.want += n.cluster;
            }
            return pit;
        };
        size_t seen = 0;
        for (const auto& it : items) {
            if ((++seen & 255) == 0 && cancelled()) break;
            const Pair& pr = pairs[it.pair];
            if (!pr.merged.empty() && std::find(pr.merged.begin(), pr.merged.end(), it.rel) != pr.merged.end()) continue;
            const std::string src = join(srcReal[it.pair], it.rel);
            const std::string dst = join(canonDst[it.pair], it.rel);
            if (!counted.insert(dst).second) continue;   // another pair of the job writes it already
            struct stat sst, dstSt;
            if (lstat(src.c_str(), &sst) != 0) continue;
            const bool exists = lstat(dst.c_str(), &dstSt) == 0;
            if (exists && !pr.overwrite && (pr.move || !pr.replaceOlder || sst.st_mtime <= dstSt.st_mtime)) continue;
            auto pit = place(dirOf(dst));
            if (pit->second.second.size() <= 1) continue;
            Need& n = drive(pit->second.first, pit->second.second);
            if (!n.known) continue;
            const uint64_t size = ((uint64_t)sst.st_size + n.cluster - 1) / n.cluster * n.cluster;
            if (exists && S_ISREG(dstSt.st_mode)) {
                const uint64_t old = ((uint64_t)dstSt.st_size + n.cluster - 1) / n.cluster * n.cluster;
                if (size > old) n.want += size - old;
                n.peak = std::max(n.peak, size);
            } else {
                n.want += size;
            }
        }
        for (const auto& ed : emptyDirs) place(join(canonDst[ed.first], ed.second));
        for (auto& kv : need) kv.second.want += kv.second.peak;
        std::string problem;
        for (const auto& kv : need) {
            const Need& n = kv.second;
            if (!n.known) continue;
            ALOGI("ps3menu: copy: %s needs %llu bytes, %llu free", n.path.c_str(),
                  (unsigned long long)n.want, (unsigned long long)n.avail);
            if (n.want > n.avail) {
                char buf[256];
                snprintf(buf, sizeof(buf), trDyn("Not enough space: %s needed, %s free. Nothing was copied."),
                         sizeText(n.want).c_str(), sizeText(n.avail).c_str());
                problem = buf;
                break;
            }
        }
        if (!problem.empty() || cancelled()) {   // nothing stopped or copied yet
            c.total = (int)items.size();
            if (!problem.empty()) {
                c.fail++;
                c.error = problem;
            }
            report();
            return res;
        }
    }

    // The programs whose files move are closed first (they write their settings when they close).
    if (h.stopPrograms && h.stopPrograms()) collect();
    c.total = (int)items.size();
    if (missingRequired > 0) {
        c.fail += missingRequired;
        c.error = trDyn("The current folder is missing. Is the SD card in?");
    } else if (unreadable > 0) {
        c.fail += unreadable;
        c.error = trDyn("Some folders could not be read.");
    }
    report();
    std::vector<std::string> madeDirs;   // folders this job made, for a move given back
    bool stopped = false;
    auto isMerged = [&](const Item& it) {
        const Pair& pr = pairs[it.pair];
        return !pr.merged.empty() && std::find(pr.merged.begin(), pr.merged.end(), it.rel) != pr.merged.end();
    };
    // Each folder made once, under a lock, so madeDirs keeps every folder after its parent.
    std::mutex dirMu;
    std::set<std::string> dirsReady;
    auto ensureDirs = [&](const std::string& dir) {
        std::lock_guard<std::mutex> lk(dirMu);
        if (dirsReady.count(dir)) return;
        makeDirs(dir, &madeDirs);
        struct stat st;
        if (stat(dir.c_str(), &st) == 0) dirsReady.insert(dir);
    };
    // One file, on a copy thread: sets its result, err and created.
    auto copyOne = [&](Item& it, const CopyIo* wio) {
        if (isMerged(it)) { it.result = 3; return; }   // its data is in another file of the job
        const Pair& pr = pairs[it.pair];
        const std::string src = join(srcReal[it.pair], it.rel);
        const std::string dst = join(pr.dst, it.rel);
        errno = 0;
        struct stat st, sst;
        int result;
        const int in = openSource(src);
        int err = in < 0 ? errno : 0;
        const bool exists = lstat(dst.c_str(), &st) == 0;
        const bool checkAge = exists && pr.replaceOlder && S_ISREG(st.st_mode);
        if (in < 0 || fstat(in, &sst) != 0) {
            result = 2;
        } else if (exists && pr.move) {
            // The very same file reached another way is never "moved" (that deletes it).
            struct stat b;
            const bool one = stat(rawPath(dst).c_str(), &b) == 0 && sst.st_dev == b.st_dev && sst.st_ino == b.st_ino;
            if (one) result = 2;
            else if (pr.overwrite) { result = copyFd(in, dst, COPY_REPLACE, wio) ? 0 : 2; err = errno; }
            else result = S_ISREG(st.st_mode) && sameContent(in, dst) ? 4 : 1;
        } else if (exists && !pr.overwrite && !(checkAge && sst.st_mtime > st.st_mtime)) {
            result = 1;
        } else {
            ensureDirs(dirOf(dst));
            if (copyFd(in, dst, exists ? COPY_REPLACE : COPY_NEW, wio)) {
                result = 0;
                it.created = !exists;
            } else {
                err = errno;
                result = !exists && err == EEXIST ? 1 : 2;   // one appeared meanwhile: kept
            }
        }
        if (in >= 0) close(in);
        it.result = result;
        it.err = err;
    };
    // Counted on this thread in the job's order, whatever order the copies end in, so the counts and
    // the error are the same at any thread count. true: the destination is full (the copy stops).
    auto count = [&](const Item& it) {
        const bool full = it.result == 2 && it.err == ENOSPC;
        c.done++;
        if (it.result == 0 || it.result == 4) c.hits++;
        else if (it.result == 1) c.skip++;
        else if (it.result == 2) { c.fail++; if (c.error.empty()) c.error = trDyn("Some files could not be copied."); }
        if (full) c.error = trDyn("The destination is full. The copy stopped.");
        report();
        return full;
    };

    // Copies with one file's destination (by two pairs, or two names a card sees as one) run one
    // after the other: never two writers on one .nanocopy.
    const size_t n = items.size();
    std::vector<size_t> after(n, SIZE_MAX);
    {
        std::unordered_map<std::string, size_t> last;
        for (size_t i = 0; i < n; i++) {
            if (isMerged(items[i])) continue;
            auto ins = last.emplace(sameFileKey(join(canonDst[items[i].pair], items[i].rel)), i);
            if (!ins.second) { after[i] = ins.first->second; ins.first->second = i; }
        }
    }
    const int threads = (int)std::max<size_t>(1, std::min<size_t>(n, h.copyThreads > 0 ? h.copyThreads : kCopyThreads));
    // Each thread's io: the same functions, its own noReplaceUnsupported.
    std::vector<CopyIo> ios(threads);
    for (auto& w : ios) {
        if (h.io.write) w.write = [&h](int fd, const void* b, size_t len) { return h.io.write(fd, b, len); };
        if (h.io.fsync) w.fsync = [&h](int fd) { return h.io.fsync(fd); };
        if (h.io.syncfs) w.syncfs = [&h](int fd) { return h.io.syncfs(fd); };
    }
    std::atomic<size_t> next{0};
    std::atomic<bool> halt{false};   // cancelled or a full destination: no new file is started
    std::mutex mu;
    std::condition_variable cv;
    std::vector<char> finished(n, 0);   // under mu
    // Files are taken in order, and every one taken is finished: those done are always the first ones.
    auto worker = [&](CopyIo* wio) {
        while (!halt.load()) {
            const size_t i = next.fetch_add(1);
            if (i >= n) return;
            if (after[i] != SIZE_MAX) {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&]() { return finished[after[i]] != 0; });
            }
            copyOne(items[i], wio);
            if (items[i].result == 2 && items[i].err == ENOSPC) halt = true;
            {
                std::lock_guard<std::mutex> lk(mu);
                finished[i] = 1;
            }
            cv.notify_all();
        }
    };
    size_t counted = 0;
    if (n > 0 && cancelled()) stopped = true;
    if (n > 0 && !stopped) {
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; t++) pool.emplace_back(worker, &ios[t]);
        std::string shown;
        for (;;) {
            size_t ready;
            {
                std::unique_lock<std::mutex> lk(mu);
                // Woken by the next file in order finishing, or now and then to look at cancel.
                cv.wait_for(lk, std::chrono::milliseconds(100), [&]() { return finished[counted] != 0; });
                for (ready = counted; ready < n && finished[ready]; ready++) {}
            }
            for (; counted < ready && !stopped; counted++) stopped = count(items[counted]);
            if (stopped || counted == n) break;
            if (cancelled()) { stopped = true; break; }
            const Item& cur = items[counted];   // the file being copied that the count waits on
            if (!isMerged(cur)) {
                const std::string name = cur.rel.empty() ? nameOf(join(srcReal[cur.pair], cur.rel)) : cur.rel;
                if (name != shown) { shown = name; c.status = name; report(); }
            }
        }
        halt = true;
        for (auto& t : pool) t.join();
        // Cancelled: the files that were being copied finished, and count. Past a full destination
        // nothing does, as when one file was copied at a time (their copies still go back with a move).
        const bool full = counted > 0 && items[counted - 1].result == 2 && items[counted - 1].err == ENOSPC;
        for (; !full && counted < n && finished[counted]; counted++)
            if (count(items[counted])) break;
        for (const auto& w : ios)
            if (w.noReplaceUnsupported) io->noReplaceUnsupported = true;
    }
    // Empty folders too: a program may count on one being there.
    for (const auto& ed : emptyDirs) {
        if (stopped || cancelled()) break;
        const std::string dst = join(pairs[ed.first].dst, ed.second);
        struct stat st;
        makeDirs(dst, &madeDirs);
        if (stat(dst.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            c.fail++;
            if (c.error.empty()) c.error = trDyn("Some files could not be copied.");
            report();
        }
    }
    bool clean = !cancelled() && c.fail == 0;
    const bool anyMove = std::any_of(pairs.begin(), pairs.end(), [](const Pair& p) { return p.move; });
    if (clean && anyMove) {
        // Everything copied on its drive before any original goes: one flush per destination drive,
        // and a write error it reports fails the move (the originals all stay).
        std::set<dev_t> flushed;
        for (const auto& pr : pairs) {
            struct stat st;
            const std::string d = stat(pr.dst.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? pr.dst : dirOf(pr.dst);
            if (stat(d.c_str(), &st) != 0 || !flushed.insert(st.st_dev).second) continue;
            if (const int err = syncDir(d, io)) {
                ALOGW("ps3menu: move: flushing %s failed: %s", d.c_str(), strerror(err));
                c.fail++;
                if (c.error.empty()) c.error = trDyn("Some files could not be copied.");
                report();
                clean = false;
                break;
            }
        }
    }
    // A move: point each group's program at the copy, then take the originals away (the ones of a
    // group whose program could not be pointed at it stay where they were). Once that starts the
    // job can no longer be cancelled.
    std::vector<Conflict> conflicts;
    std::vector<int> failedGroups;
    bool backupFailed = false, dstMissing = false;
    if (anyMove && !clean) {
        // Cancelled or failed: the card gets back what it had (the files and folders this job made there).
        for (const auto& it : items) {
            if (!it.created) continue;
            unlink(join(pairs[it.pair].dst, it.rel).c_str());
        }
        for (auto d = madeDirs.rbegin(); d != madeDirs.rend(); ++d) rmdir(d->c_str());
    }
    if (clean && anyMove) {
        res.committed = true;
        if (h.committed) h.committed();
        std::set<int> groups;
        for (const auto& p : pairs) if (p.group >= 0) groups.insert(p.group);
        auto relink = [&](size_t i) {   // a source renamed: its real path again
            srcReal[i] = realParent(pairs[i].src);
        };
        for (int g : groups) {
            c.status = trDyn("Updating settings...");
            report();
            if (h.relocate && !h.relocate(g)) { failedGroups.push_back(g); continue; }
            // A folder its program has no setting for: set aside (emptied below like any original) and
            // replaced by a link to its copy. Any of the group's links failing puts them all back.
            std::vector<std::pair<size_t, std::string>> linked;   // pair, its original's own path
            bool ok = true;
            for (size_t i = 0; i < pairs.size() && ok; i++) {
                if (pairs[i].group != g || pairs[i].linkTo.empty()) continue;
                const std::string src = pairs[i].src;
                ok = linkFolder(&pairs[i].src, pairs[i].linkTo, h);
                if (ok) { linked.emplace_back(i, src); relink(i); }
            }
            if (!ok) {
                for (const auto& l : linked) {
                    unlink(l.second.c_str());
                    if (pairs[l.first].src != l.second) rename(pairs[l.first].src.c_str(), l.second.c_str());
                    pairs[l.first].src = l.second;
                    relink(l.first);
                }
                failedGroups.push_back(g);
            }
        }
        auto failed = [&](int group) { return std::find(failedGroups.begin(), failedGroups.end(), group) != failedGroups.end(); };
        // A group left where it was: what this job put on the card for it goes, or a later move
        // would keep it over newer originals.
        for (const auto& it : items)
            if (it.created && failed(pairs[it.pair].group)) unlink(join(pairs[it.pair].dst, it.rel).c_str());
        c.status = trDyn("Removing moved files...");
        report();
        sync();   // the settings and links just changed (the copies were flushed above)
        auto skipped = [&](const Pair& pr) { return !pr.move || failed(pr.group); };
        std::set<std::string> written;   // what this job put on the card
        for (const auto& it : items)
            if (it.result == 0) written.insert(join(pairs[it.pair].dst, it.rel));
        // The clashing originals to the backup first: if one cannot go there, nothing is deleted.
        for (const auto& it : items) {
            const Pair& pr = pairs[it.pair];
            if (skipped(pr) || it.result != 1 || pr.backup.empty() || srcReal[it.pair].empty()) continue;
            const std::string src = join(srcReal[it.pair], it.rel);
            const std::string dst = join(pr.dst, it.rel);
            std::string bak = join(pr.backup, it.rel);
            makeDirs(dirOf(bak), nullptr);
            struct stat bst;
            if (lstat(bak.c_str(), &bst) == 0) {   // one there already (this job's, or an earlier one's): both kept
                const size_t sl = bak.find_last_of('/'), dot = bak.find_last_of('.');
                const bool hasExt = dot != std::string::npos && dot > sl + 1;
                const std::string stem = hasExt ? bak.substr(0, dot) : bak, ext = hasExt ? bak.substr(dot) : "";
                for (int n = 2; lstat(bak.c_str(), &bst) == 0; n++) bak = stem + " (" + std::to_string(n) + ")" + ext;
            }
            bool moved = false;
            const int pfd = openNoFollow(dirOf(src), O_PATH | O_DIRECTORY);
            if (pfd >= 0) {
                const std::string name = nameOf(src);
                moved = renameat(pfd, name.c_str(), AT_FDCWD, bak.c_str()) == 0;
                if (!moved) {   // another drive: copied, then the original removed
                    const int in = openat(pfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                    struct stat ist;
                    moved = in >= 0 && fstat(in, &ist) == 0 && S_ISREG(ist.st_mode) && copyFd(in, bak, COPY_NEW | COPY_FSYNC, io)
                            && unlinkat(pfd, name.c_str(), 0) == 0;
                    if (in >= 0) close(in);
                }
                close(pfd);
            }
            if (moved) {
                struct stat dst2;
                const int64_t dsize = lstat(dst.c_str(), &dst2) == 0 && S_ISREG(dst2.st_mode) ? (int64_t)dst2.st_size : -1;
                conflicts.push_back({bak, dst, written.count(dst) > 0, dsize});
            } else {
                ALOGW("ps3menu: move: %s could not go to the backup folder", src.c_str());
                backupFailed = true;
                break;
            }
        }
        if (!backupFailed) {
            for (const auto& it : items) {
                const Pair& pr = pairs[it.pair];
                if (skipped(pr) || (it.result != 0 && it.result != 4) || srcReal[it.pair].empty()) continue;
                const std::string src = join(srcReal[it.pair], it.rel);
                // Its copy must still be on the card, whole (pulled out meanwhile?).
                struct stat st, sst;
                if (lstat(join(pr.dst, it.rel).c_str(), &st) != 0 || lstat(src.c_str(), &sst) != 0
                    || (S_ISREG(sst.st_mode) && st.st_size != sst.st_size)) {
                    dstMissing = true;
                    continue;
                }
                unlinkNoFollow(src, 0);
            }
            // Merged originals last: once the file holding their data has gone to the card or the backup.
            for (const auto& it : items) {
                const Pair& pr = pairs[it.pair];
                if (skipped(pr) || it.result != 3 || srcReal[it.pair].empty()) continue;
                bool waiting = false;
                for (const auto& m : pr.mergedInto) waiting = waiting || access((m + "/" + it.rel).c_str(), F_OK) == 0;
                if (waiting) { dstMissing = true; continue; }
                unlinkNoFollow(join(srcReal[it.pair], it.rel), 0);
            }
            for (const auto& j : junk)
                if (!skipped(pairs[j.first]) && !srcReal[j.first].empty()) unlinkNoFollow(join(srcReal[j.first], j.second), 0);
        }
        // The folders left empty, the moved folder itself included. A folder linked without one
        // before is the link now: never followed into the card.
        for (size_t i = 0; i < pairs.size(); i++) {
            const Pair& pr = pairs[i];
            struct stat st;
            if (!pr.move || failed(pr.group) || srcReal[i].empty() || lstat(srcReal[i].c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            const int pfd = openNoFollow(dirOf(srcReal[i]), O_PATH | O_DIRECTORY);
            if (pfd < 0) continue;
            pruneAt(pfd, nameOf(srcReal[i]).c_str());
            close(pfd);
        }
    }
    if (!failedGroups.empty()) {
        c.fail += (int)failedGroups.size();
        if (c.error.empty()) c.error = trDyn("Some programs could not be set to use the microSD card.");
    }
    if (backupFailed || dstMissing) {
        c.fail++;
        if (c.error.empty())
            c.error = backupFailed ? trDyn("Some files could not be put in the backup folder. Nothing was deleted.")
                                   : trDyn("Some copies are missing from the microSD card. Their originals were kept.");
    }
    report();
    res.conflicts = std::move(conflicts);
    res.failedGroups = std::move(failedGroups);
    res.incomplete = backupFailed || dstMissing;
    return res;
}

}  // namespace nanofilejob
}  // namespace android
