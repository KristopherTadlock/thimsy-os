// The BIOS installer's worker end to end on a fixture tree (a fake /data and a card under a scratch
// folder, Env.root): what lands where, what is left alone, and what it runs. `m gammaos_nano_bios_e2e_tests`.
// Runs as any user: chown and SELinux label failures are only logged.

#include <gtest/gtest.h>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "NanoBiosCatalog.h"
#include "NanoBiosWorker.h"
#include "NanoJson.h"
#include "nano_test_fs.h"

using namespace android;
using namespace android::nanobios;
using nanotest::TempDir;

namespace {

std::string realDir(const TempDir& t) {
  char buf[PATH_MAX];
  return realpath(t.path().c_str(), buf) ? std::string(buf) : t.path();
}

void mkdirs(const std::string& p) {
  for (size_t i = 1; i <= p.size(); i++)
    if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
}

void put(const std::string& p, const std::string& text) {
  mkdirs(p.substr(0, p.find_last_of('/')));
  const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0) << p;
  ASSERT_EQ((ssize_t)text.size(), write(fd, text.data(), text.size()));
  close(fd);
}

std::string get(const std::string& p) {
  std::string out;
  const int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return "<none>";
  char buf[65536];
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) out.append(buf, (size_t)n);
  close(fd);
  return out;
}

bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }

// Every path under dir whose name holds needle.
void findNamed(const std::string& dir, const std::string& needle, std::vector<std::string>* out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    const std::string p = dir + "/" + e->d_name;
    if (strstr(e->d_name, needle.c_str())) out->push_back(p);
    struct stat st;
    if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) findNamed(p, needle, out);
  }
  closedir(d);
}

std::string pattern(size_t n, unsigned seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; i++) s[i] = (char)((i * 31 + seed * 7 + (i >> 5)) & 0xff);
  return s;
}

// A 4 MiB PS2 BIOS image of nobody's: a ROMDIR (RESET, ROMDIR, ROMVER) and its version string.
std::string ps2Image(const char* romver) {
  std::string rom(4194304, '\0');
  auto entry = [&](size_t at, const char* name, uint32_t size) {
    memcpy(&rom[at], name, strlen(name));
    rom[at + 12] = (char)(size & 0xff);
    rom[at + 13] = (char)((size >> 8) & 0xff);
    rom[at + 14] = (char)((size >> 16) & 0xff);
    rom[at + 15] = (char)((size >> 24) & 0xff);
  };
  entry(0x100, "RESET", 0x100);    // at 0
  entry(0x110, "ROMDIR", 0x40);    // at 0x100 (this table)
  entry(0x120, "ROMVER", 0x10);    // at 0x140
  memcpy(&rom[0x140], romver, 14);
  for (size_t i = 0x1000; i < rom.size(); i += 4096) rom[i] = (char)(i >> 12);   // nothing in the catalogue
  return rom;
}

struct Recorder {
  std::mutex mu;
  std::vector<std::vector<std::string>> calls;
  std::set<std::string> forceStopped() {
    std::lock_guard<std::mutex> lk(mu);
    std::set<std::string> out;
    for (const auto& c : calls)
      if (c.size() == 3 && c[0] == "/system/bin/am" && c[1] == "force-stop") out.insert(c[2]);
    return out;
  }
};

