// Host tests for NanoArchive (zip via libziparchive, 7z via the LZMA SDK), and comic book manual pages
// read through it (NanoManual).
//
// Zips are built byte by byte at test time so hostile layouts (names, flags, attributes, declared
// sizes) can be written exactly. The 7z fixtures in test/fixtures/archive/ were written by a small
// generator: good.7z (files, a folder, an empty file, "../up.txt", a link, a UTF-8 name), bad.7z
// (good.7z with a damaged stream), copy.7z (method Copy), aes.7z (AES coder), huge.7z (a 600 MB
// declared solid block) and trunc.7z (cut short).

#include "NanoArchive.h"
#include "NanoManual.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <zlib.h>

#include <linux/magic.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../unit/nano_test_util.h"

using nanoarc::EntryData;
using nanoarc::Err;
using nanoarc::ExtractResult;
using nanoarc::Format;
using nanoarc::Index;
using nanoarc::Progress;

namespace {

struct ZEntry {
    std::string name;
    std::string data;
    bool deflate = true;
    int64_t declSize = -1;        // uncompressed size to declare; -1 = the real one
    uint16_t flags = 0x0800;      // bit 11: names are UTF-8
    uint32_t mode = 0100644;      // unix mode, stored in the high half of the external attributes
};

void put16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xFF)); s.push_back((char)(v >> 8)); }
void put32(std::string& s, uint32_t v) { put16(s, (uint16_t)(v & 0xFFFF)); put16(s, (uint16_t)(v >> 16)); }

std::string rawDeflate(const std::string& in) {
    z_stream z{};
    deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&z, in.size()) + 16, '\0');
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    z.next_out = (Bytef*)&out[0];
    z.avail_out = (uInt)out.size();
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

std::string buildZip(const std::vector<ZEntry>& entries) {
    std::string out, cd;
    for (const ZEntry& e : entries) {
        const std::string body = e.deflate ? rawDeflate(e.data) : e.data;
        const uint32_t crc = (uint32_t)crc32(0, (const Bytef*)e.data.data(), (uInt)e.data.size());
        const uint32_t usize = e.declSize >= 0 ? (uint32_t)e.declSize : (uint32_t)e.data.size();
        const uint32_t off = (uint32_t)out.size();
        put32(out, 0x04034b50); put16(out, 20); put16(out, e.flags); put16(out, e.deflate ? 8 : 0);
        put16(out, 0); put16(out, 0x21); put32(out, crc); put32(out, (uint32_t)body.size());
        put32(out, usize); put16(out, (uint16_t)e.name.size()); put16(out, 0);
        out += e.name;
        out += body;
        put32(cd, 0x02014b50); put16(cd, (3 << 8) | 20); put16(cd, 20); put16(cd, e.flags);
        put16(cd, e.deflate ? 8 : 0); put16(cd, 0); put16(cd, 0x21); put32(cd, crc);
        put32(cd, (uint32_t)body.size()); put32(cd, usize); put16(cd, (uint16_t)e.name.size());
        put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, e.mode << 16); put32(cd, off);
        cd += e.name;
    }
    const uint32_t cdOff = (uint32_t)out.size();
    out += cd;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, (uint16_t)entries.size());
    put16(out, (uint16_t)entries.size()); put32(out, (uint32_t)cd.size()); put32(out, cdOff); put16(out, 0);
    return out;
}

void writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f << data;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }

int countEntries(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return -1;
    int n = 0;
    while (dirent* e = readdir(d)) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
    return n;
}

void removeTree(const std::string& p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(p.c_str())) {
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) removeTree(p + "/" + e->d_name);
            closedir(d);
        }
        rmdir(p.c_str());
    } else {
        unlink(p.c_str());
    }
}

class ArchiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp = nanotest::makeTempDir("arc");
        ASSERT_FALSE(tmp.empty());
    }
    void TearDown() override { removeTree(tmp); }
    std::string zip(const std::string& name, const std::vector<ZEntry>& entries) {
        const std::string p = tmp + "/" + name;
        writeFile(p, buildZip(entries));
        return p;
    }
    ExtractResult extractAll(const std::string& arc, const std::string& dest, bool newFolder = true) {
        Progress p;
        return nanoarc::extract(arc, "", dest, newFolder, p);
    }
    std::string tmp;
};

bool hasEntry(const Index& idx, const std::string& path) {
    for (const auto& e : idx.entries) if (e.path == path) return true;
    return false;
}

TEST(ArchiveFormat, ByExtension) {
    EXPECT_EQ(nanoarc::formatFromName("a.zip"), nanoarc::Format::Zip);
    EXPECT_EQ(nanoarc::formatFromName("A.ZIP"), nanoarc::Format::Zip);
    EXPECT_EQ(nanoarc::formatFromName("b.7z"), nanoarc::Format::SevenZip);
    EXPECT_EQ(nanoarc::formatFromName("c.rar"), nanoarc::Format::Rar);
    EXPECT_EQ(nanoarc::formatFromName("d.txt"), nanoarc::Format::None);
}

