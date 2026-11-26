// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Blkid against images built here byte by byte: exFAT, FAT32, FAT16, ext4, GPT and MBR.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/VolumePolicy.h"
#include "gtest/gtest.h"
#include "io/Blkid.h"

using namespace thimsy::session;

namespace {

struct Image {
    std::vector<uint8_t> b;
    explicit Image(size_t size) : b(size, 0) {}
    void u8(size_t off, uint8_t v) { b[off] = v; }
    void u16(size_t off, uint16_t v) {
        b[off] = v & 0xFF;
        b[off + 1] = v >> 8;
    }
    void u32(size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
    }
    void u64(size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) b[off + i] = (v >> (8 * i)) & 0xFF;
    }
    void str(size_t off, const char* s) { memcpy(&b[off], s, strlen(s)); }
    void utf16(size_t off, const char* s) {
        for (size_t i = 0; s[i]; ++i) u16(off + 2 * i, static_cast<uint8_t>(s[i]));
    }
    Reader reader() const {
        return [this](uint64_t off, void* buf, size_t len) {
            if (off + len > b.size()) return false;
            memcpy(buf, &b[off], len);
            return true;
        };
    }
};

// GUID text -> GPT's mixed-endian bytes.
void guid(Image* img, size_t off, const char* text) {
    unsigned v[16];
    sscanf(text, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
           &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
    uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) img->b[off + i] = static_cast<uint8_t>(v[order[i]]);
}

// exFAT: 512-byte sectors, 8 sectors per cluster, heap at sector 64, root directory at cluster 4.
Image exfat(bool dirty, const char* label) {
    Image img(64 * 512 + 8 * 4096);
    img.u8(0, 0xEB);
    img.str(3, "EXFAT   ");
    img.u32(88, 64);        // ClusterHeapOffset
    img.u32(96, 4);         // FirstClusterOfRootDirectory
    img.u32(100, 0x9602C755);
    img.u16(106, dirty ? 0x0002 : 0);
    img.u8(108, 9);         // BytesPerSectorShift
    img.u8(109, 3);         // SectorsPerClusterShift
    img.u16(510, 0xAA55);
    size_t root = 64 * 512 + (4 - 2) * 4096;
    img.u8(root, 0x81);     // allocation bitmap entry first
    if (label) {
        img.u8(root + 32, 0x83);
        img.u8(root + 33, static_cast<uint8_t>(strlen(label)));
        img.utf16(root + 34, label);
    }
    return img;
}

// FAT32: 512-byte sectors, 1 sector per cluster, 32 reserved, 2 FATs of 600 sectors, root cluster 2.
Image fat32(bool bootDirty, bool fatClean, const char* rootLabel) {
    Image img(2 * 1024 * 1024);
    img.u8(0, 0xEB);
    img.u8(1, 0x58);
    img.u8(2, 0x90);
    img.str(3, "MSWIN4.1");
    img.u16(11, 512);
    img.u8(13, 1);
    img.u16(14, 32);
    img.u8(16, 2);
    img.u16(17, 0);
    img.u16(19, 0);
    img.u8(21, 0xF8);
    img.u16(22, 0);
    img.u32(32, 70000);
    img.u32(36, 600);
    img.u32(44, 2);
    img.u8(65, bootDirty ? 0x01 : 0x00);
    img.u8(66, 0x29);
    img.u32(67, 0x1234ABCD);
    img.str(71, "BOOTLABEL  ");
    img.str(82, "FAT32   ");
    img.u16(510, 0xAA55);
    size_t fat = 32 * 512;
    img.u32(fat, 0x0FFFFFF8);
    img.u32(fat + 4, fatClean ? 0x0FFFFFFF : 0x07FFFFFF);
    size_t root = (32 + 2 * 600) * 512;
    if (rootLabel) {
        img.str(root, "LONGNAME   ");  // a long-name entry first, skipped
        img.u8(root + 11, 0x0F);
        char name[12];
        snprintf(name, sizeof name, "%-11s", rootLabel);
        img.str(root + 32, name);
        img.u8(root + 32 + 11, 0x08);
    }
    return img;
}

