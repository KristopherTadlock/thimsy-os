// The BIOS installer's path, ownership and space helpers, and root's reads and writes of an app's
// settings files (never through a link the app controls).

#include <gtest/gtest.h>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "NanoBiosUtil.h"
#include "nano_test_fs.h"

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
  const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0) << p;
  ASSERT_EQ((ssize_t)text.size(), write(fd, text.data(), text.size()));
  close(fd);
}

std::string get(const std::string& p) {
  std::string out;
  const int fd = open(p.c_str(), O_RDONLY);
  if (fd < 0) return "<none>";
  char buf[4096];
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) out.append(buf, (size_t)n);
  close(fd);
  return out;
}

int runCount = 0;
int fakeRun(const std::vector<std::string>&, std::string*, int) { runCount++; return 0; }

}  // namespace

TEST(BiosUtil, SafePath) {
  EXPECT_TRUE(safePath("/data/media/0/RetroArch/system"));
  EXPECT_TRUE(safePath("/data/data/com.dsemu.drastic/shared_prefs/_Dra$t1c_FS$_.xml"));
  EXPECT_TRUE(safePath("/a/..b/c."));
  EXPECT_FALSE(safePath(""));
  EXPECT_FALSE(safePath("relative"));
  EXPECT_FALSE(safePath("/a/../b"));
  EXPECT_FALSE(safePath("/a/./b"));
  EXPECT_FALSE(safePath("/a/.."));
  // Nothing reaches a shell: names people give folders are fine.
  EXPECT_TRUE(safePath("/data/media/0/Bob's Games (2) & more/RetroArch"));
  EXPECT_FALSE(safePath("/a\tb"));
  EXPECT_FALSE(safePath(std::string("/a\0b", 4)));
  EXPECT_FALSE(safePath("/a\nb"));
  EXPECT_FALSE(safePath("/a\rb"));
}

TEST(BiosUtil, UnderAndPkgAfter) {
  EXPECT_TRUE(under("/data/media/0/x", "/data/media/0"));
  EXPECT_FALSE(under("/data/media/0", "/data/media/0"));
  EXPECT_FALSE(under("/data/media/00/x", "/data/media/0"));
  EXPECT_EQ("com.armsx2", pkgAfter("/data/data/com.armsx2/files/bios", "/data/data"));
  EXPECT_EQ("com.armsx2", pkgAfter("/data/data/com.armsx2", "/data/data"));
}

TEST(BiosUtil, OwnerFor) {
  EXPECT_EQ(OWN_APPEXT, ownerFor("/data/media/0/Android/data/com.armsx2/files/bios/x.bin", "com.armsx2").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/media/0/Android/data/org.ppsspp.ppsspp/files/x", "com.armsx2").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/media/0/Android/data", "com.armsx2").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/media/0/Android/obb/com.armsx2/x", "com.armsx2").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/media/0/Android/data/x/y", "").kind);
  EXPECT_EQ(OWN_APPDATA, ownerFor("/data/data/org.scummvm.scummvm/files/extra/MT32_CONTROL.ROM", "org.scummvm.scummvm").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/data/org.scummvm.scummvm/files/extra/x", "com.armsx2").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/data", "com.armsx2").kind);
  EXPECT_EQ(OWN_SHARED, ownerFor("/data/media/0/RetroArch/system/x.bin", "com.retroarch.aarch64").kind);
  EXPECT_EQ(OWN_SYSTEM, ownerFor("/data/system/drastic-nano/DraStic/system/x", "").kind);
  EXPECT_EQ(OWN_NONE, ownerFor("/mnt/media_rw/1234-ABCD/RetroArch/system/x", "com.retroarch.aarch64").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/system/packages.list", "").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/misc/wifi/x", "").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/system/etc/x", "").kind);
  // Under a root, the same places count only there.
  EXPECT_EQ(OWN_APPEXT, ownerFor("/r/data/media/0/Android/data/com.armsx2/files/x", "com.armsx2", "/r").kind);
  EXPECT_EQ(OWN_REFUSED, ownerFor("/data/media/0/Android/data/com.armsx2/files/x", "com.armsx2", "/r").kind);
  EXPECT_EQ(OWN_NONE, ownerFor("/r/mnt/media_rw/X/y", "", "/r").kind);
}