Format magic(const std::string& head) {
    return nanoarc::formatFromMagic(reinterpret_cast<const uint8_t*>(head.data()), head.size());
}

TEST(ArchiveFormat, ByMagic) {
    EXPECT_EQ(magic(std::string("PK\x03\x04\x14\x00", 6)), Format::Zip);
    EXPECT_EQ(magic(std::string("PK\x05\x06", 4)), Format::Zip);   // an empty zip
    EXPECT_EQ(magic(std::string("7z\xBC\xAF\x27\x1C\x00\x04", 8)), Format::SevenZip);
    EXPECT_EQ(magic(std::string("Rar!\x1A\x07\x00", 7)), Format::Rar);       // RAR 4
    EXPECT_EQ(magic(std::string("Rar!\x1A\x07\x01\x00", 8)), Format::Rar);   // RAR 5
    // Too short, or close but not it.
    EXPECT_EQ(magic(std::string("PK\x03", 3)), Format::None);
    EXPECT_EQ(magic(std::string("7z\xBC\xAF\x27", 5)), Format::None);
    EXPECT_EQ(magic(std::string("Rar!\x1A\x08", 6)), Format::None);
    EXPECT_EQ(magic("%PDF-1.4"), Format::None);
    EXPECT_EQ(magic(""), Format::None);
    EXPECT_EQ(nanoarc::formatFromMagic(nullptr, 0), Format::None);
}

TEST(ArchiveFormat, WhatThisBackEndReads) {
    EXPECT_TRUE(nanoarc::canRead(Format::Zip));
    EXPECT_TRUE(nanoarc::canRead(Format::SevenZip));
    EXPECT_FALSE(nanoarc::canRead(Format::Rar));
    EXPECT_FALSE(nanoarc::canRead(Format::None));
}

TEST_F(ArchiveTest, ZipIndexAndExtract) {
    const std::string a = zip("good.zip", {
        {"top.txt", "hello top\n"},
        {"dir/", "", false, -1, 0x0800, 040755},
        {"dir/a.txt", std::string(10000, 'A')},
        {"implicit/c.txt", "c", false},
    });
    Index idx = nanoarc::readIndex(a);
    ASSERT_EQ(idx.err, Err::None);
    EXPECT_TRUE(hasEntry(idx, "top.txt"));
    EXPECT_TRUE(hasEntry(idx, "dir/a.txt"));
    EXPECT_TRUE(hasEntry(idx, "implicit/c.txt"));

    ExtractResult r = extractAll(a, tmp + "/out");
    ASSERT_EQ(r.err, Err::None);
    EXPECT_EQ(r.extracted, 3);
    EXPECT_EQ(readFile(r.dest + "/top.txt"), "hello top\n");
    EXPECT_EQ(readFile(r.dest + "/dir/a.txt"), std::string(10000, 'A'));
    EXPECT_EQ(readFile(r.dest + "/implicit/c.txt"), "c");

    // The folder name is taken: the next Extract Here gets "out (2)".
    ExtractResult r2 = extractAll(a, tmp + "/out");
    ASSERT_EQ(r2.err, Err::None);
    EXPECT_EQ(r2.dest, tmp + "/out (2)");

    // Into an existing folder, files already there are skipped, not overwritten.
    writeFile(tmp + "/out/top.txt", "mine");
    ExtractResult r3 = extractAll(a, tmp + "/out", /*newFolder=*/false);
    ASSERT_EQ(r3.err, Err::None);
    EXPECT_EQ(r3.existing, 3);
    EXPECT_EQ(readFile(tmp + "/out/top.txt"), "mine");
}

TEST_F(ArchiveTest, ZipSelectedFolderLandsUnderItsName) {
    const std::string a = zip("sel.zip", {{"x/y/f.txt", "f"}, {"x/g.txt", "g"}});
    mkdir((tmp + "/d").c_str(), 0755);
    Progress p;
    ExtractResult r = nanoarc::extract(a, "x/y", tmp + "/d", false, p);
    ASSERT_EQ(r.err, Err::None);
    EXPECT_EQ(readFile(tmp + "/d/y/f.txt"), "f");
    EXPECT_FALSE(exists(tmp + "/d/g.txt"));
}