// FAT16: 512-byte sectors, 4 sectors per cluster, 4 reserved, 2 FATs of 32 sectors, 512 root entries.
Image fat16(bool fatClean) {
    Image img(1024 * 1024);
    img.u8(0, 0xEB);
    img.u8(1, 0x3C);
    img.u8(2, 0x90);
    img.u16(11, 512);
    img.u8(13, 4);
    img.u16(14, 4);
    img.u8(16, 2);
    img.u16(17, 512);
    img.u16(19, 40000);
    img.u8(21, 0xF8);
    img.u16(22, 32);
    img.u8(37, 0);
    img.u8(38, 0x29);
    img.u32(39, 0x00C0FFEE);
    img.str(43, "NO NAME    ");
    img.str(54, "FAT16   ");
    img.u16(510, 0xAA55);
    size_t fat = 4 * 512;
    img.u16(fat, 0xFFF8);
    img.u16(fat + 2, fatClean ? 0xFFFF : 0x7FFF);
    return img;
}

Image ext4() {
    Image img(4096);
    img.u16(1024 + 56, 0xEF53);
    img.u16(1024 + 58, 0x0001);
    const uint8_t uuid[16] = {0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x41, 0x62, 0x83, 0x94, 0xa5, 0xb6, 0xc7, 0xd8, 0xe9, 0xfa};
    memcpy(&img.b[1024 + 104], uuid, 16);
    img.str(1024 + 120, "storage");
    return img;
}

// A GPT with Thimsy's card layout (E.6): entries 1-4 and optionally 5.
Image gpt(const char* gamesType) {
    Image img(64 * 512);
    img.u16(510, 0xAA55);
    img.u8(446 + 4, 0xEE);
    img.u32(446 + 8, 1);
    img.u32(446 + 12, 0xFFFFFFFF);
    const uint32_t count = 128, esize = 128;
    struct E {
        const char* type;
        const char* name;
        uint64_t first, last;
    } es[] = {
        {kTypeLinuxReserved, "uboot", 16384, 32767},
        {kTypeLinuxReserved, "boot", 49152, 131071},
        {"B921B045-1DF0-41C3-AF44-4C6F280D3FAE", "root", 212992, 2310143},
        {kTypeLinuxHome, "data", 4407296, 4931583},
        {gamesType, "games", 4931584, 30000000},
    };
    size_t arr = 2 * 512;
    for (int i = 0; i < 5; ++i) {
        if (!es[i].type) continue;
        size_t e = arr + i * esize;
        guid(&img, e, es[i].type);
        guid(&img, e + 16, "11111111-2222-3333-4444-555555555555");
        img.u8(e + 16, static_cast<uint8_t>(i + 1));
        img.u64(e + 32, es[i].first);
        img.u64(e + 40, es[i].last);
        img.utf16(e + 56, es[i].name);
    }
    size_t h = 512;
    img.str(h, "EFI PART");
    img.u32(h + 8, 0x00010000);
    img.u32(h + 12, 92);
    img.u64(h + 24, 1);
    img.u64(h + 72, 2);
    img.u32(h + 80, count);
    img.u32(h + 84, esize);
    img.u32(h + 88, crc32(&img.b[arr], count * esize));
    img.u32(h + 16, crc32(&img.b[h], 92));
    return img;
}

}  // namespace

TEST(Blkid, Crc32) {
    EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u);
    EXPECT_EQ(crc32("", 0), 0u);
}

TEST(Blkid, ExfatSerialLabelAndDirtyFlag) {
    FsInfo fs;
    Image img = exfat(false, "THIMSY");
    ASSERT_TRUE(probeFs(img.reader(), &fs));
    EXPECT_EQ(fs.type, "exfat");
    EXPECT_EQ(fs.serial, "9602-C755");
    EXPECT_EQ(fs.label, "THIMSY");
    EXPECT_FALSE(fs.dirty);
    Image dirty = exfat(true, nullptr);
    ASSERT_TRUE(probeFs(dirty.reader(), &fs));
    EXPECT_TRUE(fs.dirty);
    EXPECT_EQ(fs.label, "");
}

TEST(Blkid, Fat32ReadsTheRootDirectoryLabelAndBothDirtyFlags) {
    FsInfo fs;
    Image clean = fat32(false, true, "GAMES");
    ASSERT_TRUE(probeFs(clean.reader(), &fs));
    EXPECT_EQ(fs.type, "vfat");
    EXPECT_EQ(fs.serial, "1234-ABCD");
    EXPECT_EQ(fs.label, "GAMES");
    EXPECT_FALSE(fs.dirty);
    Image noRootLabel = fat32(false, true, nullptr);
    ASSERT_TRUE(probeFs(noRootLabel.reader(), &fs));
    EXPECT_EQ(fs.label, "BOOTLABEL");
    Image bootDirty = fat32(true, true, nullptr);
    ASSERT_TRUE(probeFs(bootDirty.reader(), &fs));
    EXPECT_TRUE(fs.dirty);
    Image fatDirty = fat32(false, false, nullptr);
    ASSERT_TRUE(probeFs(fatDirty.reader(), &fs));
    EXPECT_TRUE(fs.dirty);
}