class BiosE2e : public ::testing::Test {
protected:
  void SetUp() override {
    root = realDir(tmp);
    card = root + "/mnt/media_rw/TEST-0001";
    folder = card + "/bios";
    media = root + "/data/media/0";
    raSystem = media + "/RetroArch/system";
    armsx2Bios = media + "/Android/data/com.armsx2/files/bios";
    drastic = root + "/data/data/com.dsemu.drastic/files/DraStic/system";
    scummExtra = root + "/data/data/org.scummvm.scummvm/files/extra";
    raCfg = media + "/Android/data/com.retroarch.aarch64/files/retroarch.cfg";

    // The device's own folders, and the installed emulators (Vita3K, Flycast and others are not).
    put(root + "/data/system/packages.list",
        "com.retroarch.aarch64 10101 0 /data/user/0/com.retroarch.aarch64 default:targetSdkVersion=34 3003\n"
        "com.armsx2 10102 0 /data/user/0/com.armsx2 default 3003\n"
        "com.dsemu.drastic 10103 0 /data/user/0/com.dsemu.drastic default 3003\n"
        "org.scummvm.scummvm 10104 0 /data/user/0/org.scummvm.scummvm default 3003\n"
        "org.mupen64plusae.v3.fzurita 10105 0 /data/user/0/org.mupen64plusae.v3.fzurita default 3003\n");
    for (const char* pkg : {"com.retroarch.aarch64", "com.armsx2", "com.dsemu.drastic", "org.scummvm.scummvm",
                            "org.mupen64plusae.v3.fzurita"})
      mkdirs(root + "/data/data/" + pkg);
    mkdirs(media + "/Android/data/com.armsx2");
    mkdirs(media + "/Android/obb");
    mkdirs(root + "/outside");
    put(raCfg, "video_driver = \"gl\"\nsystem_directory = \"/storage/emulated/0/RetroArch/system\"\n");
    put(root + "/data/data/com.armsx2/shared_prefs/ARMSX2.xml",
        "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n    <string name=\"systemDir\"></string>\n</map>\n");
    put(root + "/data/data/org.mupen64plusae.v3.fzurita/shared_prefs/org.mupen64plusae.v3.fzurita_preferences.xml", "<map />\n");
    put(root + "/data/data/org.scummvm.scummvm/files/scummvm.ini", "[scummvm]\nsavepath=/data/data/org.scummvm.scummvm/files/saves\n");

    // The card's bios folder.
    lynx = pattern(512, 1);
    coleco = pattern(8192, 2);
    disksys = pattern(8192, 3);
    put(folder + "/lynx-boot.bin", lynx);          // any name: recognised by its SHA-1
    put(folder + "/sub/coleco.rom", coleco);
    std::string fw = pattern(131072, 4);
    memcpy(&fw[8], "MAC", 3);
    put(folder + "/firmware.bin", fw);
    ps2 = ps2Image("0220EC20060210");
    put(folder + "/my-ps2.bin", ps2);
    put(folder + "/my-ps2.nvm", pattern(1024, 5));
    put(folder + "/MT32_CONTROL.ROM", pattern(65536, 6));
    std::string pup = pattern(3 << 19, 7);
    memcpy(&pup[0], "SCEUF", 5);
    put(folder + "/PSP2UPDAT.PUP", pup);
    put(folder + "/notes.txt", "hello");
    // A link out of the chosen folder, to a file the catalogue knows: never followed.
    put(root + "/outside/disksys.rom", disksys);
    ASSERT_EQ(0, symlink((root + "/outside/disksys.rom").c_str(), (folder + "/disksys.rom").c_str()));

    // Synthetic SHA-1s in place of three real BIOS files' (same names, sizes and slots).
    catalog = files();
    sha.clear();
    sha.reserve(8);   // the catalogue points into these
    for (auto& f : catalog) {
      const std::string name = f.name;
      const std::string* data = name == "lynxboot.img" ? &lynx : name == "colecovision.rom" ? &coleco
                                : name == "disksys.rom" ? &disksys : nullptr;
      if (!data) continue;
      sha.push_back(sha1Hex(data->data(), data->size()));
      f.sha1 = sha.back().c_str();
    }
    ASSERT_EQ(3u, sha.size());
    setCatalogForTest(&catalog);

    env.root = root;
    env.region = REGION_EUR;
    env.run = [this](const std::vector<std::string>& args, std::string*, int) {
      std::lock_guard<std::mutex> lk(rec.mu);
      rec.calls.push_back(args);
      return 0;
    };
  }
  void TearDown() override { setCatalogForTest(nullptr); }

