#include <gtest/gtest.h>

#include <string.h>
#include <sys/stat.h>

#include "NanoNdsBanner.h"
#include "nano_test_fs.h"
#include "nano_test_zip.h"

using nanotest::TempDir;
using nanotest::ZipMember;

namespace {

uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
  }
  return c;
}
void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void w32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// A DS ROM with valid header and banner checksums. titles[l] for language l (0 Japanese,
// 1 English, ...); nullptr leaves that language empty.
std::vector<uint8_t> makeRom(uint32_t bannerOff, const char16_t* en, size_t romSize = 0x8000,
                             const char16_t* ja = u"Other", uint16_t version = 1) {
  std::vector<uint8_t> r(romSize, 0);
  uint8_t* h = r.data();
  memcpy(h, "TESTGAME", 8);
  w32(h + 0x68, bannerOff);
  w16(h + 0x15C, 0xCF56);
  w16(h + 0x15E, crc16(h, 0x15E));
  if ((uint64_t)bannerOff + 0x840 <= romSize) {
    uint8_t* b = h + bannerOff;
    w16(b, version);
    for (int i = 0; i < 512; i++) b[0x20 + i] = (uint8_t)(i * 7);
    for (int i = 0; i < 16; i++) w16(b + 0x220 + i * 2, (uint16_t)(i * 0x0421));
    for (int l = 0; l < 6; l++) {
      const char16_t* t = l == 1 ? en : l == 0 ? ja : u"Other";
      for (int i = 0; t && t[i] && i < 127; i++) w16(b + 0x240 + l * 0x100 + i * 2, t[i]);
    }
    w16(b + 2, crc16(b + 0x20, 0x820));
  }
  return r;
}

std::vector<uint8_t> zipOf(const std::vector<uint8_t>& rom, int method, const char* name = "game.nds") {
  ZipMember m;
  m.name = name; m.data = rom; m.method = method;
  return nanotest::makeZip({m});
}

class NdsBanner : public ::testing::Test {
protected:
  TempDir dir{"nds"};
  int n = 0;
  NdsBannerInfo info;
  std::string put(const std::vector<uint8_t>& d, const char* suffix) {
    const std::string p = dir / ("f" + std::to_string(n++) + suffix);
    EXPECT_TRUE(nanotest::writeFile(p, d));
    return p;
  }
  bool read(const std::vector<uint8_t>& d, const char* suffix) { return ndsReadBanner(put(d, suffix), info); }
};

const std::vector<uint8_t> kRom = makeRom(0x1000, u"Mario Kart DS\nNintendo");

}  // namespace

TEST_F(NdsBanner, RawRom) {
  ASSERT_TRUE(read(kRom, ".nds"));
  EXPECT_EQ(info.title, "Mario Kart DS");
  ASSERT_EQ(info.rgba.size(), 32u * 32 * 4);
  EXPECT_EQ(info.rgba[3], 0);                    // palette index 0 is transparent
  const uint8_t* px = &info.rgba[2 * 4];         // tile 0 byte 1 = 7: palette entry 7 = grey 7/31
  EXPECT_EQ(px[0], 7 * 255 / 31); EXPECT_EQ(px[1], px[0]); EXPECT_EQ(px[2], px[0]); EXPECT_EQ(px[3], 255);
  EXPECT_TRUE(read(kRom, ".NDS"));               // extension in any case
}

TEST_F(NdsBanner, StoredAndDeflatedZip) {
  ASSERT_TRUE(read(zipOf(kRom, 0), ".zip"));
  EXPECT_EQ(info.title, "Mario Kart DS");
  ASSERT_TRUE(read(zipOf(kRom, 8), ".ZIP"));
  EXPECT_EQ(info.title, "Mario Kart DS");
  EXPECT_EQ(info.rgba.size(), 32u * 32 * 4);
}

TEST_F(NdsBanner, ZipUsesFirstNdsEntry) {
  ZipMember readme; readme.name = "readme.txt"; readme.data = {'h', 'i'};
  ZipMember rom; rom.name = "dir/Game.NDS"; rom.data = kRom; rom.method = 8;
  ASSERT_TRUE(read(nanotest::makeZip({readme, rom}), ".zip"));
  EXPECT_EQ(info.title, "Mario Kart DS");
  EXPECT_FALSE(read(nanotest::makeZip({readme}), ".zip"));
}

TEST_F(NdsBanner, TitleLines) {
  ASSERT_TRUE(read(makeRom(0x1000, u"Zelda\nPhantom Hourglass\nNintendo"), ".nds"));
  EXPECT_EQ(info.title, "Zelda Phantom Hourglass");   // publisher line dropped
  ASSERT_TRUE(read(makeRom(0x1000, u"  Tetris DS  "), ".nds"));
  EXPECT_EQ(info.title, "Tetris DS");                 // a single line is kept, trimmed
  ASSERT_TRUE(read(makeRom(0x1000, nullptr, 0x8000, u"Nihon\nPub"), ".nds"));
  EXPECT_EQ(info.title, "Nihon");                     // no English: the next language
}

