// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "io/Blkid.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace thimsy {
namespace session {
namespace {

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32); }

void appendUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// UTF-16LE code units (stopping at a NUL) to UTF-8; lone surrogates become U+FFFD.
std::string utf16le(const uint8_t* p, size_t units) {
    std::string out;
    for (size_t i = 0; i < units; ++i) {
        uint32_t u = le16(p + 2 * i);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
            uint32_t lo = le16(p + 2 * (i + 1));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                appendUtf8(&out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
        appendUtf8(&out, u);
    }
    return out;
}

// An 8.3 directory-entry label: trailing spaces cut, bytes above 0x7F read as Latin-1.
std::string fatLabel(const uint8_t* p) {
    size_t n = 11;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) --n;
    std::string out;
    for (size_t i = 0; i < n; ++i) appendUtf8(&out, p[i]);
    if (out == "NO NAME") out.clear();
    return out;
}

std::string serial4x4(uint32_t s) {
    char buf[16];
    snprintf(buf, sizeof buf, "%04X-%04X", static_cast<unsigned>(s >> 16), static_cast<unsigned>(s & 0xFFFF));
    return buf;
}

bool probeExfat(const Reader& r, FsInfo* fs) {
    uint8_t b[512];
    if (!r(0, b, sizeof b) || memcmp(b + 3, "EXFAT   ", 8) != 0) return false;
    unsigned bpsShift = b[108], spcShift = b[109];
    if (bpsShift < 9 || bpsShift > 12 || spcShift > 25 - bpsShift) return false;
    fs->type = "exfat";
    fs->serial = serial4x4(le32(b + 100));
    fs->dirty = (le16(b + 106) & 0x0002) != 0;
    fs->label.clear();
    uint64_t bps = 1ull << bpsShift;
    uint64_t clusterSize = bps << spcShift;
    uint32_t heap = le32(b + 88);
    uint32_t rootClus = le32(b + 96);
    if (rootClus < 2) return true;
    uint64_t off = heap * bps + (rootClus - 2) * clusterSize;
    size_t len = static_cast<size_t>(std::min<uint64_t>(clusterSize, 4096));
    std::vector<uint8_t> dir(len);
    if (!r(off, dir.data(), len)) return true;
    for (size_t i = 0; i + 32 <= len; i += 32) {
        uint8_t type = dir[i];
        if (type == 0x00) break;
        if (type == 0x83) {
            size_t count = std::min<size_t>(dir[i + 1], 11);
            fs->label = utf16le(&dir[i + 2], count);
            break;
        }
    }
    return true;
}