  InstallProgress install(bool fromManifest = false) {
    shared.cancel = false;
    shared.busy = shared.running = true;
    std::string next;
    EXPECT_FALSE(runInstall(env, shared, fromManifest ? std::string() : folder, fromManifest, raCfg, &next));
    std::lock_guard<std::mutex> lk(shared.mu);
    return shared.progress;
  }
  static bool hasLine(const InstallProgress& p, const std::string& needle) {
    return std::any_of(p.lines.begin(), p.lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
  }
  static std::string allLines(const InstallProgress& p) {
    std::string s;
    for (const auto& l : p.lines) s += l + "\n";
    return s;
  }

  TempDir tmp{"bios_e2e"};
  std::string root, card, folder, media, raSystem, armsx2Bios, drastic, scummExtra, raCfg;
  std::string lynx, coleco, disksys, ps2;
  std::vector<File> catalog;
  std::vector<std::string> sha;
  Env env;
  InstallShared shared;
  Recorder rec;
};

}  // namespace

TEST_F(BiosE2e, FilesLandWhereEachEmulatorReadsThem) {
  const InstallProgress p = install();
  const std::string report = allLines(p);
  EXPECT_TRUE(p.done);
  EXPECT_FALSE(shared.busy);
  EXPECT_EQ(lynx, get(raSystem + "/lynxboot.img")) << report;
  EXPECT_EQ(coleco, get(raSystem + "/colecovision.rom")) << report;
  EXPECT_EQ(ps2, get(armsx2Bios + "/my-ps2.bin")) << report;
  EXPECT_TRUE(exists(armsx2Bios + "/my-ps2.nvm")) << report;
  EXPECT_TRUE(exists(drastic + "/nds_firmware.bin")) << report;
  EXPECT_TRUE(exists(scummExtra + "/MT32_CONTROL.ROM")) << report;
  EXPECT_GE(p.installed, 5) << report;
  EXPECT_EQ(0, p.conflicts) << report;
  EXPECT_TRUE(hasLine(p, "Vita3K not installed")) << report;
  EXPECT_TRUE(hasLine(p, "ARMSX2 uses my-ps2.bin")) << report;
}

TEST_F(BiosE2e, SettingsAreWrittenForEmulatorsThatGotAFile) {
  install();
  const std::string armsx2 = get(root + "/data/data/com.armsx2/shared_prefs/ARMSX2.xml");
  EXPECT_NE(std::string::npos,
            armsx2.find("<string name=\"bios\">/storage/emulated/0/Android/data/com.armsx2/files/bios/my-ps2.bin</string>"))
      << armsx2;
  EXPECT_NE(std::string::npos, armsx2.find("<string name=\"systemDir\"></string>")) << armsx2;   // the rest kept
  const std::string ini = get(root + "/data/data/org.scummvm.scummvm/files/scummvm.ini");
  EXPECT_EQ("[scummvm]\nsavepath=/data/data/org.scummvm.scummvm/files/saves\nextrapath=/data/data/org.scummvm.scummvm/files/extra\n", ini);
  // M64Plus FZ got nothing: its settings are untouched.
  EXPECT_EQ("<map />\n", get(root + "/data/data/org.mupen64plusae.v3.fzurita/shared_prefs/org.mupen64plusae.v3.fzurita_preferences.xml"));
}

TEST_F(BiosE2e, ForceStopOnlyForEmulatorsThatGotAFile) {
  install();
  EXPECT_EQ((std::set<std::string>{"com.armsx2", "org.scummvm.scummvm"}), rec.forceStopped());
  // restorecon for what was made in app folders, nothing for Vita3K (not installed).
  bool restorecon = false;
  for (const auto& c : rec.calls) {
    if (!c.empty() && c[0] == "/system/bin/restorecon") restorecon = true;
    for (const auto& a : c) EXPECT_EQ(std::string::npos, a.find("vita3k")) << a;
  }
  EXPECT_TRUE(restorecon);
}