TEST_F(ArchiveTest, ZipHostileNamesAreSkipped) {
    const std::string a = zip("evil.zip", {
        {"ok.txt", "ok"},
        {"../evil.txt", "evil"},
        {"/abs.txt", "abs"},
        {"x/../../evil2.txt", "evil2"},
        {"link", "/etc/passwd", false, -1, 0x0800, 0120777},
        {"win\\back.txt", "backslash"},
    });
    Index idx = nanoarc::readIndex(a);
    ASSERT_EQ(idx.err, Err::None);
    for (const auto& e : idx.entries) {
        EXPECT_EQ(e.path.find(".."), std::string::npos) << e.path;
        EXPECT_NE(e.path[0], '/') << e.path;
    }
    mkdir((tmp + "/box").c_str(), 0755);
    ExtractResult r = extractAll(a, tmp + "/box/out");
    ASSERT_EQ(r.err, Err::None);
    EXPECT_EQ(readFile(r.dest + "/ok.txt"), "ok");
    EXPECT_GE(r.unsafe, 3);
    EXPECT_EQ(r.links, 1);
    EXPECT_FALSE(exists(tmp + "/box/evil.txt"));
    EXPECT_FALSE(exists(tmp + "/evil.txt"));
    EXPECT_FALSE(exists(tmp + "/evil2.txt"));
    EXPECT_FALSE(exists(tmp + "/abs.txt"));
    EXPECT_FALSE(exists(r.dest + "/link"));
    // A backslash is a folder separator in a DOS-made zip, never a file name character.
    EXPECT_TRUE(exists(r.dest + "/win/back.txt"));
    EXPECT_FALSE(exists(r.dest + "/win\\back.txt"));
}

TEST_F(ArchiveTest, ZipNeverFollowsALinkInTheDestination) {
    const std::string outside = tmp + "/outside";
    mkdir(outside.c_str(), 0755);
    const std::string dest = tmp + "/dest";
    mkdir(dest.c_str(), 0755);
    ASSERT_EQ(symlink(outside.c_str(), (dest + "/sub").c_str()), 0);
    const std::string a = zip("sub.zip", {{"sub/x.txt", "x"}, {"sub/deep/y.txt", "y"}, {"z.txt", "z"}});
    ExtractResult r = extractAll(a, dest, /*newFolder=*/false);
    EXPECT_EQ(countEntries(outside), 0);
    EXPECT_EQ(readFile(dest + "/z.txt"), "z");
    EXPECT_GE(r.unsafe, 2);
}

TEST_F(ArchiveTest, ZipSizeMismatchIsCorruptAndRollsBack) {
    ZEntry bomb{"zz/bomb.bin", std::string(1 << 20, '\0')};
    bomb.declSize = 100;
    const std::string a = zip("bomb.zip", {{"a/ok.txt", "fine"}, bomb});
    ExtractResult r = extractAll(a, tmp + "/out");
    EXPECT_EQ(r.err, Err::Corrupt);
    EXPECT_TRUE(r.rolledBack);
    EXPECT_FALSE(exists(tmp + "/out"));
}

TEST_F(ArchiveTest, ZipEncryptedEntriesAreRefused) {
    ZEntry e{"secret.txt", std::string(100, 's')};
    e.flags |= 1;
    const std::string a = zip("enc.zip", {e});
    ExtractResult r = extractAll(a, tmp + "/out");
    EXPECT_EQ(r.err, Err::Encrypted);
    EXPECT_EQ(r.encrypted, 1);
    EXPECT_FALSE(exists(tmp + "/out"));

    // A mix extracts what it can and counts the rest.
    const std::string m = zip("mix.zip", {e, {"plain.txt", "p"}});
    ExtractResult r2 = extractAll(m, tmp + "/mix");
    EXPECT_EQ(r2.err, Err::None);
    EXPECT_EQ(r2.extracted, 1);
    EXPECT_EQ(r2.encrypted, 1);
}

TEST_F(ArchiveTest, ZipNonUtf8NamesAreReported) {
    ZEntry e{std::string("caf\x82_.txt"), "x"};
    e.flags = 0;   // no UTF-8 flag: a CP437 name
    const std::string a = zip("cp437.zip", {e});
    EXPECT_EQ(nanoarc::readIndex(a).err, Err::NameEncoding);
}

TEST_F(ArchiveTest, ZipJunkAndRar) {
    writeFile(tmp + "/junk.zip", std::string(160, 'j'));
    EXPECT_EQ(nanoarc::readIndex(tmp + "/junk.zip").err, Err::Corrupt);
    writeFile(tmp + "/x.rar", std::string("Rar!\x1a\x07\x00", 7));
    EXPECT_EQ(nanoarc::readIndex(tmp + "/x.rar").err, Err::Rar);
    EXPECT_EQ(nanoarc::readIndex(tmp + "/missing.zip").err, Err::Open);
}

// Cancels the extraction once the first bytes are written.
Progress* gCancelProg = nullptr;
void cancelOnProgress() {
    if (gCancelProg && gCancelProg->done.load() > 0) gCancelProg->cancel.store(true);
}