bool probeFat(const Reader& r, FsInfo* fs) {
    uint8_t b[512];
    if (!r(0, b, sizeof b)) return false;
    if (b[510] != 0x55 || b[511] != 0xAA) return false;
    if (b[0] != 0xEB && b[0] != 0xE9) return false;
    uint32_t bps = le16(b + 11);
    uint32_t spc = b[13];
    uint32_t reserved = le16(b + 14);
    uint32_t numFats = b[16];
    uint32_t rootEnt = le16(b + 17);
    uint32_t tot16 = le16(b + 19);
    uint32_t fatSz16 = le16(b + 22);
    uint32_t tot32 = le32(b + 32);
    uint32_t fatSz32 = le32(b + 36);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;
    if (reserved == 0 || numFats == 0 || numFats > 2) return false;
    uint32_t fatSz = fatSz16 ? fatSz16 : fatSz32;
    uint32_t total = tot16 ? tot16 : tot32;
    if (fatSz == 0 || total == 0) return false;
    uint32_t rootDirSectors = (rootEnt * 32 + bps - 1) / bps;
    uint64_t meta = reserved + static_cast<uint64_t>(numFats) * fatSz + rootDirSectors;
    if (meta >= total) return false;
    uint64_t clusters = (total - meta) / spc;
    bool fat32 = fatSz16 == 0;
    bool fat12 = !fat32 && clusters < 4085;

    const uint8_t* ext = fat32 ? b + 64 : b + 36;  // drive number, state, signature, serial, label
    fs->type = "vfat";
    fs->serial.clear();
    fs->label.clear();
    if (ext[2] == 0x29 || ext[2] == 0x28) fs->serial = serial4x4(le32(ext + 3));
    if (ext[2] == 0x29) fs->label = fatLabel(ext + 7);
    fs->dirty = (ext[1] & 0x01) != 0;

    // FAT[1]'s clean-shutdown bit, as dosfstools reads it.
    uint64_t fatOff = static_cast<uint64_t>(reserved) * bps;
    uint8_t f[8];
    if (!fat12 && r(fatOff, f, sizeof f)) {
        if (fat32) {
            if ((le32(f + 4) & 0x08000000u) == 0) fs->dirty = true;
        } else if ((le16(f + 2) & 0x8000u) == 0) {
            fs->dirty = true;
        }
    }

    // The root directory's volume-label entry wins over the boot sector's, as in util-linux.
    uint64_t dirOff = 0;
    size_t dirLen = 0;
    if (fat32) {
        uint32_t rootClus = le32(b + 44);
        if (rootClus >= 2) {
            uint64_t data = (reserved + static_cast<uint64_t>(numFats) * fatSz32) * bps;
            dirOff = data + static_cast<uint64_t>(rootClus - 2) * spc * bps;
            dirLen = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(spc) * bps, 16384));
        }
    } else {
        dirOff = (reserved + static_cast<uint64_t>(numFats) * fatSz16) * bps;
        dirLen = std::min<size_t>(rootEnt * 32u, 16384);
    }
    if (dirLen >= 32) {
        std::vector<uint8_t> dir(dirLen);
        if (r(dirOff, dir.data(), dirLen)) {
            for (size_t i = 0; i + 32 <= dirLen; i += 32) {
                const uint8_t* e = &dir[i];
                if (e[0] == 0x00) break;
                if (e[0] == 0xE5) continue;
                uint8_t attr = e[11];
                if (attr == 0x0F) continue;  // a long-name entry
                if ((attr & 0x08) && !(attr & 0x10)) {
                    uint8_t name[11];
                    memcpy(name, e, 11);
                    if (name[0] == 0x05) name[0] = 0xE5;
                    fs->label = fatLabel(name);
                    break;
                }
            }
        }
    }
    return true;
}

bool probeExt(const Reader& r, FsInfo* fs) {
    uint8_t sb[256];
    if (!r(1024, sb, sizeof sb)) return false;
    if (le16(sb + 56) != 0xEF53) return false;
    fs->type = "ext4";
    // ext's UUID is stored big-endian (RFC 4122 order), unlike GPT's mixed-endian GUIDs.
    char buf[40];
    const uint8_t* u = sb + 104;
    snprintf(buf, sizeof buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2],
             u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    fs->serial = buf;
    size_t n = 0;
    while (n < 16 && sb[120 + n]) ++n;
    fs->label.assign(reinterpret_cast<const char*>(sb + 120), n);
    fs->dirty = (le16(sb + 58) & 0x0001) == 0;
    return true;
}