TEST_F(BiosE2e, LinkOutOfTheChosenFolderIsIgnored) {
  const InstallProgress p = install();
  EXPECT_FALSE(exists(raSystem + "/disksys.rom")) << allLines(p);
  EXPECT_EQ(disksys, get(root + "/outside/disksys.rom"));
}

TEST_F(BiosE2e, DifferentFileIsAConflictAndKeptTheSameOneIsAlreadyThere) {
  put(raSystem + "/lynxboot.img", lynx);                    // the same: already there
  put(raSystem + "/colecovision.rom", "the user's own");    // different: kept
  const InstallProgress p = install();
  const std::string report = allLines(p);
  EXPECT_EQ("the user's own", get(raSystem + "/colecovision.rom"));
  EXPECT_EQ(lynx, get(raSystem + "/lynxboot.img"));
  EXPECT_EQ(1, p.conflicts) << report;
  EXPECT_GE(p.already, 1) << report;
  EXPECT_TRUE(hasLine(p, "a different colecovision.rom is already there (kept)")) << report;
  EXPECT_TRUE(hasLine(p, "lynxboot.img already installed")) << report;
}

TEST_F(BiosE2e, LinkSwappedIntoTheTargetWritesNothingOutside) {
  mkdirs(raSystem);   // a real folder when it is checked
  const std::string target = root + "/outside/system";
  mkdirs(target);
  bool swapped = false;
  env.freeSpace = [&](const std::string&, uint64_t* bsize, uint64_t* avail) {
    if (!swapped) {   // after the check, before the copy
      swapped = true;
      rmdir(raSystem.c_str());
      EXPECT_EQ(0, symlink(target.c_str(), raSystem.c_str()));
    }
    *bsize = 4096;
    *avail = 1ull << 34;
    return true;
  };
  const InstallProgress p = install();
  ASSERT_TRUE(swapped);
  DIR* d = opendir(target.c_str());
  ASSERT_NE(nullptr, d);
  int n = 0;
  while (dirent* e = readdir(d)) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
  closedir(d);
  EXPECT_EQ(0, n) << allLines(p);
  EXPECT_TRUE(hasLine(p, "lynxboot.img could not be installed")) << allLines(p);
  EXPECT_TRUE(exists(armsx2Bios + "/my-ps2.bin"));   // other emulators' folders were fine
}

TEST_F(BiosE2e, FolderWithNothingRecognisedSaysSoAndChangesNothing) {
  folder = card + "/not-bios";
  put(folder + "/notes.txt", "hello");
  put(folder + "/game.bin", pattern(512, 11));   // a catalogue size, not a catalogue file
  const InstallProgress p = install();
  EXPECT_TRUE(p.done);
  EXPECT_TRUE(p.noneFound) << allLines(p);
  EXPECT_EQ(0, p.installed + p.already + p.conflicts) << allLines(p);
  EXPECT_EQ(1, p.unknown) << allLines(p);   // only the file of a catalogue size is looked at
  EXPECT_FALSE(exists(raSystem)) << allLines(p);
  EXPECT_TRUE(rec.forceStopped().empty());
}

TEST_F(BiosE2e, NoRoomInstallsNothing) {
  env.freeSpace = [](const std::string&, uint64_t* bsize, uint64_t* avail) {
    *bsize = 4096;
    *avail = 1 << 20;   // less than the 8 MB kept free
    return true;
  };
  const InstallProgress p = install();
  EXPECT_TRUE(hasLine(p, "Not enough space on Internal storage")) << allLines(p);
  EXPECT_EQ(0, p.installed);
  EXPECT_FALSE(p.noneFound);   // files were recognised, there was no room for them
  EXPECT_FALSE(exists(raSystem + "/lynxboot.img"));
  EXPECT_FALSE(exists(armsx2Bios + "/my-ps2.bin"));
  EXPECT_TRUE(rec.forceStopped().empty());
}