TEST_F(ArchiveTest, ZipCancelRollsBack) {
    std::vector<ZEntry> many;
    for (int i = 0; i < 20; i++) many.push_back({"d/f" + std::to_string(i) + ".txt", std::string(1000 + i, 'q')});
    const std::string a = zip("many.zip", many);
    Progress p;
    gCancelProg = &p;
    p.notify = cancelOnProgress;
    ExtractResult r = nanoarc::extract(a, "", tmp + "/out", true, p);
    gCancelProg = nullptr;
    EXPECT_EQ(r.err, Err::Cancelled);
    EXPECT_TRUE(r.rolledBack);
    EXPECT_EQ(r.extracted, 0);
    EXPECT_FALSE(exists(tmp + "/out"));
}

TEST_F(ArchiveTest, CancelBeforeStartWritesNothing) {
    const std::string a = zip("one.zip", {{"a.txt", "a"}});
    Progress p;
    p.cancel = true;
    ExtractResult r = nanoarc::extract(a, "", tmp + "/out", true, p);
    EXPECT_EQ(r.err, Err::Cancelled);
    EXPECT_FALSE(exists(tmp + "/out"));
}

TEST_F(ArchiveTest, CompressRoundTrip) {
    struct statfs fs;
    if (statfs(tmp.c_str(), &fs) == 0 && fs.f_type == TMPFS_MAGIC) GTEST_SKIP() << "temp dir is tmpfs";
    const std::string src = tmp + "/src";
    mkdir(src.c_str(), 0755);
    mkdir((src + "/sub").c_str(), 0755);
    mkdir((src + "/empty").c_str(), 0755);
    std::string bin;
    for (int i = 0; i < 70000; i++) bin.push_back((char)(i * 131 + (i >> 7)));
    writeFile(src + "/a.txt", "alpha");
    writeFile(src + "/sub/b.bin", bin);
    writeFile(src + "/ünï 日本.txt", "utf");
    ASSERT_EQ(symlink("/etc/passwd", (src + "/lnk").c_str()), 0);

    Progress p;
    nanoarc::CompressResult c = nanoarc::compressToZip(src, p);
    ASSERT_EQ(c.err, Err::None) << c.detail;
    EXPECT_EQ(c.zipPath, tmp + "/src.zip");
    EXPECT_EQ(c.links, 1);

    // A second compress does not overwrite the first.
    Progress p2;
    nanoarc::CompressResult c2 = nanoarc::compressToZip(src, p2);
    ASSERT_EQ(c2.err, Err::None);
    EXPECT_EQ(c2.zipPath, tmp + "/src (2).zip");

    Index idx = nanoarc::readIndex(c.zipPath);
    ASSERT_EQ(idx.err, Err::None);
    EXPECT_TRUE(hasEntry(idx, "src/a.txt"));
    EXPECT_TRUE(hasEntry(idx, "src/sub/b.bin"));
    EXPECT_FALSE(hasEntry(idx, "src/lnk"));

    const std::string dest = tmp + "/back";
    mkdir(dest.c_str(), 0755);
    ExtractResult r = extractAll(c.zipPath, dest, false);
    ASSERT_EQ(r.err, Err::None);
    EXPECT_EQ(readFile(dest + "/src/a.txt"), "alpha");
    EXPECT_EQ(readFile(dest + "/src/sub/b.bin"), bin);
    EXPECT_EQ(readFile(dest + "/src/ünï 日本.txt"), "utf");
    struct stat st;
    EXPECT_EQ(stat((dest + "/src/empty").c_str(), &st), 0);

    // A single file zips beside itself.
    Progress p3;
    nanoarc::CompressResult c3 = nanoarc::compressToZip(src + "/a.txt", p3);
    ASSERT_EQ(c3.err, Err::None);
    EXPECT_EQ(c3.zipPath, src + "/a.txt.zip");

    Progress p4;
    EXPECT_EQ(nanoarc::compressToZip(src + "/lnk", p4).err, Err::Link);
}

