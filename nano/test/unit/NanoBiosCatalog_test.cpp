#include <gtest/gtest.h>

#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <set>

#include "NanoBiosCatalog.h"
#include "nano_test_fs.h"
#include "nano_test_zip.h"

using namespace android::nanobios;
using nanotest::TempDir;

namespace {

bool isHex(const char* s, size_t n) {
  if (strlen(s) != n) return false;
  for (; *s; s++)
    if (!strchr("0123456789abcdef", *s)) return false;
  return true;
}

std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

}  // namespace

// ---- the catalogue

TEST(BiosCatalog, RowsAreUniqueAndComplete) {
  std::set<std::string> ids;
  for (const auto& r : rows()) {
    EXPECT_TRUE(ids.insert(r.id).second) << "duplicate row " << r.id;
    EXPECT_GE(r.section, SEC_REQUIRED); EXPECT_LE(r.section, SEC_OPTIONAL);
    EXPECT_TRUE(r.system && *r.system) << r.id;
    EXPECT_TRUE(r.emulators && r.qualifier && r.note) << r.id;
    EXPECT_EQ(findRow(r.id), &r);
  }
  EXPECT_EQ(findRow("no-such-row"), nullptr);
}

TEST(BiosCatalog, FilesAreConsistent) {
  for (const auto& f : files()) {
    SCOPED_TRACE(f.name);
    EXPECT_NE(findRow(f.row), nullptr) << f.row;
    EXPECT_TRUE(!*f.sha1 || isHex(f.sha1, 40)) << f.sha1;
    EXPECT_TRUE(!*f.md5 || isHex(f.md5, 32)) << f.md5;
    if (f.match == MATCH_SHA1) EXPECT_TRUE(*f.sha1);
    if (f.match == MATCH_MD5) EXPECT_TRUE(*f.md5);
    if (f.match == MATCH_ZIPSET) EXPECT_NE(findZipSet(f.name), nullptr);
    if (f.sizeMax) EXPECT_GE(f.sizeMax, f.size);
    EXPECT_FALSE(sizeText(f).empty());
    for (const auto& s : f.slots) {
      EXPECT_GE(s.kind, 0); EXPECT_LT(s.kind, SLOT_KIND_COUNT);
      ASSERT_NE(s.path, nullptr);
      EXPECT_EQ(strstr(s.path, ".."), nullptr) << s.path;   // a slot never writes outside its base
      EXPECT_NE(s.path[0], '/') << s.path;
    }
  }
}

TEST(BiosCatalog, ZipSetsAreConsistent) {
  for (const auto& z : zipSets()) {
    SCOPED_TRACE(z.name);
    EXPECT_EQ(findZipSet(z.name), &z);
    for (const auto& m : z.members) {
      for (const char* p = m.name; *p; p++) EXPECT_FALSE(*p >= 'A' && *p <= 'Z') << m.name;   // compared lower-case
      EXPECT_TRUE(!*m.sha1 || isHex(m.sha1, 40)) << m.name;
    }
    for (const auto& group : z.need)
      for (const char* n : group) {
        const size_t len = strlen(n);
        if (len && n[len - 1] == '*') continue;
        bool member = false;
        for (const auto& m : z.members) member |= !strcmp(m.name, n);
        EXPECT_TRUE(member) << n;
      }
  }
  EXPECT_EQ(findZipSet("nope.zip"), nullptr);
}

// A zip holding exactly a set's members (a non-merged set) is taken for that set and no earlier one:
// a parent's set comes before its clone's and needs a ROM the clone's zip lacks.
TEST(BiosCatalog, EachZipSetRecognisesItsOwnZip) {
  for (const auto& z : zipSets()) {
    SCOPED_TRACE(z.name);
    auto inZip = [&z](const ZipMember& m) {
      for (const auto& o : z.members)
        if (!strcmp(o.name, m.name) && o.size == m.size && o.crc == m.crc) return true;
      return false;
    };
    const ZipSet* first = nullptr;
    for (const auto& t : zipSets()) {
      bool usable = !t.need.empty();
      for (const auto& group : t.need) {
        bool any = false;
        for (const char* n : group) {
          const size_t len = strlen(n);
          for (const auto& m : (len && n[len - 1] == '*') ? z.members : t.members) {
            if (len && n[len - 1] == '*') any |= !strncmp(m.name, n, len - 1);
            else any |= !strcmp(m.name, n) && inZip(m);
          }
        }
        usable &= any;
      }
      if (usable) { first = &t; break; }
    }
    EXPECT_EQ(first, &z) << (first ? first->name : "none");
  }
}