TEST(Blkid, Fat16) {
    FsInfo fs;
    Image img = fat16(true);
    ASSERT_TRUE(probeFs(img.reader(), &fs));
    EXPECT_EQ(fs.type, "vfat");
    EXPECT_EQ(fs.serial, "00C0-FFEE");
    EXPECT_EQ(fs.label, "");  // "NO NAME"
    EXPECT_FALSE(fs.dirty);
    Image dirty = fat16(false);
    ASSERT_TRUE(probeFs(dirty.reader(), &fs));
    EXPECT_TRUE(fs.dirty);
}

TEST(Blkid, Ext4UuidAndLabel) {
    FsInfo fs;
    Image img = ext4();
    ASSERT_TRUE(probeFs(img.reader(), &fs));
    EXPECT_EQ(fs.type, "ext4");
    EXPECT_EQ(fs.serial, "0b1c2d3e-4f50-4162-8394-a5b6c7d8e9fa");
    EXPECT_EQ(fs.label, "storage");
}

TEST(Blkid, NothingKnown) {
    FsInfo fs;
    Image zero(8192);
    EXPECT_FALSE(probeFs(zero.reader(), &fs));
    EXPECT_EQ(fs.type, "");
    Image ntfs(8192);
    ntfs.u8(0, 0xEB);
    ntfs.str(3, "NTFS    ");
    ntfs.u16(11, 512);
    ntfs.u8(13, 8);
    ntfs.u16(510, 0xAA55);
    EXPECT_FALSE(probeFs(ntfs.reader(), &fs));
}

TEST(Blkid, GptEntriesWithTypesAndNames) {
    PartTable t;
    std::string err;
    Image img = gpt(kTypeBasicData);
    ASSERT_TRUE(readPartTable(img.reader(), 512, &t, &err));
    EXPECT_EQ(t.scheme, "gpt");
    ASSERT_EQ(t.entries.size(), 5u);
    ASSERT_NE(t.entry(5), nullptr);
    EXPECT_EQ(t.entry(5)->type, kTypeBasicData);
    EXPECT_EQ(t.entry(5)->name, "games");
    EXPECT_EQ(t.entry(4)->type, kTypeLinuxHome);
    EXPECT_EQ(t.entry(1)->type, kTypeLinuxReserved);
    EXPECT_EQ(t.entry(1)->name, "uboot");
    EXPECT_EQ(t.entry(6), nullptr);
    Image reserved = gpt(kTypeLinuxReserved);
    ASSERT_TRUE(readPartTable(reserved.reader(), 512, &t, &err));
    EXPECT_EQ(t.entry(5)->type, kTypeLinuxReserved);
}

TEST(Blkid, GptWithABadCrcIsRefused) {
    PartTable t;
    std::string err;
    Image img = gpt(kTypeBasicData);
    img.b[2 * 512 + 4 * 128 + 56] ^= 1;  // the games name, without fixing the array CRC
    EXPECT_FALSE(readPartTable(img.reader(), 512, &t, &err));
    EXPECT_NE(err.find("CRC"), std::string::npos);
    EXPECT_EQ(t.scheme, "");
}

TEST(Blkid, MbrTypes) {
    Image img(4096);
    img.u16(510, 0xAA55);
    img.u8(446 + 4, 0x0C);
    img.u32(446 + 8, 2048);
    img.u32(446 + 12, 100000);
    img.u8(446 + 16 + 4, 0x05);
    img.u32(446 + 16 + 8, 102048);
    img.u32(446 + 16 + 12, 5000);
    PartTable t;
    ASSERT_TRUE(readPartTable(img.reader(), 512, &t));
    EXPECT_EQ(t.scheme, "mbr");
    ASSERT_EQ(t.entries.size(), 2u);
    EXPECT_EQ(t.entry(1)->type, "0c");
    EXPECT_EQ(t.entry(2)->type, "05");
    Image none(4096);
    EXPECT_FALSE(readPartTable(none.reader(), 512, &t));
}

TEST(Blkid, GuidText) {
    Image img(16);
    guid(&img, 0, kTypeBasicData);
    EXPECT_EQ(guidToString(img.b.data(), true), kTypeBasicData);
    EXPECT_EQ(guidToString(img.b.data(), false), "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7");
}
