// The microSD move's source allowlist (audit H4) and the emulator settings parsers it reads.

#include <gtest/gtest.h>

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "NanoAppFiles.h"
#include "nano_test_fs.h"

using namespace android;
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

}  // namespace

// ---- H4: where a move may take an emulator's files from ----

TEST(SdSourceRoot, RefusesSystemFoldersTheOldCheckLetThrough) {
  // sdTooBroad alone accepted these (the audit's sdmove_check); the allowlist must not.
  for (const char* p : {"/data/system", "/data/misc/wifi", "/data/app", "/data/system/users/0", "/data",
                        "/data/data/com.android.providers.contacts", "/data/data/com.android.providers.contacts/databases",
                        "/system/etc", "/", "/proc/self"}) {
    EXPECT_EQ("", sdSourceRoot(p, "com.dsemu.drastic")) << p;
  }
}

TEST(SdSourceRoot, RefusesAnotherAppsFolders) {
  EXPECT_EQ("", sdSourceRoot("/data/data/org.ppsspp.ppsspp/files", "com.dsemu.drastic"));
  EXPECT_EQ("", sdSourceRoot("/data/user/0/org.ppsspp.ppsspp/files", "com.dsemu.drastic"));
  EXPECT_EQ("", sdSourceRoot("/storage/emulated/0/Android/data/org.ppsspp.ppsspp/files", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/data/media/0/Android/obb/com.armsx2", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/data/media/0/Android/data", "com.armsx2"));
  // The built-in DS player has no app folder of its own.
  EXPECT_EQ("", sdSourceRoot("/data/data/com.dsemu.drastic/files", ""));
}