// MAME4droid 2024 (not shipped): MAME's own zips, installed whole into its roms folder, which the launch
// commands put in -rompath.
TEST(BiosCatalog, Mame4droidSets) {
  EXPECT_STREQ(slotPackage(SLOT_MAME4DROID), "com.seleuco.mame4d2024");
  EXPECT_STREQ(slotAppName(SLOT_MAME4DROID), "MAME4droid 2024");
  std::set<std::string> zips;
  for (const auto& r : rows()) {
    if (strncmp(r.id, "m4d_", 4)) continue;
    SCOPED_TRACE(r.id);
    EXPECT_STREQ(r.emulators, "MAME4droid 2024");
    EXPECT_EQ(r.section, SEC_REQUIRED);
    int n = 0;
    for (const auto& f : files()) {
      if (strcmp(f.row, r.id)) continue;
      n++;
      SCOPED_TRACE(f.name);
      EXPECT_EQ(f.match, MATCH_ZIPSET);
      ASSERT_FALSE(f.slots.empty());
      for (const auto& s : f.slots) EXPECT_EQ(s.kind, SLOT_MAME4DROID);
      const ZipSet* z = findZipSet(f.name);
      ASSERT_NE(z, nullptr);
      EXPECT_FALSE(z->need.empty());
      for (const auto& m : z->members) EXPECT_TRUE(isHex(m.sha1, 40)) << m.name;   // every MAME ROM has one
      zips.insert(f.name);
    }
    EXPECT_GT(n, 0);
  }
  // The machines Nano's MAME4droid players start, and the default devices among them that load ROMs.
  for (const char* z : {"apple2gs.zip", "ti99_4a.zip", "ti99_speech.zip", "coco.zip", "coco3.zip", "coco_fdc.zip",
                        "dragon32.zip", "dragon64.zip", "dragon_fdc.zip", "fmtownsux.zip", "fmtmarty.zip", "adam.zip",
                        "adam_kb.zip", "adam_prn.zip", "adam_ddp.zip", "adam_fdc.zip", "aa310.zip", "aa3000.zip",
                        "electron64.zip", "electron_plus3.zip", "fm7.zip", "fm77av.zip", "oric1.zip", "orica.zip",
                        "samcoupe.zip", "trs80l2.zip", "atom.zip", "atom_discpack.zip", "astrocde.zip", "gamate.zip",
                        "gamecom.zip", "vsmile.zip", "supracan.zip", "gmaster.zip", "studio2.zip", "advision.zip",
                        "apfm1000.zip"})
    EXPECT_TRUE(zips.count(z)) << z;
  // The A440/1 has the A3000's ROMs.
  bool a4401 = false;
  for (const auto& f : files())
    if (!strcmp(f.name, "aa3000.zip"))
      for (const auto& s : f.slots) a4401 |= !strcmp(s.path, "aa4401.zip");
  EXPECT_TRUE(a4401);
}

TEST(BiosCatalog, SlotKindsHaveNames) {
  for (int k = 0; k < SLOT_KIND_COUNT; k++) {
    EXPECT_NE(slotPackage(k), nullptr) << k;
    EXPECT_NE(slotAppName(k), nullptr) << k;
    EXPECT_TRUE(slotWhere(k) && *slotWhere(k)) << k;
    EXPECT_NE(slotWhereMoved(k), nullptr) << k;
  }
}

TEST(BiosCatalog, AllGroupsCoverEveryRowSorted) {
  const auto groups = allGroups();
  size_t nRows = 0;
  for (size_t i = 0; i < groups.size(); i++) {
    nRows += groups[i].rows.size();
    if (i) EXPECT_LE(strcasecmp(groups[i - 1].system.c_str(), groups[i].system.c_str()), 0);
  }
  EXPECT_EQ(nRows, rows().size());
  EXPECT_TRUE(*allFilesIntro());
  EXPECT_TRUE(*needNothingLine());
}