TEST(BiosUtil, RoundUpAndAdmit) {
  EXPECT_EQ(0u, roundUp(0, 4096));
  EXPECT_EQ(4096u, roundUp(1, 4096));
  EXPECT_EQ(4096u, roundUp(4096, 4096));
  EXPECT_EQ(8192u, roundUp(4097, 4096));
  EXPECT_EQ(5u, roundUp(5, 0));
  EXPECT_EQ(32768u, roundUp(512, 32768));   // a 32 KB cluster card

  const uint64_t spare = 8ull << 20;
  uint64_t admitted = 0;
  EXPECT_TRUE(admit(&admitted, 4096, spare, spare + 8192));
  EXPECT_TRUE(admit(&admitted, 4096, spare, spare + 8192));
  EXPECT_FALSE(admit(&admitted, 1, spare, spare + 8192));   // the spare stays free
  EXPECT_EQ(8192u, admitted);
  uint64_t a2 = 0;
  EXPECT_FALSE(admit(&a2, 4096, spare, spare));
  EXPECT_FALSE(admit(&a2, 1, spare, 100));                   // less free than the spare
  EXPECT_FALSE(admit(&a2, UINT64_MAX - 10, spare, UINT64_MAX - 5));   // never wraps into "fits"
  uint64_t a3 = UINT64_MAX - spare - 10;
  EXPECT_FALSE(admit(&a3, 20, spare, UINT64_MAX));
  EXPECT_TRUE(admit(&a3, 10, spare, UINT64_MAX));
}

TEST(BiosUtil, Views) {
  EXPECT_EQ("/storage/emulated/0/RetroArch", appView("/data/media/0/RetroArch"));
  EXPECT_EQ("/storage/1234-ABCD/bios", appView("/mnt/media_rw/1234-ABCD/bios"));
  EXPECT_EQ("/data/data/x", appView("/data/data/x"));
  EXPECT_EQ("/data/media/0/RetroArch", rawView("/storage/emulated/0/RetroArch"));
  EXPECT_EQ("/data/media/0/RetroArch", rawView("/sdcard/RetroArch"));
  EXPECT_EQ("/data/media/0", rawView("/storage/self/primary"));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD/bios", rawView("/storage/1234-ABCD/bios"));
  EXPECT_EQ("/data/data/x/y", rawView("/data/user/0/x/y"));
  EXPECT_EQ("/sdcardx/y", rawView("/sdcardx/y"));
}

TEST(BiosUtil, IniMain) {
  EXPECT_EQ("", iniMainGet("[scummvm]\nsavepath=/x\n", "extrapath"));
  EXPECT_EQ("/storage/emulated/0/extra", iniMainGet("[ScummVM]\r\nExtraPath = /sdcard/extra/\r\n", "extrapath"));
  EXPECT_EQ("", iniMainGet("[monkey]\nextrapath=/x\n", "extrapath"));
  EXPECT_EQ("[scummvm]\nsavepath=/x\nextrapath=/e\n[g]\n", iniMainSet("[scummvm]\nsavepath=/x\n[g]\n", "extrapath", "/e"));
  EXPECT_EQ("[scummvm]\nextrapath=/e\n[g]\na=1", iniMainSet("[g]\na=1", "extrapath", "/e"));
  EXPECT_EQ("[scummvm]\nextrapath=/e\n", iniMainSet("", "extrapath", "/e"));
}

TEST(BiosUtil, PrefsBool) {
  std::string p;
  ASSERT_TRUE(prefsSetBool(&p, "initial_setup_completed", true));
  EXPECT_EQ(1, prefsGetBool(p, "initial_setup_completed"));
  ASSERT_TRUE(prefsSetBool(&p, "initial_setup_completed", false));
  EXPECT_EQ(0, prefsGetBool(p, "initial_setup_completed"));
  prefsRemoveBool(&p, "initial_setup_completed");
  EXPECT_EQ(-1, prefsGetBool(p, "initial_setup_completed"));
}

// ---- an app's settings files ----

class AppFiles : public ::testing::Test {
protected:
  void SetUp() override {
    root = realDir(tmp);
    prefsDir = root + "/data/data/com.armsx2/shared_prefs";
    mkdirs(prefsDir);
    mkdirs(root + "/data/system");
    put(root + "/data/system/secret.conf", "psk=hunter2\n");
    prefs = prefsDir + "/ARMSX2.xml";
  }
  TempDir tmp{"appfiles"};
  std::string root, prefsDir, prefs;
};