TEST_F(ArchiveTest, SevenZipRoundTrip) {
    Index idx = nanoarc::readIndex(nanotest::fixture("archive/good.7z"));
    ASSERT_EQ(idx.err, Err::None);
    EXPECT_TRUE(hasEntry(idx, "readme.txt"));
    EXPECT_TRUE(hasEntry(idx, "folder/inner.bin"));
    EXPECT_TRUE(hasEntry(idx, "ünï/日本.txt"));
    EXPECT_FALSE(hasEntry(idx, "../up.txt"));

    ExtractResult r = extractAll(nanotest::fixture("archive/good.7z"), tmp + "/out");
    ASSERT_EQ(r.err, Err::None);
    EXPECT_EQ(r.extracted, 4);
    EXPECT_EQ(r.unsafe, 1);
    EXPECT_EQ(r.links, 1);
    std::string readme;
    for (int i = 0; i < 50; i++) readme += "seven zip\n";
    EXPECT_EQ(readFile(r.dest + "/readme.txt"), readme);
    EXPECT_EQ(readFile(r.dest + "/folder/inner.bin").size(), 3000u);
    EXPECT_EQ(readFile(r.dest + "/empty.txt"), "");
    EXPECT_EQ(readFile(r.dest + "/ünï/日本.txt"), "utf");
    EXPECT_FALSE(exists(r.dest + "/lnk"));
    EXPECT_FALSE(exists(tmp + "/up.txt"));

    ExtractResult c = extractAll(nanotest::fixture("archive/copy.7z"), tmp + "/copy");
    ASSERT_EQ(c.err, Err::None);
    EXPECT_EQ(readFile(c.dest + "/plain.txt"), "copy method");
}

TEST_F(ArchiveTest, SevenZipDamaged) {
    ExtractResult r = extractAll(nanotest::fixture("archive/bad.7z"), tmp + "/bad");
    EXPECT_EQ(r.err, Err::Corrupt);
    EXPECT_TRUE(r.rolledBack);
    EXPECT_FALSE(exists(tmp + "/bad"));
    EXPECT_EQ(nanoarc::readIndex(nanotest::fixture("archive/trunc.7z")).err, Err::Corrupt);
}

TEST_F(ArchiveTest, SevenZipEncrypted) {
    ExtractResult r = extractAll(nanotest::fixture("archive/aes.7z"), tmp + "/aes");
    EXPECT_EQ(r.err, Err::Encrypted);
    EXPECT_FALSE(exists(tmp + "/aes"));
}

TEST_F(ArchiveTest, SevenZipOversizedBlockIsRefusedUpFront) {
    ExtractResult r = extractAll(nanotest::fixture("archive/huge.7z"), tmp + "/huge");
    EXPECT_EQ(r.err, Err::BlockTooBig);
    EXPECT_GT(r.needBytes, nanoarc::k7zBlockCap);
    EXPECT_FALSE(exists(tmp + "/huge"));
}

// ---- comic book manuals: one entry read into memory ---------------------------------------------

constexpr uint64_t kMiB = 1 << 20;

std::string bytes(const EntryData& d) { return std::string(d.data.begin(), d.data.end()); }

TEST_F(ArchiveTest, ReadEntryIntoMemory) {
    const std::string a = zip("Game.cbz", {
        {"p10.jpg", "ten"},
        {"p2.jpg", std::string(5000, 'b')},
        {"notes.txt", "n", false},
        {"dir/", "", false, -1, 0x0800, 040755},
    });
    // A .cbz is a zip that its name does not announce: read as one only when told.
    EXPECT_EQ(nanoarc::readIndex(a).err, Err::Open);
    Index idx = nanoarc::readIndex(a, Format::Zip);
    ASSERT_EQ(idx.err, Err::None);
    EXPECT_TRUE(hasEntry(idx, "p10.jpg"));

    EntryData d = nanoarc::readEntry(a, Format::Zip, "p2.jpg", kMiB, kMiB);
    ASSERT_EQ(d.err, Err::None);
    EXPECT_EQ(bytes(d), std::string(5000, 'b'));
    d = nanoarc::readEntry(a, Format::Zip, "p10.jpg", kMiB, kMiB);
    ASSERT_EQ(d.err, Err::None);
    EXPECT_EQ(bytes(d), "ten");

    // Over the cap, nothing is read.
    d = nanoarc::readEntry(a, Format::Zip, "p2.jpg", 4999, kMiB);
    EXPECT_EQ(d.err, Err::TooBig);
    EXPECT_TRUE(d.data.empty());
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "p2.jpg", 5000, kMiB).err, Err::None);

    // No such file, and a folder is not one.
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "missing.jpg", kMiB, kMiB).err, Err::Open);
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "dir", kMiB, kMiB).err, Err::Open);
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "", kMiB, kMiB).err, Err::Open);
    EXPECT_EQ(nanoarc::readEntry(tmp + "/missing.cbz", Format::Zip, "p2.jpg", kMiB, kMiB).err, Err::Open);
}