// hakuX reads console/mcpx.bin, bios.bin and eeprom.bin; dumps usually keep xemu's names.
TEST(BiosCatalog, XboxMcpxIsMatchedByMd5UnderAnyName) {
  int n = 0;
  for (const auto& f : files()) {
    if (strcmp(f.row, "xbox")) continue;
    EXPECT_FALSE(nameMatches(f, "mcpx.bin", 512)) << f.name;
    EXPECT_FALSE(nameMatches(f, "mcpx_1.0.bin", 512)) << f.name;
    if (f.match != MATCH_MD5) continue;
    n++;
    EXPECT_STREQ("d49c52a4102f6df7bcf8d0617ac475ed", f.md5);
    EXPECT_STREQ("", f.sha1);
    EXPECT_EQ(512u, f.size);
    EXPECT_STREQ("mcpx_1.0.bin", f.name);
    ASSERT_EQ(1u, f.slots.size());
    EXPECT_EQ(SLOT_HAKUX, f.slots[0].kind);
    EXPECT_STREQ("mcpx.bin", f.slots[0].path);
    EXPECT_TRUE(f.slots[0].flags & SLOTF_IF_ABSENT);
  }
  EXPECT_EQ(1, n);
}

TEST(BiosCatalog, XboxBiosIsRecognisedByItsUsualNames) {
  auto target = [](const std::string& name, uint64_t size) -> std::string {
    for (const auto& f : files())
      if (nameMatches(f, name, size)) {
        if (strcmp(f.row, "xbox") || f.slots.size() != 1 || f.slots[0].kind != SLOT_HAKUX) return "<other>";
        return f.slots[0].path;
      }
    return "";
  };
  for (const char* name : {"Complex_4627.bin", "complex_4627.BIN", "bios.bin", "BIOS.BIN", "flash.bin"}) {
    SCOPED_TRACE(name);
    EXPECT_EQ("bios.bin", target(name, 262144));
    EXPECT_EQ("bios.bin", target(name, 1048576));
    EXPECT_EQ("", target(name, 524288));
    EXPECT_EQ("", target(name, 262143));
  }
  EXPECT_EQ("eeprom.bin", target("eeprom.bin", 256));
  EXPECT_EQ("", target("Complex_4627.rom", 262144));
}

TEST(BiosCatalog, NameMatches) {
  File f{};
  f.match = MATCH_NAME; f.name = "movable.sed"; f.size = 288; f.sizeMax = 320;
  EXPECT_TRUE(nameMatches(f, "MOVABLE.sed", 300));
  EXPECT_FALSE(nameMatches(f, "movable.sed", 321));
  EXPECT_FALSE(nameMatches(f, "movable.sed2", 300));
  f.size = 0; f.sizeMax = 0;
  EXPECT_TRUE(nameMatches(f, "movable.sed", 12345));
  f.match = MATCH_SHA1;
  EXPECT_FALSE(nameMatches(f, "movable.sed", 12345));
}

TEST(BiosCatalog, SizeText) {
  File f{};
  f.match = MATCH_NAME; f.size = 512 * 1024;
  EXPECT_EQ(sizeText(f), "512 KB");
  f.size = 933636;
  EXPECT_EQ(sizeText(f), "933636 bytes");
  f.size = 256 * 1024; f.sizeMax = 4u << 20;
  EXPECT_EQ(sizeText(f), "256 KB to 4 MB");
  f.size = 0; f.sizeMax = 0;
  EXPECT_EQ(sizeText(f), "any size");
  f.match = MATCH_ZIPSET;
  EXPECT_EQ(sizeText(f), "zip");
}

TEST(BiosCatalog, RegionForLocale) {
  EXPECT_EQ(regionForLocale("en", "US"), REGION_USA);
  EXPECT_EQ(regionForLocale("en", "GB"), REGION_EUR);
  EXPECT_EQ(regionForLocale("en", ""), REGION_USA);
  EXPECT_EQ(regionForLocale("en", "jp"), REGION_JPN);   // region in any case
  EXPECT_EQ(regionForLocale("ja", ""), REGION_JPN);
  EXPECT_EQ(regionForLocale("de", ""), REGION_EUR);
  EXPECT_EQ(regionForLocale("FR", ""), REGION_EUR);
  EXPECT_EQ(regionForLocale("fr", "CA"), REGION_USA);   // the region wins over the language
  EXPECT_EQ(regionForLocale("pt", "BR"), REGION_USA);
  EXPECT_EQ(regionForLocale("es", "419"), REGION_USA);
  EXPECT_EQ(regionForLocale("en", "AU"), REGION_EUR);
  EXPECT_EQ(regionForLocale("zh", "TW"), REGION_USA);
  EXPECT_EQ(regionForLocale("", ""), REGION_USA);
}