TEST_F(BiosE2e, CancelLeavesNoTempFiles) {
  env.wakeThrottled = [&]() {
    std::lock_guard<std::mutex> lk(shared.mu);
    if (shared.progress.current == "my-ps2.bin" && shared.progress.scanned == shared.progress.total) shared.cancel = true;
  };
  const InstallProgress p = install();
  EXPECT_TRUE(hasLine(p, "Install cancelled.")) << allLines(p);
  EXPECT_FALSE(exists(armsx2Bios + "/my-ps2.bin"));
  std::vector<std::string> tmps;
  findNamed(root, ".nanobios-", &tmps);
  findNamed(root, ".nanotmp", &tmps);
  EXPECT_TRUE(tmps.empty()) << tmps[0];
  EXPECT_TRUE(rec.forceStopped().count("com.armsx2") == 0);
}

TEST_F(BiosE2e, ManifestRecordsWhatWentWhereAndReinstallPutsItBack) {
  install();
  njson::Value m;
  ASSERT_TRUE(njson::parse(get(root + kManifestPath), &m)) << get(root + kManifestPath);
  const njson::Value* list = m.find("files");
  ASSERT_TRUE(list && list->isArray());
  bool sawLynx = false, sawPs2 = false;
  for (const auto& e : list->arr) {
    if (e.getString("file") == "lynxboot.img") {
      sawLynx = true;
      EXPECT_EQ(sha1Hex(lynx.data(), lynx.size()), e.getString("sha1"));
      EXPECT_EQ(folder + "/lynx-boot.bin", e.getString("source"));
      const njson::Value* t = e.find("targets");
      ASSERT_TRUE(t && t->isArray() && t->arr.size() == 1);
      EXPECT_EQ(raSystem + "/lynxboot.img", t->arr[0].asString());
    }
    if (e.getString("row") == "ps2" && e.getString("name") == "my-ps2.bin") {
      sawPs2 = true;
      EXPECT_EQ(REGION_EUR, e.getInt("region", -1));
    }
  }
  EXPECT_TRUE(sawLynx);
  EXPECT_TRUE(sawPs2);

  // After a move the emulators' copies are gone; a reinstall puts back what the manifest has.
  unlink((raSystem + "/lynxboot.img").c_str());
  unlink((armsx2Bios + "/my-ps2.bin").c_str());
  put(root + kReinstallMarker, "");
  rec.calls.clear();
  install(true);
  EXPECT_EQ(lynx, get(raSystem + "/lynxboot.img"));
  EXPECT_EQ(ps2, get(armsx2Bios + "/my-ps2.bin"));
  EXPECT_FALSE(exists(root + kReinstallMarker));   // every folder was there: done
  // ARMSX2's setting still names a file that is there: left alone.
  EXPECT_EQ(0u, rec.forceStopped().count("com.armsx2"));
}

TEST_F(BiosE2e, SettingsFileThatIsALinkIsNeverWritten) {
  const std::string ini = root + "/data/data/org.scummvm.scummvm/files/scummvm.ini";
  put(root + "/outside/secret.conf", "psk=hunter2\n");
  unlink(ini.c_str());
  ASSERT_EQ(0, symlink((root + "/outside/secret.conf").c_str(), ini.c_str()));
  install();
  EXPECT_EQ("psk=hunter2\n", get(root + "/outside/secret.conf"));
  struct stat st;
  ASSERT_EQ(0, lstat(ini.c_str(), &st));
  EXPECT_TRUE(S_ISLNK(st.st_mode));
}

TEST_F(BiosE2e, FoldersWithQuotesSpacesAndAmpersandsWork) {
  const std::string sys = media + "/Bob's RetroArch (1) & co/system";
  put(raCfg, "system_directory = \"/storage/emulated/0/Bob's RetroArch (1) & co/system\"\n");
  const InstallProgress p = install();
  EXPECT_EQ(lynx, get(sys + "/lynxboot.img")) << allLines(p);
  EXPECT_FALSE(exists(raSystem + "/lynxboot.img"));
}