TEST_F(ArchiveTest, ReadEntryRefusesLinksSecretsAndBombs) {
    ZEntry lnk{"link.jpg", "/etc/passwd", false, -1, 0x0800, 0120777};
    ZEntry secret{"secret.jpg", std::string(100, 's')};
    secret.flags |= 1;
    ZEntry bomb{"bomb.jpg", std::string(1 << 20, '\0')};
    bomb.declSize = 100;
    const std::string a = zip("evil.cbz", {lnk, secret, bomb, {"../up.jpg", "up"}});

    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "link.jpg", kMiB, kMiB).err, Err::Link);
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "secret.jpg", kMiB, kMiB).err, Err::Encrypted);
    // Inflating past its declared size stops at that size.
    EntryData b = nanoarc::readEntry(a, Format::Zip, "bomb.jpg", 1ull << 30, kMiB);
    EXPECT_EQ(b.err, Err::Corrupt);
    EXPECT_TRUE(b.data.empty());
    // An unsafe name is never an entry, by either spelling.
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "up.jpg", kMiB, kMiB).err, Err::Open);
    EXPECT_EQ(nanoarc::readEntry(a, Format::Zip, "../up.jpg", kMiB, kMiB).err, Err::Open);
}

TEST_F(ArchiveTest, ReadEntryFromSevenZip) {
    const std::string a = nanotest::fixture("archive/good.7z");
    EntryData d = nanoarc::readEntry(a, Format::SevenZip, "readme.txt", kMiB, kMiB);
    ASSERT_EQ(d.err, Err::None);
    std::string readme;
    for (int i = 0; i < 50; i++) readme += "seven zip\n";
    EXPECT_EQ(bytes(d), readme);
    d = nanoarc::readEntry(a, Format::SevenZip, "folder/inner.bin", kMiB, kMiB);
    ASSERT_EQ(d.err, Err::None);
    EXPECT_EQ(d.data.size(), 3000u);
    d = nanoarc::readEntry(a, Format::SevenZip, "empty.txt", kMiB, kMiB);
    EXPECT_EQ(d.err, Err::None);
    EXPECT_TRUE(d.data.empty());
    // Its link is listed as one, and never read.
    int links = 0;
    for (const auto& e : nanoarc::readIndex(a, Format::SevenZip).entries) {
        if (!e.isLink) continue;
        links++;
        EXPECT_EQ(nanoarc::readEntry(a, Format::SevenZip, e.path, kMiB, kMiB).err, Err::Link) << e.path;
    }
    EXPECT_EQ(links, 1);
}

// The first file entry readIndex lists, or "".
std::string firstFile(const std::string& archive, Format fmt) {
    for (const auto& e : nanoarc::readIndex(archive, fmt).entries)
        if (!e.isDir) return e.path;
    return std::string();
}

TEST_F(ArchiveTest, ReadEntryFromSevenZipRefusals) {
    const std::string aes = nanotest::fixture("archive/aes.7z");
    const std::string secret = firstFile(aes, Format::SevenZip);
    ASSERT_FALSE(secret.empty());
    EXPECT_EQ(nanoarc::readEntry(aes, Format::SevenZip, secret, kMiB, kMiB).err, Err::Encrypted);

    // A solid block over the caller's cap, or over k7zBlockCap whatever the caller allows, is refused
    // before anything is decoded.
    const std::string huge = nanotest::fixture("archive/huge.7z");
    const std::string big = firstFile(huge, Format::SevenZip);
    ASSERT_FALSE(big.empty());
    EXPECT_EQ(nanoarc::readEntry(huge, Format::SevenZip, big, UINT64_MAX, kMiB).err, Err::BlockTooBig);
    EXPECT_EQ(nanoarc::readEntry(huge, Format::SevenZip, big, UINT64_MAX, UINT64_MAX).err, Err::BlockTooBig);
}

TEST_F(ArchiveTest, ComicRarIsRefusedAsUnsupported) {
    const std::string cbr = tmp + "/Game.cbr";
    writeFile(cbr, std::string("Rar!\x1a\x07\x00", 7));
    ASSERT_EQ(nanomanual::comicFormat(cbr), Format::Rar);
    const Index idx = nanoarc::readIndex(cbr, nanomanual::comicFormat(cbr));
    EXPECT_EQ(idx.err, Err::Rar);
    EXPECT_TRUE(nanomanual::unsupportedFormat(idx.err));
    EXPECT_EQ(nanoarc::readEntry(cbr, Format::Rar, "p1.jpg", kMiB, kMiB).err, Err::Rar);
}