Reader fileReader(int fd) {
    return [fd](uint64_t off, void* buf, size_t len) {
        size_t done = 0;
        while (done < len) {
            ssize_t n = pread(fd, static_cast<char*>(buf) + done, len - done, static_cast<off_t>(off + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    };
}

}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string guidToString(const uint8_t* b, bool upper) {
    char buf[40];
    const char* fmt = upper ? "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X"
                            : "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x";
    snprintf(buf, sizeof buf, fmt, b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
    return buf;
}

const PartEntry* PartTable::entry(int index) const {
    for (const PartEntry& e : entries) {
        if (e.index == index) return &e;
    }
    return nullptr;
}

bool readPartTable(const Reader& r, uint32_t sectorSize, PartTable* out, std::string* err) {
    auto fail = [err](const char* why) {
        if (err) *err = why;
        return false;
    };
    *out = PartTable();
    if (sectorSize < 512 || sectorSize > 4096) return fail("bad sector size");
    std::vector<uint8_t> hdr(sectorSize);
    if (r(sectorSize, hdr.data(), sectorSize) && memcmp(hdr.data(), "EFI PART", 8) == 0) {
        uint32_t hsize = le32(&hdr[12]);
        if (hsize < 92 || hsize > sectorSize) return fail("GPT header size");
        uint32_t want = le32(&hdr[16]);
        std::vector<uint8_t> h(hdr.begin(), hdr.begin() + hsize);
        memset(&h[16], 0, 4);
        if (crc32(h.data(), h.size()) != want) return fail("GPT header CRC");
        uint64_t entLba = le64(&hdr[72]);
        uint32_t count = le32(&hdr[80]);
        uint32_t esize = le32(&hdr[84]);
        uint32_t ecrc = le32(&hdr[88]);
        if (count == 0 || count > 1024 || esize < 128 || esize > 4096 || esize % 8 != 0) return fail("GPT entry array");
        std::vector<uint8_t> arr(static_cast<size_t>(count) * esize);
        if (!r(entLba * sectorSize, arr.data(), arr.size())) return fail("GPT entry array unreadable");
        if (crc32(arr.data(), arr.size()) != ecrc) return fail("GPT entry array CRC");
        out->scheme = "gpt";
        static const uint8_t kZero[16] = {0};
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = &arr[static_cast<size_t>(i) * esize];
            if (memcmp(e, kZero, 16) == 0) continue;
            PartEntry pe;
            pe.index = static_cast<int>(i + 1);
            pe.type = guidToString(e, true);
            pe.partUuid = guidToString(e + 16, false);
            pe.firstLba = le64(e + 32);
            pe.lastLba = le64(e + 40);
            pe.name = utf16le(e + 56, 36);
            out->entries.push_back(pe);
        }
        return true;
    }
    uint8_t mbr[512];
    if (!r(0, mbr, sizeof mbr)) return fail("unreadable");
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return fail("no partition table");
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr + 446 + 16 * i;
        if (e[0] != 0x00 && e[0] != 0x80) return fail("not an MBR");
        if (e[4] == 0xEE) return fail("a protective MBR without a readable GPT");
    }
    out->scheme = "mbr";
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr + 446 + 16 * i;
        if (e[4] == 0) continue;
        PartEntry pe;
        pe.index = i + 1;
        char t[4];
        snprintf(t, sizeof t, "%02x", e[4]);
        pe.type = t;
        pe.firstLba = le32(e + 8);
        pe.lastLba = pe.firstLba + le32(e + 12) - 1;
        out->entries.push_back(pe);
    }
    return true;
}

bool readPartTableFile(const std::string& node, uint32_t sectorSize, PartTable* out, std::string* err) {
    int fd = open(node.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = std::string("open: ") + strerror(errno);
        *out = PartTable();
        return false;
    }
    bool ok = readPartTable(fileReader(fd), sectorSize, out, err);
    close(fd);
    return ok;
}

bool probeFs(const Reader& r, FsInfo* fs) {
    *fs = FsInfo();
    if (probeExfat(r, fs)) return true;
    *fs = FsInfo();
    if (probeFat(r, fs)) return true;
    *fs = FsInfo();
    if (probeExt(r, fs)) return true;
    *fs = FsInfo();
    return false;
}

bool probeFsFile(const std::string& node, FsInfo* fs) {
    int fd = open(node.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *fs = FsInfo();
        return false;
    }
    bool ok = probeFs(fileReader(fd), fs);
    close(fd);
    return ok;
}

}  // namespace session
}  // namespace thimsy