// SeedlessDS reads the DS BIOS and a 256 KB firmware from files/SeedlessDS/system in its
// app folder, under the same names as DraStic.
TEST_F(BiosE2e, SeedlessDsGetsTheDsFilesWhenInstalled) {
  const std::string bios7 = pattern(16384, 8), bios9 = pattern(4096, 9);
  const std::string sha7 = sha1Hex(bios7.data(), bios7.size()), sha9 = sha1Hex(bios9.data(), bios9.size());
  for (auto& f : catalog) {
    if (!strcmp(f.row, "nds") && !strcmp(f.name, "bios7.bin")) f.sha1 = sha7.c_str();
    if (!strcmp(f.row, "nds") && !strcmp(f.name, "bios9.bin")) f.sha1 = sha9.c_str();
  }
  put(folder + "/ds/arm7.bin", bios7);
  put(folder + "/ds/arm9.bin", bios9);
  std::string lite = pattern(262144, 10);
  memcpy(&lite[8], "MAC", 3);
  put(folder + "/ds/firmware.bin", lite);
  put(root + "/data/system/packages.list", get(root + "/data/system/packages.list") +
      "com.seedlessds.app 10106 0 /data/user/0/com.seedlessds.app default 3003\n");
  mkdirs(root + "/data/data/com.seedlessds.app");

  const InstallProgress p = install();
  const std::string report = allLines(p);
  const std::string sys = root + "/data/data/com.seedlessds.app/files/SeedlessDS/system";
  EXPECT_EQ(bios7, get(sys + "/nds_bios_arm7.bin")) << report;
  EXPECT_EQ(bios9, get(sys + "/nds_bios_arm9.bin")) << report;
  EXPECT_EQ(lite, get(sys + "/nds_firmware.bin")) << report;   // the 128 KB one is not for it
  EXPECT_EQ(bios7, get(drastic + "/nds_bios_arm7.bin")) << report;
  EXPECT_FALSE(hasLine(p, "SeedlessDS not installed")) << report;
  EXPECT_EQ(0u, rec.forceStopped().count("com.seedlessds.app"));   // no settings of its are written
}

TEST_F(BiosE2e, SeedlessDsSkippedWhenNotInstalled) {
  const std::string bios9 = pattern(4096, 9);
  const std::string sha9 = sha1Hex(bios9.data(), bios9.size());
  for (auto& f : catalog)
    if (!strcmp(f.row, "nds") && !strcmp(f.name, "bios9.bin")) f.sha1 = sha9.c_str();
  put(folder + "/arm9.bin", bios9);
  std::string lite = pattern(262144, 10);
  memcpy(&lite[8], "MAC", 3);
  put(folder + "/ds/firmware.bin", lite);

  const InstallProgress p = install();
  EXPECT_EQ(bios9, get(drastic + "/nds_bios_arm9.bin")) << allLines(p);
  EXPECT_FALSE(exists(root + "/data/data/com.seedlessds.app")) << allLines(p);
  EXPECT_TRUE(hasLine(p, "SeedlessDS not installed")) << allLines(p);
}

// MAME4droid 2024 (not shipped) gets its files in its own roms folder, which Nano's launch commands put
// in its -rompath; nothing is written for it when it is not installed. A stand-in file with a slot of
// its (MAME's zips are recognised by real ROMs).
TEST_F(BiosE2e, Mame4droidGetsItsFilesInItsRomsFolderWhenInstalled) {
  const std::string data = pattern(4096, 12);
  const std::string sha = sha1Hex(data.data(), data.size());
  catalog.push_back(File{"m4d_gamate", "m4d-test.zip", "", MATCH_SHA1, 4096, 0, sha.c_str(), "", REC_ALL,
                         {Slot{SLOT_MAME4DROID, "", 0}}});
  put(folder + "/mame/whatever.bin", data);
  const std::string roms = media + "/Android/data/com.seleuco.mame4d2024/files/roms";

  InstallProgress p = install();
  EXPECT_FALSE(exists(roms + "/m4d-test.zip")) << allLines(p);
  EXPECT_FALSE(exists(media + "/Android/data/com.seleuco.mame4d2024")) << allLines(p);
  EXPECT_TRUE(hasLine(p, "MAME4droid 2024 not installed")) << allLines(p);

  put(root + "/data/system/packages.list", get(root + "/data/system/packages.list") +
      "com.seleuco.mame4d2024 10107 0 /data/user/0/com.seleuco.mame4d2024 default 3003\n");
  mkdirs(root + "/data/data/com.seleuco.mame4d2024");
  p = install();
  EXPECT_EQ(data, get(roms + "/m4d-test.zip")) << allLines(p);
  EXPECT_FALSE(hasLine(p, "MAME 2003-Plus")) << allLines(p);
}