TEST(SdSourceRoot, RefusesARootItselfAndDotDot) {
  EXPECT_EQ("", sdSourceRoot("/data/media/0", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/storage/emulated/0", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/mnt/media_rw/1234-ABCD", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/data/data/com.armsx2", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/data/media/0/../../system", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/storage/emulated/0/Flycast/../../../data/system", "com.flycast.emulator"));
  EXPECT_EQ("", sdSourceRoot("relative/path", "com.armsx2"));
}

// Every spelling an emulator's settings use for shared storage, a card or its app folder.
TEST(SdSourceRoot, AcceptsEverySpellingOfSharedStorageAndCards) {
  const char* pkg = "com.flycast.emulator";
  EXPECT_EQ("/data/media/0", sdSourceRoot("/storage/emulated/0/Flycast", pkg));
  EXPECT_EQ("/data/media/0", sdSourceRoot("/sdcard/Flycast", pkg));
  EXPECT_EQ("/data/media/0", sdSourceRoot("/storage/self/primary/Flycast", pkg));
  EXPECT_EQ("/data/media/0", sdSourceRoot("/data/media/0/Flycast", pkg));
  EXPECT_EQ("/data/media/0", sdSourceRoot("/storage/emulated/0/Flycast/", pkg));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD", sdSourceRoot("/storage/1234-ABCD/Flycast", pkg));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD", sdSourceRoot("/mnt/media_rw/1234-ABCD/Flycast", pkg));
  EXPECT_EQ("/data/data/com.flycast.emulator", sdSourceRoot("/data/user/0/com.flycast.emulator/files", pkg));
  EXPECT_EQ("/data/data/com.flycast.emulator", sdSourceRoot("/data/data/com.flycast.emulator/files", pkg));
  EXPECT_EQ("/data/media/0/Android/data/com.flycast.emulator",
            sdSourceRoot("/storage/emulated/0/Android/data/com.flycast.emulator/files", pkg));
  EXPECT_EQ("/data/media/0/Android/data/com.flycast.emulator",
            sdSourceRoot("/sdcard/Android/data/com.flycast.emulator/files/data", pkg));
}

// What the six emulators' settings give today, untouched, and the paths emuMoveStart builds from
// them (rawStoragePath of the folder or one of its parts): all still accepted.
TEST(SdSourceRoot, AcceptsTodaysDefaultSourcesOfEveryEmulator) {
  struct Case { std::string src; const char* pkg; };
  bool internal = false, chosen = false;
  const Case cases[] = {
      {flycastHomeFrom(""), "com.flycast.emulator"},                                         // Flycast, unset
      {flycastHomeFrom("<map><string name=\"home_directory\">/storage/emulated/0/Flycast/</string></map>") + "/emu.cfg",
       "com.flycast.emulator"},
      {drasticSystemDirFrom(""), "com.dsemu.drastic"},                                        // DraStic, unset
      {drasticSystemDirFrom("<map><string name=\"_SystemPath\">/fp19/storage/emulated/0/DraStic</string></map>"),
       "com.dsemu.drastic"},
      {mupenDataDirFrom("", &internal) + "/GameData", "org.mupen64plusae.v3.fzurita"},        // M64Plus FZ, unset
      {"/data/data/org.mupen64plusae.v3.fzurita/files/GameData", "org.mupen64plusae.v3.fzurita"},   // internal
      {mupenDataDirFrom("<map><string name=\"gameDataStoragePath\">file:///storage/emulated/0/My%20N64</string></map>",
                        &internal) + "/GameData", "org.mupen64plusae.v3.fzurita"},
      {armsx2SystemDirFrom("<map />", &chosen), "com.armsx2"},                                 // ARMSX2, unset
      {armsx2SystemDirFrom("<map><string name=\"systemDir\">/storage/1234-ABCD/ARMSX2</string></map>", &chosen), "com.armsx2"},
      {scummvmSavePath(""), "org.scummvm.scummvm"},                                           // ScummVM, unset
      {scummvmSavePath("[scummvm]\nsavepath=/sdcard/ScummVM/Saves\n"), "org.scummvm.scummvm"},
      {appdef::kVita3kDefPref, "org.vita3k.emulator"},                                         // Vita3K, unset
      {vita3kPrefPath("pref-path: /storage/emulated/0/vita/\n"), "org.vita3k.emulator"},
  };
  for (const auto& c : cases) {
    ASSERT_FALSE(c.src.empty());
    EXPECT_NE("", sdSourceRoot(c.src, c.pkg)) << c.src;
    EXPECT_FALSE(sdTooBroad(c.src, false)) << c.src;
  }
}

TEST(SdSourceRoot, AcceptsNamesPeopleGiveFolders) {
  EXPECT_EQ("/data/media/0", sdSourceRoot("/storage/emulated/0/Bob's Games/Flycast", "com.flycast.emulator"));
  EXPECT_EQ("/data/media/0", sdSourceRoot("/sdcard/Emus & Saves (old)/DraStic", "com.dsemu.drastic"));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD", sdSourceRoot("/storage/1234-ABCD/Kid's (2) & co/ARMSX2", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/storage/emulated/0/a\nb", "com.armsx2"));
  EXPECT_EQ("", sdSourceRoot("/storage/emulated/0/a\tb", "com.armsx2"));
}

TEST(SdSourceRoot, CardAppFoldersAsInternalOnes) {
  const char* pkg = "com.armsx2";
  EXPECT_EQ("/mnt/media_rw/1234-ABCD/Android/data/com.armsx2",
            sdSourceRoot("/storage/1234-ABCD/Android/data/com.armsx2/files", pkg));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD/android/DATA/com.armsx2",
            sdSourceRoot("/mnt/media_rw/1234-ABCD/android/DATA/com.armsx2/files", pkg));   // FAT: any case
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/Android/data/org.ppsspp.ppsspp/files", pkg));
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/ANDROID/Data/org.ppsspp.ppsspp/files", pkg));
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/Android/data/com.armsx2", pkg));   // its folder itself
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/Android/data", pkg));
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/Android/obb/com.armsx2", pkg));
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/android/OBB", pkg));
  EXPECT_EQ("", sdSourceRoot("/storage/1234-ABCD/Android/data/com.armsx2/files", ""));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD", sdSourceRoot("/storage/1234-ABCD/Android Games/x", pkg));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD", sdSourceRoot("/storage/1234-ABCD/Android/media/x", pkg));
}

TEST(SdSourceRoot, ResolvesLinksBeforeChecking) {
  TempDir t("srcroot");
  const std::string root = realDir(t);
  mkdirs(root + "/data/media/0/Emu/saves");
  mkdirs(root + "/data/system/secret");
  ASSERT_EQ(0, symlink((root + "/data/system").c_str(), (root + "/data/media/0/Link").c_str()));
  ASSERT_EQ(0, symlink((root + "/data/system").c_str(), (root + "/data/media/0/Emu/inner").c_str()));
  EXPECT_EQ(root + "/data/media/0", sdSourceRoot(root + "/data/media/0/Emu", "com.armsx2", root));
  EXPECT_EQ(root + "/data/media/0", sdSourceRoot(root + "/data/media/0/Emu/saves", "com.armsx2", root));
  EXPECT_EQ("", sdSourceRoot(root + "/data/media/0/Link", "com.armsx2", root));
  EXPECT_EQ("", sdSourceRoot(root + "/data/media/0/Link/secret", "com.armsx2", root));
  EXPECT_EQ("", sdSourceRoot(root + "/data/media/0/Emu/inner/not-there-yet", "com.armsx2", root));
}

