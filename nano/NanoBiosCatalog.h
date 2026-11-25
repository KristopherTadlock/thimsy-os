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

// The BIOS catalogue: every file the BIOS installer (NanoBiosInstall.cpp) recognises, the dialog
// rows they are shown in (listed ones only), and where each emulator needs them. Names, sizes and
// hashes only. Also the matchers that need no Android code: SHA-1/MD5 of a file, a zip's central
// directory and member check, the PS2 ROMDIR version, the PS Vita PUP header. Plain C++ (std,
// BoringSSL, zlib) so BIOS.md can be generated from it on a host.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace android {
namespace nanobios {

enum Section { SEC_REQUIRED = 0, SEC_SOME_GAMES, SEC_OPTIONAL };

// Regions a file is the recommended one for (bits); 0 = an alternate.
enum { REC_USA = 1, REC_EUR = 2, REC_JPN = 4, REC_ALL = 7 };
enum Region { REGION_USA = 0, REGION_EUR, REGION_JPN };
inline int regionBit(Region r) { return 1 << (int)r; }

enum Match {
    MATCH_SHA1 = 0,    // content hash
    MATCH_MD5,         // content hash (no SHA-1 known)
    MATCH_NAME,        // exact file name (any case) and size (size 0 = any, sizeMax = a range)
    MATCH_ZIPSET,      // a zip whose members cover a known set (zipSets())
    MATCH_FOLDERSET,   // two sentinel files inside a folder tree (blueMSX Machines/ + Databases/)
    MATCH_PS2ROMDIR,   // any 4 MiB PS2 BIOS with a valid ROMDIR/ROMVER
    MATCH_PUP,         // a PS Vita update package (SCEUF header)
    MATCH_COMPANION,   // a PS2 BIOS's .nvm/.mec/.rom1/.rom2/.erom beside it (same base name)
};

// Where a slot installs. Each kind has a base folder resolved at install time.
enum SlotKind {
    SLOT_RA = 0,          // RetroArch system_directory
    SLOT_RA_FOLDER,       // RetroArch system_directory: a whole folder (path = folder name)
    SLOT_ARMSX2,          // Android/data/com.armsx2/files/bios
    SLOT_DOLPHIN_GC,      // Android/data/org.dolphinemu.dolphinemu/files/GC
    SLOT_FLYCAST,         // <Flycast home>/data
    SLOT_DRASTIC,         // <DraStic system folder>/system
    SLOT_DRASTIC_NANO,    // /data/system/drastic-nano/DraStic/system (built-in DS player)
    SLOT_MUPEN,           // Android/data/org.mupen64plusae.v3.fzurita/files
    SLOT_PPSSPP_FONT,     // <PPSSPP memory stick>/PSP/flash0/font
    SLOT_SCUMMVM,         // ScummVM's extrapath
    SLOT_AZAHAR,          // Azahar's folder (CITRA_DIRECTORY)
    SLOT_HAKUX,           // /data/data/com.rfandango.haku_x/files/console
    SLOT_VITA3K,          // installed by Vita3K itself (its install service)
    SLOT_SEEDLESSDS,      // /data/data/com.seedlessds.app/files/SeedlessDS/system
    SLOT_MAME4DROID,      // Android/data/com.seleuco.mame4d2024/files/roms (in Nano's -rompath for it)
    SLOT_KIND_COUNT
};

// Slot flags.
enum {
    SLOTF_SATURN_SEL = 1,   // YabaSanshiro's saturn_bios.bin: only for Japan, or when no other Saturn BIOS is there
    SLOTF_SELECT = 2,       // the emulator has one selection (ARMSX2 bios pref): recommended region first
    SLOTF_IF_ABSENT = 4,    // name-matched: install only when the target does not exist
    SLOTF_PREF_64DD = 8,    // M64Plus FZ: set japanIdlPath64dd when empty
};

struct Slot {
    int kind;
    const char* path;   // under the kind's base; "" = the file's name
    int flags;
};

struct Row {
    const char* id;
    int section;
    bool listed;            // shown in the setup / Install Your Own BIOS Files dialog
    const char* system;     // "Saturn"
    const char* emulators;  // "YabaSanshiro, Beetle Saturn"
    const char* qualifier;  // "" or e.g. "Famicom Disk System games"
    const char* note;       // "" or a short note shown in the row
};

struct File {
    const char* row;        // Row::id
    const char* name;       // canonical name (shown; the target name when a slot path is "")
    const char* desc;       // "USA v2.30", "" = none
    int match;
    uint32_t size;          // bytes (0 = any / unknown)
    uint32_t sizeMax;       // MATCH_NAME: upper bound of a size range (0 = exactly size)
    const char* sha1;       // "" = none
    const char* md5;        // "" = none
    int rec;                // REC_* bits: shown in the row for those regions, 0 = alternate
    std::vector<Slot> slots;
};

struct ZipMember { const char* name; uint32_t size; uint32_t crc; const char* sha1; };
struct ZipSet {
    const char* name;                    // canonical zip name ("neogeo.zip")
    std::vector<ZipMember> members;      // the known members
    std::vector<std::vector<const char*>> need;   // usable when, for every group, one of its members is there
};

const std::vector<Row>& rows();
const std::vector<File>& files();
void setCatalogForTest(const std::vector<File>* f);   // files() returns *f (nullptr: the real one)
const std::vector<ZipSet>& zipSets();
const Row* findRow(const char* id);
const ZipSet* findZipSet(const std::string& name);

// The whole catalogue by system (alphabetical; rows and files in table order), for the All Supported
// Files dialog and BIOS.md. English.
struct AllFile { const File* file; std::string name, desc, size, check, how; };
struct AllGroup { std::string system, emulators; std::vector<const Row*> rows; std::vector<AllFile> files; };
std::vector<AllGroup> allGroups();
std::string sizeText(const File& f);   // "512 KB", "933636 bytes", "256 KB to 4 MB", "any size"
const char* allFilesIntro();

// The systems that need no BIOS, as one English line for the dialog.
const char* needNothingLine();

// The recommended region for a language and region code ("en", "GB"): Japan, Europe or USA.
Region regionForLocale(const std::string& lang, const std::string& region);

// The emulator app a slot kind belongs to ("" = built in), and its name for reports.
const char* slotPackage(int kind);
const char* slotAppName(int kind);
// Where a slot kind installs, for people (BIOS.md): before and after Move Emulator Files to microSD.
const char* slotWhere(int kind);
const char* slotWhereMoved(int kind);

// ---- matchers ----

// A MATCH_NAME entry's check: the name (any case) and, when it has one, the size or size range.
bool nameMatches(const File& f, const std::string& name, uint64_t size);

// A regular file opened for reading (O_NONBLOCK while opening, so a FIFO never blocks; cleared
// after): -1 for anything else. noFollow: a link fails.
int openRegular(const std::string& path, bool noFollow);

// Lower-case hex SHA-1 / MD5 of a file ("" when it cannot be read).
bool hashFile(const std::string& path, std::string* sha1, std::string* md5, bool noFollow = false);   // noFollow: a link fails
std::string sha1Hex(const void* data, size_t len);

struct ZipEntry {
    std::string name;       // as stored (may hold folders)
    uint32_t crc = 0, csize = 0, usize = 0;
    uint16_t method = 0;
    uint32_t localOffset = 0;
};
// A zip's central directory (no zip64; at most 4096 entries). False when it is not a zip.
bool zipDirectory(const std::string& path, std::vector<ZipEntry>* out);
// SHA-1 of a member, inflated (stored or deflated). "" on any error.
std::string zipMemberSha1(const std::string& path, const ZipEntry& e);
// The set a zip covers (members by name, size and CRC32 from the directory, then their SHA-1s
// inflated): nullptr when none. *found gets the matched member names.
const ZipSet* zipMatchSet(const std::string& path, std::vector<std::string>* found);

// A PS2 BIOS image's ROMVER ("0230AC20080220"): "" when the file has no valid ROMDIR.
std::string ps2Romver(const std::string& path);
// "USA v2.30" from a ROMVER, and its region (REGION_* or -1).
std::string ps2RomverLabel(const std::string& romver, int* region);

// A PS Vita update package: the SCEUF header. *fwVersion gets its firmware version (0x03650000).
bool isVitaPup(const std::string& path, uint32_t* fwVersion);

}  // namespace nanobios
}  // namespace android
