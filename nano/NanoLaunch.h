/*
 * Copyright (C) 2026 GammaOS
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

// Pure helpers for game launches and anything else that hands untrusted text (file names,
// properties an app can set, downloaded lists) to a shell or to the framework's intent parser.
// No Android or NanoMenu dependencies, so the host unit tests can exercise them directly.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nanolaunch {

// POSIX shell single-quote ('\'' splices embedded quotes).
std::string shellQuote(const std::string& s);

// Replace every occurrence of `from`, resuming after each replacement, so a `to` that itself
// contains `from` is never expanded again.
std::string replaceAll(std::string s, const std::string& from, const std::string& to);

// Percent-encode everything but the RFC 3986 unreserved set (A-Z a-z 0-9 - . _ ~), byte-wise.
std::string urlEncode(const std::string& in);

// The path an app sees for a ROM: /data/media/0/... as /sdcard/..., /mnt/media_rw/<vol>/... as
// /storage/<vol>/...; anything else unchanged.
std::string appVisiblePath(const std::string& path);

// file:// URI for an absolute path: urlEncode per segment, '/' kept.
std::string fileUri(const std::string& path);

// Storage Access Framework tree root ("primary%3AROMs%2Fnes") and encoded bare file name for a
// ROM path on internal storage (/data/media/0, /sdcard, /storage/emulated/0) or an external
// volume (/storage/<UUID>, /mnt/media_rw/<UUID>). For any other prefix the tree is
// ROMs/<fallbackRomDir> when fallbackRomDir is non-empty, else the path's own leading dirs.
void safTree(const std::string& fullRomPath, const std::string& fallbackRomDir,
             std::string& treeRoot, std::string& encodedFilename);

// content://com.android.externalstorage.documents/tree/<root>/document/<root>%2F<file>
std::string safContentUri(const std::string& fullRomPath, const std::string& fallbackRomDir);

// A value can travel as one token of the tab separated intent file (RootWindowContainer
// parseAmIntent) and one line of a line-based file: no tab, CR, LF or NUL.
bool tokenSafe(const std::string& v);

struct LaunchVars {
    std::string fileUri;    // {file.uri}
    std::string filePath;   // {file.path}; also {file.storagepath} / {file.storagedir} (storagePath)
    std::string fileMime = "application/octet-stream";   // {file.mime}
    std::string fileContent;   // {file.content}: the game file's launch ID (see launchFileId)
};

// The launch ID a game file holds, as ES-DE's %INJECT% reads it: its first line, trimmed (a UTF-8
// byte order mark before it dropped). ScummVM launches a game by the ID of its entry in ScummVM's
// game list (a <name>.scummvm file holding "sky"). "" unless the ID is 1-64 of [A-Za-z0-9_-], the
// characters ScummVM allows in one, not starting with '-', so it can never be read as an option, a
// URI scheme or a second token.
std::string launchFileId(const std::string& text);

// launchFileId of the first 4 KB of the file at path; "" when it is not a regular file or cannot be read.
std::string readLaunchFileId(const std::string& path);

// True when the template or the extra launch args use {file.content}.
bool usesFileContent(const std::string& tmpl, const std::string& extraArgs);

// An app-visible path as the storage volume names it (StorageVolume.getDirectory(), the form an
// app's Storage Access Framework folder is resolved to): /sdcard/... and /data/media/0/... as
// /storage/emulated/0/..., /mnt/media_rw/<vol>/... as /storage/<vol>/...; anything else unchanged.
std::string storagePath(const std::string& path);

// Split an am-start template (plus the system's extra launch args) on whitespace, then
// substitute {file.uri} / {file.path} / {file.mime} / {file.content} / {file.storagepath} /
// {file.storagedir} (storagePath of {file.path}, and its folder) inside each token, so a value
// with spaces stays one token and a value can never add tokens of its own.
//
// A token that starts with '"' runs to the next '"' (\" inside it is a '"') and stays one token,
// spaces and all: an argument string the app splits again itself, as MAME4droid splits its
// cli_params extra (on single spaces; a '...' span is one argument; no escapes). Its spaces must
// be single ones between arguments, and each '...' span a whole argument. In it a placeholder must
// be inside a '...' span, and its value must be non-empty, hold no ' and not start with '-', so it
// stays that one argument and is never read as an option.
//
// Returns false (tokens cleared) when the template is malformed (an unclosed '"', text right after
// one, a misplaced ' in a quoted token), when a value the template actually uses is not tokenSafe
// or breaks a quoted token's rule, or is an empty {file.content}.
bool buildLaunchIntent(const std::string& tmpl, const std::string& extraArgs,
                       const LaunchVars& vars, std::vector<std::string>& tokens);

// The tab separated form for /data/system/nano_launch_intent.txt.
std::string joinTabbed(const std::vector<std::string>& tokens);

// `am start` argument string for /bin/sh: every token quoted except plain option words.
std::string joinShell(const std::vector<std::string>& tokens);

// One am option and its values, e.g. {"-es", {key, value}}.
struct AmArg {
    std::string flag;
    std::vector<std::string> values;
};

// Join fixed option flags with their values. tabbed: the intent-file form (flag "-es" kept).
// Shell: flags bare ("-es" becomes am's "--es"), every value single-quoted. Returns false when
// a value is not tokenSafe.
bool joinAmArgs(const std::vector<AmArg>& args, bool tabbed, std::string& out);

// ^[A-Za-z0-9_.]+$, up to 255 bytes.
bool isValidPackageName(const std::string& pkg);

// <package>/<class>, the class ^[A-Za-z0-9_.$]+$ (".Short" form allowed).
bool isValidComponent(const std::string& comp);

// One directory name from a downloaded list: ^[A-Za-z0-9._-]{1,64}$, not starting with '.'
// (so neither "." nor ".." nor a hidden staging dir).
bool isValidRepoName(const std::string& name);

// One Recently Played row.
struct RecentRecord {
    std::string romPath;
    std::string coreSo;
    std::string launchPkg;
    std::string launchIntent;
    std::string displayName;
    std::string systemName;
    std::string romDir;
};

// Versioned format: a header line, then one line per entry with tab separated fields and
// \\ \t \n \r escapes, so no field can shift into another.
std::string serializeRecents(const std::vector<RecentRecord>& recents);

// Reads the versioned format, or the legacy one (7 lines per entry, blank line between) for
// migration. Entries without a ROM path are skipped; at most `max` entries are returned.
std::vector<RecentRecord> parseRecents(const std::string& content, size_t max);

}  // namespace nanolaunch