TEST(SdTooBroad, StorageRootsAndStandardFolders) {
  EXPECT_TRUE(sdTooBroad("/storage/emulated/0", false));
  EXPECT_TRUE(sdTooBroad("/sdcard", false));
  EXPECT_TRUE(sdTooBroad("/storage/emulated/0/DCIM", false));
  EXPECT_TRUE(sdTooBroad("/storage/1234-ABCD/ROMs", false));
  EXPECT_TRUE(sdTooBroad("/storage/emulated/0/Android/data", false));
  EXPECT_TRUE(sdTooBroad("/storage/emulated/0/ES-DE", false));
  EXPECT_FALSE(sdTooBroad("/storage/emulated/0/ES-DE", true));
  EXPECT_TRUE(sdTooBroad("/data", false));
  EXPECT_FALSE(sdTooBroad("/storage/emulated/0/RetroArch", false));
}

TEST(SdCanon, MapsAppSpellings) {
  EXPECT_EQ("/data/media/0/x", sdCanon("/storage/emulated/0/x"));
  EXPECT_EQ("/data/media/0/x", sdCanon("/sdcard/x/"));
  EXPECT_EQ("/data/media/0", sdCanon("/storage/self/primary"));
  EXPECT_EQ("/mnt/media_rw/AB12-CD34/x", sdCanon("/storage/AB12-CD34/x"));
  EXPECT_EQ("/data/data/pkg/files", sdCanon("/data/user/0/pkg/files"));
  EXPECT_EQ("/data/media/10/x", sdCanon("/storage/emulated/10/x"));
}

// ---- the parsers ----

TEST(Parsers, DrasticSystemDir) {
  auto v = [](const std::string& sp) { return drasticSystemDirFrom("<map><string name=\"_SystemPath\">" + sp + "</string></map>"); };
  EXPECT_EQ(appdef::kDrasticDefDir, drasticSystemDirFrom(""));
  EXPECT_EQ(appdef::kDrasticDefDir, drasticSystemDirFrom("<map />"));
  EXPECT_EQ("/storage/emulated/0/DraStic", v("/fp19/storage/emulated/0/DraStic"));
  EXPECT_EQ("/storage/emulated/0/DraStic", v("/fp19/storage/emulated/0/DraStic/"));
  EXPECT_EQ("", v("/ss/content://com.android.externalstorage.documents/tree/primary%3ADraStic"));
  EXPECT_EQ("", v("/fp"));
  EXPECT_EQ("", v("/fp99/short"));
  EXPECT_EQ("", v("/fpx/storage"));
  // The round trip of what fileJobRelocate writes.
  EXPECT_EQ("/storage/1234-ABCD/emulator_config/drastic", v(drasticSystemPathValue("/storage/1234-ABCD/emulator_config/drastic")));
}

TEST(Parsers, DrasticSystemDirLongDigitRuns) {
  auto v = [](const std::string& sp) { return drasticSystemDirFrom("<map><string name=\"_SystemPath\">" + sp + "</string></map>"); };
  // 2^64 + 13 wraps to 13 in a size_t: the old parser then took "/data/system" out of the tail.
  EXPECT_EQ("", v("/fp18446744073709551629/data/system"));
  EXPECT_EQ("", v("/fp18446744073709551616/data/system"));
  EXPECT_EQ("", v("/fp" + std::string(400, '9') + "/data/system"));
  EXPECT_EQ("", v("/fp" + std::string(4000, '1')));
}

TEST(Parsers, Flycast) {
  EXPECT_EQ(appdef::kFlycastDefHome, flycastHomeFrom(""));
  EXPECT_EQ("/storage/emulated/0/Flycast",
            flycastHomeFrom("<map><string name=\"home_directory\">/storage/emulated/0/Flycast/</string></map>"));
  EXPECT_EQ("", flycastHomeFrom("<map><string name=\"home_directory\">content://x</string></map>"));
  EXPECT_EQ("", flycastHomeFrom("<map><string name=\"home_directory\"></string></map>"));
}

