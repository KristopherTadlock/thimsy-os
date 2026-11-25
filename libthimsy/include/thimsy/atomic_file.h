// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Crash-safe whole-file writes: write a temp file in the same directory, fsync it, rename it over
// the target, fsync the directory. A reader sees the old file or the new one, never a mix; a writer
// killed before the rename leaves the old file and a stray temp file (".<name>.tmp.<pid>.<n>").
#pragma once

#include <sys/types.h>

#include <string>
#include <string_view>

namespace thimsy {

struct AtomicWriteOptions {
    mode_t mode = 0644;  // applied with fchmod, so the umask does not widen or narrow it
    bool durable = true; // fsync the file and the directory; false for tmpfs (the run dir)
};

// Returns false and fills *err (if given) on failure; the target is then unchanged.
bool writeFileAtomic(const std::string& path, std::string_view data,
                     const AtomicWriteOptions& opts = {}, std::string* err = nullptr);

// Reads a whole file. Returns false if it can't be opened or read (errno is kept).
bool readFile(const std::string& path, std::string* out);

// The temp name prefix writeFileAtomic uses for `path` (".<basename>.tmp."): callers that hold a
// lock over the file may remove stale temp files left by a killed writer.
std::string atomicTempPrefix(const std::string& path);

// Removes stale temp files of `path` (only call while holding the file's writer lock).
void removeStaleTemps(const std::string& path);

// Creates a directory and its parents (like mkdir -p). Existing directories are fine.
bool makeDirs(const std::string& dir, mode_t mode = 0755);

// Test seam: called with the temp path after the temp file is complete and before the rename.
// Process-wide; nullptr (the default) disables it. Tests use it to kill a writer mid-write.
using AtomicWriteHook = void (*)(const char* tempPath);
void setAtomicWriteTestHook(AtomicWriteHook hook);

}  // namespace thimsy