// hakuX reads console/mcpx.bin, bios.bin and eeprom.bin; dumps usually keep xemu's names. A synthetic
// MD5 stands in for the real MCPX's. Of several BIOSes the first in the catalogue is installed.
TEST_F(BiosE2e, XboxDumpsUnderXemuNamesLandInHakuX) {
  const std::string mcpx = pattern(512, 13), bios = pattern(262144, 14), flash = pattern(1048576, 15),
                    eeprom = pattern(256, 16);
  put(folder + "/xbox/mcpx_1.0.bin", mcpx);
  put(folder + "/xbox/Complex_4627.bin", bios);
  put(folder + "/xbox/old/flash.bin", flash);
  put(folder + "/xbox/eeprom.bin", eeprom);
  put(folder + "/other/mcpx.bin", pattern(512, 17));   // the name alone is not enough
  std::string sha, md5;
  ASSERT_TRUE(hashFile(folder + "/xbox/mcpx_1.0.bin", &sha, &md5));
  int rows = 0;
  for (auto& f : catalog)
    if (!strcmp(f.row, "xbox") && f.match == MATCH_MD5) { f.md5 = md5.c_str(); rows++; }
  ASSERT_EQ(1, rows);
  put(root + "/data/system/packages.list", get(root + "/data/system/packages.list") +
      "com.rfandango.haku_x 10108 0 /data/user/0/com.rfandango.haku_x default 3003\n");
  mkdirs(root + "/data/data/com.rfandango.haku_x");
  const std::string console = root + "/data/data/com.rfandango.haku_x/files/console";

  InstallProgress p = install();
  std::string report = allLines(p);
  EXPECT_EQ(mcpx, get(console + "/mcpx.bin")) << report;
  EXPECT_EQ(bios, get(console + "/bios.bin")) << report;
  EXPECT_EQ(eeprom, get(console + "/eeprom.bin")) << report;
  EXPECT_FALSE(exists(console + "/mcpx_1.0.bin")) << report;
  EXPECT_FALSE(exists(console + "/Complex_4627.bin")) << report;
  EXPECT_FALSE(exists(console + "/flash.bin")) << report;
  EXPECT_TRUE(hasLine(p, "Xbox: mcpx_1.0.bin installed")) << report;
  EXPECT_TRUE(hasLine(p, "Xbox: Complex_4627.bin installed")) << report;
  EXPECT_FALSE(hasLine(p, "flash.bin")) << report;
  EXPECT_TRUE(std::any_of(p.lines.begin(), p.lines.end(), [](const std::string& l) {
    return l.find("not recognised: ") != std::string::npos && l.find("mcpx.bin") != std::string::npos;
  })) << report;
  EXPECT_EQ(0, p.conflicts) << report;

  // Again: the same files are already there, and the other BIOS still never replaces the first.
  p = install();
  report = allLines(p);
  EXPECT_TRUE(hasLine(p, "Xbox: mcpx_1.0.bin already installed")) << report;
  EXPECT_TRUE(hasLine(p, "Xbox: Complex_4627.bin already installed")) << report;
  EXPECT_EQ(bios, get(console + "/bios.bin")) << report;
  EXPECT_EQ(0, p.conflicts) << report;
}