TEST(Parsers, Mupen) {
  bool internal = true;
  EXPECT_EQ(appdef::kMupenDefDir, mupenDataDirFrom("", &internal));
  EXPECT_FALSE(internal);
  EXPECT_EQ("", mupenDataDirFrom("<map><string name=\"gameDataStorageType\">internal</string></map>", &internal));
  EXPECT_TRUE(internal);
  EXPECT_EQ("/storage/emulated/0/My N64",
            mupenDataDirFrom("<map><string name=\"gameDataStoragePath\">file:///storage/emulated/0/My%20N64/</string></map>", &internal));
  EXPECT_EQ("", mupenDataDirFrom("<map><string name=\"gameDataStoragePath\">content://tree/x</string></map>", &internal));
  EXPECT_EQ("file:///storage/1234-ABCD/emulator_config/m64plus%20x", sdFileUri("/storage/1234-ABCD/emulator_config/m64plus x"));
  EXPECT_EQ("/a b/c%", sdUriDecode("/a%20b/c%"));
}

TEST(Parsers, Armsx2) {
  bool chosen = true;
  EXPECT_EQ(appdef::kArmsx2DefDir, armsx2SystemDirFrom("<map />", &chosen));
  EXPECT_FALSE(chosen);
  EXPECT_EQ("/storage/emulated/0/ARMSX2",
            armsx2SystemDirFrom("<map><string name=\"systemDir\">/sdcard/ARMSX2/</string></map>", &chosen));
  EXPECT_TRUE(chosen);
}

TEST(Parsers, Ppsspp) {
  bool set = false;
  EXPECT_EQ("/storage/emulated/0/PSP", ppssppMemstickFrom("/storage/emulated/0/PSP/\n", &set));
  EXPECT_TRUE(set);
  EXPECT_EQ("", ppssppMemstickFrom("content://com.android.externalstorage.documents/tree/x\n", &set));
  EXPECT_TRUE(set);
  EXPECT_EQ("", ppssppMemstickFrom(" \n", &set));
  EXPECT_FALSE(set);
}

TEST(Parsers, Vita3k) {
  EXPECT_EQ("", vita3kPrefPath("modules-mode: 0\n"));
  EXPECT_EQ("/storage/emulated/0/vita", vita3kPrefPath("a: 1\npref-path: /sdcard/vita/\nb: 2\n"));
  EXPECT_EQ("a: 1\npref-path: /x/\n", vita3kSetPrefPath("a: 1\npref-path: /old/\n", "/x"));
  EXPECT_EQ("a: 1\npref-path: /x/\n...\n", vita3kSetPrefPath("a: 1\n...\n", "/x"));
  EXPECT_EQ("a: 1\npref-path: /x/\n", vita3kSetPrefPath("a: 1", "/x"));
}

TEST(Parsers, Scummvm) {
  EXPECT_EQ(appdef::kScummvmDefSaves, scummvmSavePath(""));
  EXPECT_EQ(appdef::kScummvmDefSaves, scummvmSavePath("[monkey]\nsavepath=/x\n"));
  EXPECT_EQ("/storage/emulated/0/saves", scummvmSavePath("[ScummVM]\n SavePath = \"/sdcard/saves/\"\n"));
  EXPECT_EQ("", scummvmSavePath("[scummvm]\nsavepath=/saf/tree/x\n"));
  const std::string out = scummvmSetSavePath("[scummvm]\nsavepath=/old\n[monkey]\nsavepath=/old/monkey\n[other]\nsavepath=/elsewhere\n",
                                             "/old", "/new");
  EXPECT_EQ("[scummvm]\nsavepath=/new\n[monkey]\nsavepath=/new/monkey\n[other]\nsavepath=/elsewhere\n", out);
  EXPECT_EQ("[scummvm]\nsavepath=/new\n[g]\nx=1\n", scummvmSetSavePath("[g]\nx=1\n", "/old", "/new"));
}