TEST_F(ArchiveTest, FormatFromContent) {
    EXPECT_EQ(nanoarc::formatFromContent(zip("Game.cbr", {{"p1.jpg", "1"}})), Format::Zip);
    EXPECT_EQ(nanoarc::formatFromContent(zip("empty.cbz", {})), Format::Zip);
    EXPECT_EQ(nanoarc::formatFromContent(nanotest::fixture("archive/good.7z")), Format::SevenZip);
    writeFile(tmp + "/rar4.cbz", std::string("Rar!\x1a\x07\x00", 7));
    EXPECT_EQ(nanoarc::formatFromContent(tmp + "/rar4.cbz"), Format::Rar);
    writeFile(tmp + "/rar5.cbz", std::string("Rar!\x1a\x07\x01\x00", 8));
    EXPECT_EQ(nanoarc::formatFromContent(tmp + "/rar5.cbz"), Format::Rar);
    writeFile(tmp + "/short.cbz", "PK");
    EXPECT_EQ(nanoarc::formatFromContent(tmp + "/short.cbz"), Format::None);
    writeFile(tmp + "/text.cbz", "not an archive at all");
    EXPECT_EQ(nanoarc::formatFromContent(tmp + "/text.cbz"), Format::None);
    EXPECT_EQ(nanoarc::formatFromContent(tmp + "/missing.cbz"), Format::None);
    EXPECT_EQ(nanoarc::formatFromContent(tmp), Format::None);   // a folder
}

TEST_F(ArchiveTest, OpenArchiveReadsEntriesWithoutReopening) {
    const std::string a = zip("Game.cbz", {
        {"p1.jpg", "one"},
        {"p2.jpg", std::string(5000, 'b')},
        {"dir/", "", false, -1, 0x0800, 040755},
    });
    Index idx;
    const nanoarc::ArchivePtr arc(nanoarc::openArchive(a, Format::Zip, &idx));
    ASSERT_TRUE(arc);
    ASSERT_EQ(idx.err, Err::None);
    EXPECT_TRUE(hasEntry(idx, "p1.jpg"));
    EXPECT_TRUE(hasEntry(idx, "p2.jpg"));
    // The same archive answers every read, in any order and more than once.
    for (int round = 0; round < 2; round++) {
        EntryData d = nanoarc::readEntry(arc.get(), "p2.jpg", kMiB, kMiB);
        ASSERT_EQ(d.err, Err::None);
        EXPECT_EQ(bytes(d), std::string(5000, 'b'));
        d = nanoarc::readEntry(arc.get(), "p1.jpg", kMiB, kMiB);
        ASSERT_EQ(d.err, Err::None);
        EXPECT_EQ(bytes(d), "one");
    }
    // The rules hold as for a one-off read: the cap, missing files, folders.
    EXPECT_EQ(nanoarc::readEntry(arc.get(), "p2.jpg", 4999, kMiB).err, Err::TooBig);
    EXPECT_EQ(nanoarc::readEntry(arc.get(), "missing.jpg", kMiB, kMiB).err, Err::Open);
    EXPECT_EQ(nanoarc::readEntry(arc.get(), "dir", kMiB, kMiB).err, Err::Open);
    // A failed read leaves the archive usable.
    EXPECT_EQ(bytes(nanoarc::readEntry(arc.get(), "p1.jpg", kMiB, kMiB)), "one");
    // No archive: nothing to read.
    EXPECT_EQ(nanoarc::readEntry(nullptr, "p1.jpg", kMiB, kMiB).err, Err::Open);
    nanoarc::closeArchive(nullptr);
}

TEST_F(ArchiveTest, OpenArchiveFailures) {
    Index idx;
    idx.entries.push_back(nanoarc::Entry());   // cleared by every open
    EXPECT_EQ(nanoarc::openArchive(tmp + "/missing.cbz", Format::Zip, &idx), nullptr);
    EXPECT_EQ(idx.err, Err::Open);
    EXPECT_TRUE(idx.entries.empty());
    const std::string cbr = tmp + "/Game.cbr";
    writeFile(cbr, std::string("Rar!\x1a\x07\x00", 7));
    EXPECT_EQ(nanoarc::openArchive(cbr, Format::Rar, &idx), nullptr);
    EXPECT_EQ(idx.err, Err::Rar);
    writeFile(tmp + "/junk.cbz", "not an archive at all");
    EXPECT_EQ(nanoarc::openArchive(tmp + "/junk.cbz", Format::Zip, &idx), nullptr);
    EXPECT_EQ(idx.err, Err::Corrupt);
    EXPECT_EQ(nanoarc::openArchive(tmp + "/junk.cbz", Format::None, &idx), nullptr);
    EXPECT_EQ(idx.err, Err::Open);
}

TEST_F(ArchiveTest, OpenSevenZipKeepsItsBlockBetweenReads) {
    const std::string a = nanotest::fixture("archive/good.7z");
    Index idx;
    const nanoarc::ArchivePtr arc(nanoarc::openArchive(a, Format::SevenZip, &idx));
    ASSERT_TRUE(arc);
    std::string readme;
    for (int i = 0; i < 50; i++) readme += "seven zip\n";
    // Back and forth, as page turns go: each read is whole and right whether or not its block is
    // the one already decoded.
    for (int round = 0; round < 2; round++) {
        EntryData d = nanoarc::readEntry(arc.get(), "readme.txt", kMiB, kMiB);
        ASSERT_EQ(d.err, Err::None);
        EXPECT_EQ(bytes(d), readme);
        d = nanoarc::readEntry(arc.get(), "folder/inner.bin", kMiB, kMiB);
        ASSERT_EQ(d.err, Err::None);
        EXPECT_EQ(d.data.size(), 3000u);
        d = nanoarc::readEntry(arc.get(), "empty.txt", kMiB, kMiB);
        EXPECT_EQ(d.err, Err::None);
        EXPECT_TRUE(d.data.empty());
    }
}

