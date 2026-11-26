// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Reads what the volume policy needs straight from the media, with no libblkid: the partition
// table (GPT entries' type GUIDs and names, or MBR types) and the filesystem's type, serial, label
// and dirty flags for exFAT, FAT12/16/32 and ext2/3/4. Serials are printed as util-linux and
// Android's blkid print them: FAT and exFAT "XXXX-XXXX" (upper case), ext4 the UUID in lower case
// with dashes.
//
// The parsers take a Reader, so tests feed them images built in memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/VolumePolicy.h"

namespace thimsy {
namespace session {

// Reads len bytes at off into buf; false on a short read or an error.
using Reader = std::function<bool(uint64_t off, void* buf, size_t len)>;

struct PartEntry {
    int index = 0;          // 1-based table entry
    std::string type;       // GPT: type GUID upper case; MBR: two lower-case hex digits
    std::string name;       // GPT name (UTF-8)
    std::string partUuid;   // GPT unique GUID, lower case
    uint64_t firstLba = 0;
    uint64_t lastLba = 0;
};

struct PartTable {
    std::string scheme;  // "gpt", "mbr" or empty (none found)
    std::vector<PartEntry> entries;
    const PartEntry* entry(int index) const;
};

// GPT (primary header and entry array, both CRCs checked) or else MBR (primary entries only).
bool readPartTable(const Reader& r, uint32_t sectorSize, PartTable* out, std::string* err = nullptr);
bool readPartTableFile(const std::string& node, uint32_t sectorSize, PartTable* out, std::string* err = nullptr);

// Fills fs with the first filesystem recognised (exFAT, FAT, ext). False when none is.
bool probeFs(const Reader& r, FsInfo* fs);
bool probeFsFile(const std::string& node, FsInfo* fs);

// Standard CRC-32 (zlib's), as GPT uses.
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);
// GPT's mixed-endian GUID bytes -> "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX".
std::string guidToString(const uint8_t* b, bool upper);

}  // namespace session
}  // namespace thimsy