TEST(Parsers, Azahar) {
  std::string uri;
  EXPECT_EQ("", azaharDir("", &uri));
  const std::string set = azaharTreeUri("1234-ABCD", "emulator_config/azahar");
  EXPECT_EQ("content://com.android.externalstorage.documents/tree/1234-ABCD%3Aemulator_config%2Fazahar", set);
  EXPECT_EQ("/storage/1234-ABCD/emulator_config/azahar",
            azaharDir("<map><string name=\"CITRA_DIRECTORY\">" + set + "</string></map>", &uri));
  EXPECT_EQ("/storage/emulated/0/azahar",
            azaharDir("<map><string name=\"CITRA_DIRECTORY\">" + azaharTreeUri("primary", "azahar") + "</string></map>", &uri));
  EXPECT_EQ("", azaharDir("<map><string name=\"CITRA_DIRECTORY\">content://other/tree/x</string></map>", &uri));
}

TEST(Parsers, PrefsText) {
  EXPECT_EQ("a<b>&\"'", sdPrefsGet("<map><string name=\"k\">a&lt;b&gt;&amp;&quot;&apos;</string></map>", "k"));
  EXPECT_EQ("&lt;", sdPrefsGet("<map><string name=\"k\">&amp;lt;</string></map>", "k"));
  std::string p;
  ASSERT_TRUE(sdPrefsSet(&p, "k", "a&b"));
  EXPECT_EQ("a&b", sdPrefsGet(p, "k"));
  ASSERT_TRUE(sdPrefsSet(&p, "k", "c"));
  EXPECT_EQ("c", sdPrefsGet(p, "k"));
  std::string e = "<map />";
  ASSERT_TRUE(sdPrefsSet(&e, "x", "1"));
  EXPECT_EQ("1", sdPrefsGet(e, "x"));
  sdPrefsRemove(&e, "x");
  EXPECT_EQ("", sdPrefsGet(e, "x"));
  EXPECT_EQ("/storage/emulated/0/x", sdTrimDir(" '/sdcard/x/' "));
  EXPECT_EQ("/data/data/p/f", sdTrimDir("/data/user/0/p/f"));
}

TEST(Parsers, PrefsInt) {
  EXPECT_EQ(7, sdPrefsGetInt("<map />", "_SaveLocation", 7));
  EXPECT_EQ(1, sdPrefsGetInt("<map>\n    <int name=\"_SaveLocation\" value=\"1\" />\n</map>", "_SaveLocation", 0));
  EXPECT_EQ(-1, sdPrefsGetInt("<map><int name=\"s_1\" value=\"-1\" /></map>", "s_1", 0));
  EXPECT_EQ(0, sdPrefsGetInt("<map><int name=\"_SaveLocation\" value=\"x\" /></map>", "_SaveLocation", 0));
  EXPECT_EQ(0, sdPrefsGetInt("<map><int name=\"_SaveLocationOld\" value=\"2\" /></map>", "_SaveLocation", 0));
  std::string p = "<map />";
  ASSERT_TRUE(sdPrefsSetInt(&p, "_SaveLocation", 2));
  EXPECT_EQ(2, sdPrefsGetInt(p, "_SaveLocation", 0));
  ASSERT_TRUE(sdPrefsSet(&p, "_SaveFolder", "content://x"));
  ASSERT_TRUE(sdPrefsSetInt(&p, "_SaveLocation", 1));
  EXPECT_EQ(1, sdPrefsGetInt(p, "_SaveLocation", 0));
  EXPECT_EQ(std::string::npos, p.find("value=\"2\""));
  EXPECT_EQ("content://x", sdPrefsGet(p, "_SaveFolder"));
  std::string none;
  ASSERT_TRUE(sdPrefsSetInt(&none, "k", 3));
  EXPECT_EQ(3, sdPrefsGetInt(none, "k", 0));
}