TEST_F(NdsBanner, SurrogatesInTitle) {
  ASSERT_TRUE(read(makeRom(0x1000, u"A\U0001F600B\xD800"), ".nds"));
  EXPECT_EQ(info.title, "A\xF0\x9F\x98\x80" "B");     // the pair decoded, the lone half dropped
}

TEST_F(NdsBanner, RejectsBadChecksumsAndOffsets) {
  auto bad = kRom; bad[0] ^= 1;
  EXPECT_FALSE(read(bad, ".nds"));
  EXPECT_TRUE(info.rgba.empty());
  bad = kRom; bad[0x15C] ^= 1;                                          // logo checksum
  EXPECT_FALSE(read(bad, ".nds"));
  bad = kRom; bad[0x1000 + 0x30] ^= 1;                                  // banner CRC
  EXPECT_FALSE(read(bad, ".nds"));
  EXPECT_FALSE(read(makeRom(0x1000, u"x", 0x8000, u"y", 0), ".nds"));   // banner version 0
  EXPECT_FALSE(read(makeRom(0x100, u"x"), ".nds"));                     // inside the header
  EXPECT_FALSE(read(makeRom(0x7FFF, u"x"), ".nds"));                    // runs off the end
  EXPECT_FALSE(read(makeRom(0xFFFFFFFFu, u"x"), ".nds"));
}

TEST_F(NdsBanner, RejectsOtherFiles) {
  EXPECT_FALSE(read(std::vector<uint8_t>(10, 0), ".nds"));
  EXPECT_FALSE(read(kRom, ".gba"));
  EXPECT_FALSE(ndsReadBanner(dir / "missing.nds", info));
  const std::string sub = dir / "sub.nds";
  ASSERT_EQ(mkdir(sub.c_str(), 0700), 0);
  EXPECT_FALSE(ndsReadBanner(sub, info));
}

TEST_F(NdsBanner, RejectsUnsupportedZips) {
  auto z = zipOf(kRom, 8);
  auto m = z;   // method 12 (bzip2) in the central directory
  const size_t cd = z.size() - 22 - (46 + 8);
  m[cd + 10] = 12;
  EXPECT_FALSE(read(m, ".zip"));
  auto z64 = z;   // zip64 marker sizes
  nanotest::put32(z64, cd + 20, 0xFFFFFFFFu);
  EXPECT_FALSE(read(z64, ".zip"));
  auto noLocal = z;   // the local header the directory points at is missing
  nanotest::put32(noLocal, 0, 0);
  EXPECT_FALSE(read(noLocal, ".zip"));
}

TEST_F(NdsBanner, RawTruncationsRejected) {
  for (size_t cut : {(size_t)0x1FF, (size_t)0x200, (size_t)0x1000, (size_t)0x1000 + 0x83F}) {
    std::vector<uint8_t> t(kRom.begin(), kRom.begin() + cut);
    EXPECT_FALSE(read(t, ".nds")) << cut;
  }
}

TEST_F(NdsBanner, ZipPrefixesRejected) {
  const auto z = zipOf(kRom, 8);
  for (size_t cut = 0; cut < z.size(); cut += 97) {
    std::vector<uint8_t> t(z.begin(), z.begin() + cut);
    EXPECT_FALSE(read(t, ".zip")) << cut;
  }
}

// A deflated entry is inflated only up to 24 MB: a banner beyond that is not worth the time.
TEST_F(NdsBanner, DeflateCap) {
  const auto deep = makeRom(25u << 20, u"Deep", (25u << 20) + 0x1000);
  EXPECT_FALSE(read(zipOf(deep, 8), ".zip"));
  ASSERT_TRUE(read(zipOf(deep, 0), ".zip"));          // stored: read in place, no cap
  EXPECT_EQ(info.title, "Deep");
  const auto under = makeRom(23u << 20, u"Under", (23u << 20) + 0x1000);
  ASSERT_TRUE(read(zipOf(under, 8), ".zip"));
  EXPECT_EQ(info.title, "Under");
}

TEST_F(NdsBanner, ByteFlipsNeverCrash) {
  const auto z = zipOf(kRom, 8);
  unsigned seed = 1;
  for (int i = 0; i < 300; i++) {
    auto t = z;
    for (int k = 0; k < 4; k++) {
      seed = seed * 1103515245 + 12345;
      t[(seed >> 8) % t.size()] ^= (uint8_t)(seed >> 3);
    }
    if (read(t, ".zip")) EXPECT_EQ(info.rgba.size(), 32u * 32 * 4);
  }
}