TEST_F(AppFiles, ReadsAndWritesAPlainFile) {
  put(prefs, "<map />\n");
  chmod(prefs.c_str(), 0640);
  put(prefs + ".bak", "<map><string name=\"bios\">old</string></map>\n");
  std::string text;
  ASSERT_TRUE(appPrefsRead(prefs, &text, root));   // its backup when there is one
  EXPECT_NE(std::string::npos, text.find("old"));
  ASSERT_TRUE(appFileRead(prefs, &text, root));
  EXPECT_EQ("<map />\n", text);
  runCount = 0;
  ASSERT_TRUE(appFileWrite(prefs, "<map>new</map>\n", fakeRun, root));
  EXPECT_EQ("<map>new</map>\n", get(prefs));
  EXPECT_EQ("<none>", get(prefs + ".bak"));   // SharedPreferences would restore it over the new file
  EXPECT_EQ("<none>", get(prefs + ".nanotmp"));
  struct stat st;
  ASSERT_EQ(0, stat(prefs.c_str(), &st));
  EXPECT_EQ(0640u, st.st_mode & 0777);   // the old file's mode
  EXPECT_EQ(1, runCount);                // restorecon
}

TEST_F(AppFiles, MakesTheFolderAnAppNeverMade) {
  const std::string ini = root + "/data/data/org.scummvm.scummvm/files/scummvm.ini";
  mkdirs(root + "/data/data/org.scummvm.scummvm");
  ASSERT_TRUE(appFileWrite(ini, "[scummvm]\n", fakeRun, root));
  EXPECT_EQ("[scummvm]\n", get(ini));
}

TEST_F(AppFiles, NeverReadsOrWritesThroughALinkAtTheEnd) {
  ASSERT_EQ(0, symlink((root + "/data/system/secret.conf").c_str(), prefs.c_str()));
  std::string text;
  EXPECT_FALSE(appFileRead(prefs, &text, root));
  EXPECT_FALSE(appPrefsRead(prefs, &text, root));
  EXPECT_FALSE(appFileWrite(prefs, "pwned", fakeRun, root));
  EXPECT_EQ("psk=hunter2\n", get(root + "/data/system/secret.conf"));
  struct stat st;
  ASSERT_EQ(0, lstat(prefs.c_str(), &st));
  EXPECT_TRUE(S_ISLNK(st.st_mode));   // left as it was
}

TEST_F(AppFiles, NeverReadsOrWritesThroughALinkedFolder) {
  // shared_prefs swapped for a link to a system folder holding a file of the same name.
  put(root + "/data/system/ARMSX2.xml", "system's\n");
  ASSERT_EQ(0, rename(prefsDir.c_str(), (prefsDir + ".old").c_str()));
  ASSERT_EQ(0, symlink((root + "/data/system").c_str(), prefsDir.c_str()));
  std::string text;
  EXPECT_FALSE(appFileRead(prefs, &text, root));
  EXPECT_FALSE(appFileWrite(prefs, "pwned", fakeRun, root));
  EXPECT_EQ("system's\n", get(root + "/data/system/ARMSX2.xml"));
  EXPECT_EQ("<none>", get(root + "/data/system/ARMSX2.xml.nanotmp"));
}

TEST_F(AppFiles, RefusesWhenTheAppFolderItselfIsALink) {
  const std::string other = root + "/data/data/com.flycast.emulator";
  ASSERT_EQ(0, symlink((root + "/data/system").c_str(), other.c_str()));
  std::string text;
  EXPECT_EQ(-2, appOwnerUid(other + "/x", root));
  EXPECT_FALSE(appFileRead(other + "/secret.conf", &text, root));
  EXPECT_FALSE(appFileWrite(other + "/secret.conf", "pwned", fakeRun, root));
  EXPECT_EQ("psk=hunter2\n", get(root + "/data/system/secret.conf"));
}

TEST_F(AppFiles, NeverBlocksOnAFifo) {
  ASSERT_EQ(0, mkfifo(prefs.c_str(), 0644));
  std::string text;
  EXPECT_FALSE(appFileRead(prefs, &text, root));   // would hang in open() without O_NONBLOCK
  EXPECT_FALSE(appFileWrite(prefs, "x", fakeRun, root));
}

TEST_F(AppFiles, OwnerIsTheAppFolders) {
  struct stat st;
  ASSERT_EQ(0, stat((root + "/data/data/com.armsx2").c_str(), &st));
  EXPECT_EQ((int)st.st_uid, appOwnerUid(prefs, root));
  EXPECT_EQ(-1, appOwnerUid(root + "/data/media/0/RetroArch/retroarch.cfg", root));
}