// A comic book manual's format as the viewer picks it: the content first, then the extension.
Format comicFormatOf(const std::string& path) {
    return nanomanual::comicFormat(path, nanoarc::formatFromContent(path));
}

TEST_F(ArchiveTest, ComicCbrThatIsReallyAZipOpens) {
    const std::string a = zip("Game.cbr", {{"p2.jpg", "2"}, {"p1.jpg", "1"}});
    ASSERT_EQ(nanomanual::comicFormat(a), Format::Rar);   // its name says RAR
    const Format fmt = comicFormatOf(a);
    ASSERT_EQ(fmt, Format::Zip);
    Index idx;
    const nanoarc::ArchivePtr arc(nanoarc::openArchive(a, fmt, &idx));
    ASSERT_TRUE(arc);
    const std::vector<std::string> pages = nanomanual::comicPages(idx.entries);
    ASSERT_EQ(pages, (std::vector<std::string>{"p1.jpg", "p2.jpg"}));
    EXPECT_EQ(bytes(nanoarc::readEntry(arc.get(), pages[0], kMiB, kMiB)), "1");
}

TEST_F(ArchiveTest, ComicCbzThatIsReallyARarIsUnsupported) {
    const std::string cbz = tmp + "/Game.cbz";
    writeFile(cbz, std::string("Rar!\x1a\x07\x01\x00", 8));
    const Format fmt = comicFormatOf(cbz);
    ASSERT_EQ(fmt, Format::Rar);
    EXPECT_FALSE(nanoarc::canRead(fmt));
    Index idx;
    EXPECT_EQ(nanoarc::openArchive(cbz, fmt, &idx), nullptr);
    EXPECT_EQ(idx.err, Err::Rar);
    EXPECT_TRUE(nanomanual::unsupportedFormat(idx.err));   // UNSUPPORTED MANUAL FORMAT
    // Damaged content that announces nothing is still read by its extension, and says so.
    writeFile(cbz, "not an archive at all");
    ASSERT_EQ(comicFormatOf(cbz), Format::Zip);
    EXPECT_EQ(nanoarc::openArchive(cbz, Format::Zip, &idx), nullptr);
    EXPECT_FALSE(nanomanual::unsupportedFormat(idx.err));   // THIS MANUAL CAN'T BE OPENED
}

TEST_F(ArchiveTest, ComicManualPagesThroughNanoarc) {
    const std::string a = zip("Game.cbz", {
        {"Game/p10.jpg", "10"},
        {"Game/p2.jpg", "2"},
        {"Game/P1.jpg", "1"},
        {"Game/ComicInfo.xml", "<ComicInfo/>"},
        {"__MACOSX/Game/._P1.jpg", "fork"},
        {"../p0.jpg", "evil"},
        {"Game/link.jpg", "/etc/passwd", false, -1, 0x0800, 0120777},
    });
    const Format fmt = comicFormatOf(a);
    ASSERT_EQ(fmt, Format::Zip);
    Index idx;
    const nanoarc::ArchivePtr arc(nanoarc::openArchive(a, fmt, &idx));
    ASSERT_TRUE(arc);
    ASSERT_EQ(idx.err, Err::None);
    const std::vector<std::string> pages = nanomanual::comicPages(idx.entries);
    ASSERT_EQ(pages, (std::vector<std::string>{"Game/P1.jpg", "Game/p2.jpg", "Game/p10.jpg"}));
    const char* const want[] = {"1", "2", "10"};
    for (size_t i = 0; i < pages.size(); i++) {
        const EntryData d =
            nanoarc::readEntry(arc.get(), pages[i], nanomanual::kMaxPageBytes, nanomanual::kMaxBlockBytes);
        ASSERT_EQ(d.err, Err::None) << pages[i];
        EXPECT_EQ(bytes(d), want[i]);
    }
}

TEST_F(ArchiveTest, ComicManualPagesFromSevenZip) {
    // good.7z holds no images: a comic manual made like it has no pages.
    const std::string a = nanotest::fixture("archive/good.7z");
    const Index idx = nanoarc::readIndex(a, Format::SevenZip);
    ASSERT_EQ(idx.err, Err::None);
    EXPECT_TRUE(nanomanual::comicPages(idx.entries).empty());
}

}  // namespace