TEST(BiosCatalog, Ps2RomverLabel) {
  int region = 99;
  EXPECT_EQ(ps2RomverLabel("0230AC20080220", &region), "USA v2.30");
  EXPECT_EQ(region, REGION_USA);
  EXPECT_EQ(ps2RomverLabel("0160EC20010704", &region), "Europe v1.60");
  EXPECT_EQ(region, REGION_EUR);
  EXPECT_EQ(ps2RomverLabel("0100JC20000117", &region), "Japan v1.00");
  EXPECT_EQ(region, REGION_JPN);
  EXPECT_EQ(ps2RomverLabel("0250HC20100408", &region), "Asia v2.50");
  EXPECT_EQ(region, -1);
  EXPECT_EQ(ps2RomverLabel("", &region), "");
  EXPECT_EQ(ps2RomverLabel("0230A", nullptr), "");
}

// ---- matchers

TEST(BiosMatchers, HashFile) {
  TempDir d("bios");
  ASSERT_TRUE(nanotest::writeFile(d / "abc", "abc", 3));
  std::string sha1, md5;
  ASSERT_TRUE(hashFile(d / "abc", &sha1, &md5));
  EXPECT_EQ(sha1, "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(md5, "900150983cd24fb0d6963f7d28e17f72");
  ASSERT_TRUE(nanotest::writeFile(d / "empty", "", 0));
  ASSERT_TRUE(hashFile(d / "empty", &sha1, nullptr));
  EXPECT_EQ(sha1, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(sha1Hex("abc", 3), "a9993e364706816aba3e25717850c26c9cd0d89d");
  // Larger than the 64 KiB read buffer.
  std::vector<uint8_t> big(200000, 'a');
  ASSERT_TRUE(nanotest::writeFile(d / "big", big));
  ASSERT_TRUE(hashFile(d / "big", &sha1, nullptr));
  EXPECT_EQ(sha1, sha1Hex(big.data(), big.size()));
  EXPECT_FALSE(hashFile(d / "missing", &sha1, &md5));
  EXPECT_FALSE(hashFile(d.path(), &sha1, &md5));
  ASSERT_EQ(symlink((d / "abc").c_str(), (d / "link").c_str()), 0);
  EXPECT_TRUE(hashFile(d / "link", &sha1, nullptr));
  EXPECT_FALSE(hashFile(d / "link", &sha1, nullptr, /*noFollow=*/true));
}

TEST(BiosMatchers, OpenRegular) {
  TempDir d("bios");
  ASSERT_TRUE(nanotest::writeFile(d / "f", "x", 1));
  int fd = openRegular(d / "f", true);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(fcntl(fd, F_GETFL) & O_NONBLOCK, 0);   // cleared once open
  close(fd);
  EXPECT_LT(openRegular(d.path(), false), 0);
  ASSERT_EQ(symlink((d / "f").c_str(), (d / "l").c_str()), 0);
  fd = openRegular(d / "l", false);
  EXPECT_GE(fd, 0);
  if (fd >= 0) close(fd);
  EXPECT_LT(openRegular(d / "l", true), 0);
  // A FIFO with no writer would block a plain open forever.
  ASSERT_EQ(mkfifo((d / "fifo").c_str(), 0600), 0);
  EXPECT_LT(openRegular(d / "fifo", false), 0);
  EXPECT_LT(openRegular(d / "missing", false), 0);
}

TEST(BiosMatchers, ZipDirectory) {
  TempDir d("bios");
  nanotest::ZipMember a; a.name = "a.bin"; a.data = bytes("hello hello hello hello");
  nanotest::ZipMember b; b.name = "sub/b.bin"; b.data = bytes("world"); b.method = 8;
  ASSERT_TRUE(nanotest::writeFile(d / "t.zip", nanotest::makeZip({a, b})));
  std::vector<ZipEntry> dir;
  ASSERT_TRUE(zipDirectory(d / "t.zip", &dir));
  ASSERT_EQ(dir.size(), 2u);
  EXPECT_EQ(dir[0].name, "a.bin");
  EXPECT_EQ(dir[0].method, 0);
  EXPECT_EQ(dir[0].usize, a.data.size());
  EXPECT_EQ(dir[0].crc, (uint32_t)crc32(0, a.data.data(), (uInt)a.data.size()));
  EXPECT_EQ(dir[1].name, "sub/b.bin");
  EXPECT_EQ(dir[1].method, 8);
  EXPECT_EQ(dir[1].usize, 5u);

  // A trailing comment between the directory and the end of the file.
  auto withComment = nanotest::makeZip({a});
  nanotest::put16(withComment, withComment.size() - 2, 5);
  for (char c : std::string("note!")) withComment.push_back((uint8_t)c);
  ASSERT_TRUE(nanotest::writeFile(d / "c.zip", withComment));
  EXPECT_TRUE(zipDirectory(d / "c.zip", &dir));
  EXPECT_EQ(dir.size(), 1u);

  ASSERT_TRUE(nanotest::writeFile(d / "empty.zip", nanotest::makeZip({})));
  EXPECT_TRUE(zipDirectory(d / "empty.zip", &dir));
  EXPECT_TRUE(dir.empty());

  ASSERT_TRUE(nanotest::writeFile(d / "not.zip", bytes(std::string(100, 'x'))));
  EXPECT_FALSE(zipDirectory(d / "not.zip", &dir));
  auto z = nanotest::makeZip({a, b});
  auto badCd = z;   // the directory offset points past the end
  nanotest::put32(badCd, badCd.size() - 6, 0x7fffffff);
  ASSERT_TRUE(nanotest::writeFile(d / "badcd.zip", badCd));
  EXPECT_FALSE(zipDirectory(d / "badcd.zip", &dir));
  auto badCount = z;   // claims more entries than the directory holds
  nanotest::put16(badCount, badCount.size() - 12, 3);
  ASSERT_TRUE(nanotest::writeFile(d / "badcount.zip", badCount));
  EXPECT_FALSE(zipDirectory(d / "badcount.zip", &dir));
  for (size_t cut = 0; cut < z.size(); cut += 7) {
    std::vector<uint8_t> t(z.begin(), z.begin() + cut);
    ASSERT_TRUE(nanotest::writeFile(d / "cut.zip", t));
    EXPECT_FALSE(zipDirectory(d / "cut.zip", &dir)) << cut;
  }
  ASSERT_EQ(symlink((d / "t.zip").c_str(), (d / "l.zip").c_str()), 0);
  EXPECT_FALSE(zipDirectory(d / "l.zip", &dir));   // the scan resolves links itself
}

TEST(BiosMatchers, ZipMemberSha1) {
  TempDir d("bios");
  std::vector<uint8_t> payload(100000);
  for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i * 31 / 7);
  nanotest::ZipMember s; s.name = "stored.bin"; s.data = payload;
  nanotest::ZipMember f; f.name = "deflated.bin"; f.data = payload; f.method = 8;
  nanotest::ZipMember e; e.name = "empty.bin"; e.method = 8;
  ASSERT_TRUE(nanotest::writeFile(d / "t.zip", nanotest::makeZip({s, f, e})));
  std::vector<ZipEntry> dir;
  ASSERT_TRUE(zipDirectory(d / "t.zip", &dir));
  ASSERT_EQ(dir.size(), 3u);
  const std::string want = sha1Hex(payload.data(), payload.size());
  EXPECT_EQ(zipMemberSha1(d / "t.zip", dir[0]), want);
  EXPECT_EQ(zipMemberSha1(d / "t.zip", dir[1]), want);
  EXPECT_EQ(zipMemberSha1(d / "t.zip", dir[2]), sha1Hex("", 0));

  ZipEntry lie = dir[1];
  lie.usize += 1;                                    // inflates short of the size claimed
  EXPECT_EQ(zipMemberSha1(d / "t.zip", lie), "");
  lie = dir[0];
  lie.usize -= 1;                                    // stored with csize != usize
  EXPECT_EQ(zipMemberSha1(d / "t.zip", lie), "");
  lie = dir[0];
  lie.method = 12;
  EXPECT_EQ(zipMemberSha1(d / "t.zip", lie), "");
  lie = dir[0];
  lie.usize = lie.csize = 65u * 1024 * 1024;         // over the 64 MB member cap
  EXPECT_EQ(zipMemberSha1(d / "t.zip", lie), "");
  lie = dir[0];
  lie.localOffset = 1;                               // no local header there
  EXPECT_EQ(zipMemberSha1(d / "t.zip", lie), "");
  lie = dir[1];
  lie.csize = 1u << 30;                              // runs past the end of the file
  EXPECT_EQ(zipMemberSha1(d / "t.zip", lie), "");
}

// A zip whose directory names, sizes and CRCs match a known set is still not that set unless the
// members' contents hash right.
TEST(BiosMatchers, ZipMatchSetChecksContents) {
  TempDir d("bios");
  const ZipSet* q = findZipSet("qsound.zip");
  ASSERT_NE(q, nullptr);
  ASSERT_FALSE(q->members.empty());
  const ZipMember& m = q->members[0];
  nanotest::ZipMember fake;
  fake.name = m.name;
  fake.data.assign(m.size, 0);
  fake.fakeCrc = true;
  fake.crc = m.crc;
  ASSERT_TRUE(nanotest::writeFile(d / "q.zip", nanotest::makeZip({fake})));
  std::vector<std::string> found;
  EXPECT_EQ(zipMatchSet(d / "q.zip", &found), nullptr);
  EXPECT_TRUE(found.empty());
  EXPECT_EQ(zipMatchSet(d / "missing.zip", &found), nullptr);
}

namespace {

// A 4 MiB PS2 image: RESET (0x100 bytes), then the ROMDIR itself, EXTINFO and ROMVER.
std::vector<uint8_t> makePs2(const char* romver, size_t size = 4u << 20) {
  std::vector<uint8_t> rom(size, 0);
  auto entry = [&](size_t at, const char* name, uint32_t len) {
    memcpy(&rom[at], name, strlen(name));
    nanotest::put32(rom, at + 12, len);
  };
  entry(0x100, "RESET", 0x100);
  entry(0x110, "ROMDIR", 0x50);
  entry(0x120, "EXTINFO", 0x10);
  entry(0x130, "ROMVER", 0x10);
  // RESET at 0, ROMDIR at 0x100 (0x50 -> 0x150 aligned), EXTINFO at 0x150, ROMVER at 0x160.
  memcpy(&rom[0x160], romver, strlen(romver));
  return rom;
}

}  // namespace

TEST(BiosMatchers, Ps2Romver) {
  TempDir d("bios");
  ASSERT_TRUE(nanotest::writeFile(d / "ok.bin", makePs2("0230AC20080220\n")));
  EXPECT_EQ(ps2Romver(d / "ok.bin"), "0230AC20080220");
  ASSERT_TRUE(nanotest::writeFile(d / "bad.bin", makePs2("02X0AC20080220")));
  EXPECT_EQ(ps2Romver(d / "bad.bin"), "");
  ASSERT_TRUE(nanotest::writeFile(d / "nozone.bin", makePs2("02301C20080220")));
  EXPECT_EQ(ps2Romver(d / "nozone.bin"), "");
  ASSERT_TRUE(nanotest::writeFile(d / "short.bin", makePs2("0230AC20080220", (4u << 20) - 1)));
  EXPECT_EQ(ps2Romver(d / "short.bin"), "");         // not a 4 MiB image
  ASSERT_TRUE(nanotest::writeFile(d / "zero.bin", std::vector<uint8_t>(4u << 20, 0)));
  EXPECT_EQ(ps2Romver(d / "zero.bin"), "");          // no ROMDIR
  auto huge = makePs2("0230AC20080220");
  nanotest::put32(huge, 0x12C, 0xFFFFFFF0u);         // EXTINFO's size pushes ROMVER off the image
  ASSERT_TRUE(nanotest::writeFile(d / "huge.bin", huge));
  EXPECT_EQ(ps2Romver(d / "huge.bin"), "");
}

TEST(BiosMatchers, IsVitaPup) {
  TempDir d("bios");
  std::vector<uint8_t> h(0x40, 0);
  memcpy(h.data(), "SCEUF", 5);
  nanotest::put32(h, 0x10, 0x03650000);
  ASSERT_TRUE(nanotest::writeFile(d / "PSVUPDAT.PUP", h));
  uint32_t fw = 0;
  EXPECT_TRUE(isVitaPup(d / "PSVUPDAT.PUP", &fw));
  EXPECT_EQ(fw, 0x03650000u);
  EXPECT_TRUE(isVitaPup(d / "PSVUPDAT.PUP", nullptr));
  std::vector<uint8_t> shortH(h.begin(), h.begin() + 0x1F);
  ASSERT_TRUE(nanotest::writeFile(d / "short.pup", shortH));
  EXPECT_FALSE(isVitaPup(d / "short.pup", &fw));
  h[0] = 'X';
  ASSERT_TRUE(nanotest::writeFile(d / "bad.pup", h));
  EXPECT_FALSE(isVitaPup(d / "bad.pup", &fw));
  EXPECT_FALSE(isVitaPup(d.path(), &fw));
}
