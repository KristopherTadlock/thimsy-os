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

// The copy engine behind folder copies and the microSD moves (NanoMenu::fileCopyThreadFunc), and
// the File Explorer's copy, move and delete. No Android UI code: the menu's side (progress, cancel,
// closing programs, pointing them at the copy) comes in through Hooks, so host tests can run it.
#pragma once

#include <stdint.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

namespace android {
namespace nanofilejob {

// One folder (or file) of a job, copied to dst; files already at the destination are kept.
// replaceOlder: a newer source replaces the file. required: the source holds the user's current
// files (a chosen folder), so its absence (a card that is out) fails the copy instead of skipping.
// move: after a clean copy (and the group's relocation) the sources that copied are deleted and the
// ones already at the destination go to backup (the conflict prompt then offers to overwrite the
// destination's copy with them). merged: files under src not copied (another file of the job holds
// what they had) and deleted with the rest. overwrite: replace what is at the destination.
struct Pair {
    std::string src, dst;
    bool replaceOlder = false;
    bool required = false;
    bool move = false;
    bool overwrite = false;
    std::string backup;
    std::vector<std::string> merged;
    std::vector<std::string> mergedInto; // where merged's data waits (same relative paths): an original goes once that has
    std::vector<std::string> skipDirs;   // folders under src left out (another pair moves them)
    int group = -1;
    std::string linkTo;                  // a move: a link to here replaces src (its program has no setting for it)
    std::string srcRoot;                 // set: src must really be inside this folder (sdSourceRoot) when the job runs
};

// sameJob: the card's copy came from this move. dstSize: the card's copy's size when it was kept,
// checked again before the internal copy is deleted.
struct Conflict { std::string backup, dst; bool sameJob = false; int64_t dstSize = -1; };

// What the modal shows.
struct Counters { int total = 0, done = 0, hits = 0, skip = 0, fail = 0; std::string error, status; };

// Stand-ins for the system calls a file copy makes, for tests (the real ones when empty).
struct CopyIo {
    std::function<ssize_t(int fd, const void* buf, size_t n)> write;
    std::function<int(int fd)> fsync;
    std::function<int(int fd)> syncfs;
    // A destination that refused renameat2(RENAME_NOREPLACE) once: the fallback from then on.
    mutable bool noReplaceUnsupported = false;
};

// Files run() copies at once. Small files cost mostly their opens, creates and closes, which overlap;
// a big file streams on one thread.
constexpr int kCopyThreads = 4;

// Called on run()'s thread, except rawPath and io's functions: those may be called from several copy
// threads at once.
struct Hooks {
    std::function<bool()> cancelled;
    std::function<void(const Counters&)> progress;                        // after every change
    std::function<int(const char* path, struct statvfs* out)> statvfs;
    std::function<bool()> stopPrograms;                                  // before copying; true: they were, look again
    std::function<void()> committed;                                     // past the copy: no cancelling it now
    std::function<bool(int group)> relocate;                             // point a group's program at the copy
    std::function<void(const std::string& path)> restorecon;             // a link just made
    std::function<std::string(const std::string&)> rawPath;              // a path by its backing mount
    CopyIo io;
    int copyThreads = 0;                                                 // 0: kCopyThreads
};

struct Result {
    Counters counters;
    std::vector<Conflict> conflicts;
    std::vector<int> failedGroups;   // groups whose relocation failed (kept as they were)
    bool committed = false;          // a move past its copy
    bool incomplete = false;         // a move whose backup or deletion stopped part way
};

Result run(std::vector<Pair> pairs, const Hooks& hooks);

// ---- files ----
enum CopyMode {
    COPY_NEW = 0,       // through dst.nanocopy, renamed only where nothing is (EEXIST otherwise)
    COPY_REPLACE,       // through dst.nanocopy, renamed over what is there
    COPY_EXCL_DIRECT,   // dst itself, made new (O_EXCL), with the source's mode
    COPY_FSYNC = 0x100, // flag: flushed before it counts (else the caller syncs its filesystem, syncDir)
};
// Copies an open regular file. Every write and the close are checked, and the copy must hold exactly
// the bytes the source had: anything short is removed. A file over 256 MB, or one with COPY_FSYNC, is
// also flushed. errno on failure (ENOSPC: the destination filled up).
bool copyFd(int in, const std::string& dst, int flags, const CopyIo* io = nullptr);
bool copyFile(const std::string& src, const std::string& dst, int flags, const CopyIo* io = nullptr);   // a link fails
// Opens a path whose every part is a real folder: a link anywhere fails (ELOOP / ENOTDIR).
int openNoFollow(const std::string& path, int flags);
bool unlinkNoFollow(const std::string& path, int flags);
// 0, or the errno (EEXIST: one is there). *unsupported: the filesystem has no RENAME_NOREPLACE (set
// once it refuses it; the check-then-rename fallback is used directly while set).
int renameNoReplace(const std::string& from, const std::string& to, bool* unsupported = nullptr);
// Flushes the filesystem holding a folder (or a file's folder): 0, or the errno of a write that failed.
int syncDir(const std::string& path, const CopyIo* io = nullptr);
std::string realParent(const std::string& p);   // its folder with links resolved, and its own name

// ---- File Explorer ----
// A copy of a file, folder or link (links copied as links; sockets, FIFOs and devices left out).
bool treeCopy(const std::string& src, const std::string& dst, const CopyIo* io = nullptr);
// The same, then what was copied is deleted from src (never through a link; what was left out stays).
bool treeMove(const std::string& src, const std::string& dst, const CopyIo* io = nullptr);
bool treeRemove(const std::string& p);   // never through a link

}  // namespace nanofilejob
}  // namespace android