// SeedlessDS: _SaveLocation 0 (beside each game, its default), 1 (its app folder), 2 (a picked folder).
TEST(Parsers, SeedlessDs) {
  int loc = -1;
  std::string uri;
  EXPECT_EQ("", seedlessSaveDir("", &loc, &uri));
  EXPECT_EQ(kSeedlessSaveBesideGames, loc);
  EXPECT_EQ("", seedlessSaveDir("<map><int name=\"_SaveLocation\" value=\"0\" /></map>", &loc, &uri));
  EXPECT_EQ(appdef::kSeedlessUserDir,
            seedlessSaveDir("<map><int name=\"_SaveLocation\" value=\"1\" /></map>", &loc, &uri));
  EXPECT_EQ(kSeedlessSaveApp, loc);
  const std::string card = azaharTreeUri("1234-ABCD", "emulator_config/seedlessds");
  const std::string prefs = "<map>\n    <string name=\"_SaveFolder\">" + card +
                            "</string>\n    <int name=\"_SaveLocation\" value=\"2\" />\n</map>\n";
  EXPECT_EQ("/storage/1234-ABCD/emulator_config/seedlessds", seedlessSaveDir(prefs, &loc, &uri));
  EXPECT_EQ(kSeedlessSaveFolder, loc);
  EXPECT_EQ(card, uri);
  EXPECT_EQ("/storage/emulated/0/DS Saves",
            seedlessSaveDir("<map><string name=\"_SaveFolder\">" + azaharTreeUri("primary", "DS Saves") +
                            "</string><int name=\"_SaveLocation\" value=\"2\" /></map>", &loc, &uri));
  // Picked but no folder (or not a storage folder): nothing to move.
  EXPECT_EQ("", seedlessSaveDir("<map><int name=\"_SaveLocation\" value=\"2\" /></map>", &loc, &uri));
  EXPECT_EQ("", seedlessSaveDir("<map><string name=\"_SaveFolder\">content://com.example.docs/tree/1</string>"
                                "<int name=\"_SaveLocation\" value=\"2\" /></map>", &loc, &uri));
}

TEST(SdSourceRoot, AcceptsSeedlessDsFolders) {
  const char* pkg = "com.seedlessds.app";
  for (const char* part : {"/backup", "/savestates"}) {
    const std::string src = std::string(appdef::kSeedlessUserDir) + part;
    EXPECT_EQ("/data/data/com.seedlessds.app", sdSourceRoot(src, pkg)) << src;
    EXPECT_FALSE(sdTooBroad(src, false)) << src;
    EXPECT_EQ("", sdSourceRoot(src, "me.magnum.melondualds")) << src;
  }
  EXPECT_EQ("/data/data/com.seedlessds.app", sdSourceRoot("/data/user/0/com.seedlessds.app/files/SeedlessDS/user/backup", pkg));
  int loc = 0;
  std::string uri;
  const std::string picked = seedlessSaveDir(
      "<map><string name=\"_SaveFolder\">" + azaharTreeUri("primary", "DS Saves") + "</string>"
      "<int name=\"_SaveLocation\" value=\"2\" /></map>", &loc, &uri);
  EXPECT_EQ("/data/media/0", sdSourceRoot(picked, pkg));
  EXPECT_FALSE(sdTooBroad(picked, false));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD", sdSourceRoot("/storage/1234-ABCD/DS Saves", pkg));
}

TEST(SeedlessDs, SavesOnlyFolder) {
  TempDir t("seedless");
  const std::string d = realDir(t);
  auto touch = [](const std::string& p) { FILE* f = fopen(p.c_str(), "w"); if (f) fclose(f); };
  EXPECT_TRUE(seedlessSavesOnly(d));   // empty
  touch(d + "/Mario Kart DS.dsv");
  touch(d + "/Mario Kart DS_0.dss");
  touch(d + "/Mario Kart DS_0.ra");
  touch(d + "/Other.SAV");
  mkdirs(d + "/p-1234567");
  touch(d + "/p-1234567/Mario Kart DS_8.dss");
  mkdirs(d + "/p42");
  EXPECT_TRUE(seedlessSavesOnly(d));
  touch(d + "/p42/game.nds");   // a game in a profile's folder
  EXPECT_FALSE(seedlessSavesOnly(d));
  unlink((d + "/p42/game.nds").c_str());
  mkdirs(d + "/p42/deeper");
  EXPECT_FALSE(seedlessSavesOnly(d));
  rmdir((d + "/p42/deeper").c_str());
  touch(d + "/Mario Kart DS.nds");   // the games' own folder
  EXPECT_FALSE(seedlessSavesOnly(d));
  unlink((d + "/Mario Kart DS.nds").c_str());
  mkdirs(d + "/subfolder");
  EXPECT_FALSE(seedlessSavesOnly(d));
  rmdir((d + "/subfolder").c_str());
  ASSERT_EQ(0, symlink("/etc/passwd", (d + "/link.dsv").c_str()));
  EXPECT_FALSE(seedlessSavesOnly(d));
  unlink((d + "/link.dsv").c_str());
  EXPECT_TRUE(seedlessSavesOnly(d));
  EXPECT_FALSE(seedlessSavesOnly(d + "/not-there"));
}

// Flycast's emu.cfg names folders by full path once one is chosen in its settings; the move rewrites
// the ones in what moved.
TEST(FlycastCfg, PathSettingsFollowTheMove) {
  const std::string home = "/storage/emulated/0/Android/data/com.flycast.emulator/files";
  const std::string card = "/storage/1234-ABCD/emulator_config/flycast";
  const std::string cfg =
      "[config]\n"
      "Dreamcast.ContentPath = /storage/emulated/0/ROMs/dc;" + home + "/roms/\n"
      "Dreamcast.SavestatePath = " + home + "/data/\n"
      "Dreamcast.VMUPath = /sdcard/Android/data/com.flycast.emulator/files/data\n"
      "Dreamcast.BiosPath = \n"
      "Dreamcast.BoxartPath = \"" + home + "/a;b\"\n"
      "rend.Resolution = 960\n"
      "Dreamcast.Broadcast = 4\n";
  const std::string whole = flycastRewriteCfg(cfg, {{home, card}});
  EXPECT_EQ(
      "[config]\n"
      "Dreamcast.ContentPath = /storage/emulated/0/ROMs/dc;" + card + "/roms/\n"
      "Dreamcast.SavestatePath = " + card + "/data/\n"
      "Dreamcast.VMUPath = " + card + "/data\n"
      "Dreamcast.BiosPath = \n"
      "Dreamcast.BoxartPath = \"" + home + "/a;b\"\n"
      "rend.Resolution = 960\n"
      "Dreamcast.Broadcast = 4\n",
      whole);
  // A chosen home moves only its data, mappings and textures.
  const std::string parts = flycastRewriteCfg(cfg, {{home + "/data", card + "/data"}, {home + "/mappings", card + "/mappings"}});
  EXPECT_NE(std::string::npos, parts.find(home + "/roms/"));
  EXPECT_NE(std::string::npos, parts.find("Dreamcast.SavestatePath = " + card + "/data/\n"));
  EXPECT_EQ("rend.Resolution = 960\n", flycastRewriteCfg("rend.Resolution = 960\n", {{home, card}}));
}

// ARMSX2 resolves a relative [Folders] entry against its data folder and keeps its BIOS in its app
// folder (Pcsx2Config.cpp LoadPathFromSettings; native-lib.cpp pins Folders/Bios).
TEST(Armsx2Ini, FoldersFollowTheMoveButTheBiosStays) {
  const std::string from = "/storage/emulated/0/Android/data/com.armsx2/files";
  const std::string to = "/storage/1234-ABCD/emulator_config/armsx2";
  const std::string bios = from + "/bios";
  const std::string ini =
      "[EmuCore]\nSavestates = " + from + "/sstates\n"
      "[Folders]\n"
      "Bios = " + from + "/bios\n"
      "Snapshots = snaps\n"
      "Savestates = " + from + "/sstates\n"
      "MemoryCards = /sdcard/Android/data/com.armsx2/files/memcards\n"
      "Cache = " + from + "/cache\n"
      "Logs = " + from + "/logs\n"
      "Cheats = /storage/emulated/0/PS2/cheats\n"
      "[Hotkeys]\nX = " + from + "/x\n";
  EXPECT_EQ(
      "[EmuCore]\nSavestates = " + from + "/sstates\n"
      "[Folders]\n"
      "Bios = " + bios + "\n"
      "Snapshots = snaps\n"
      "Savestates = " + to + "/sstates\n"
      "MemoryCards = " + to + "/memcards\n"
      "Cache = " + from + "/cache\n"
      "Logs = " + from + "/logs\n"
      "Cheats = /storage/emulated/0/PS2/cheats\n"
      "[Hotkeys]\nX = " + from + "/x\n",
      armsx2RewriteIni(ini, from, to, bios));
  // A relative Bios would look on the card, where no BIOS goes.
  EXPECT_EQ("[Folders]\nBios = " + bios + "\n", armsx2RewriteIni("[Folders]\nBios = bios\n", from, to, bios));
  // A BIOS folder of the user's own stays theirs.
  EXPECT_EQ("[Folders]\nBios = /storage/emulated/0/BIOS\n",
            armsx2RewriteIni("[Folders]\nBios = /storage/emulated/0/BIOS\n", from, to, bios));
}
