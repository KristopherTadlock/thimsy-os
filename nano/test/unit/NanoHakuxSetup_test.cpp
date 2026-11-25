// hakuX's games folders kept to Game Settings (NanoHakuxSetup): which folders the Xbox system's scan
// candidates make, what hakuX's settings become, and the whole sync on a scratch root.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include "NanoHakuxSetup.h"
#include "nano_test_fs.h"

using namespace android::nanohakux;
using nanotest::TempDir;

namespace {

const char kHeader[] = "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n";
const char kTree[] = "content://com.android.externalstorage.documents/tree/";
const std::string kInternal = std::string(kTree) + "primary%3AROMs%2Fxbox";
const std::string kCard = std::string(kTree) + "1A2B-3C4D%3AROMs%2Fxbox";
const std::string kMine = std::string(kTree) + "primary%3AGames%2FXbox";   // picked in hakuX

void mkdirs(const std::string& p) {
  for (size_t i = 1; i <= p.size(); i++)
    if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0775);
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

std::string prefsWith(const std::string& body) { return std::string(kHeader) + "<map>\n" + body + "</map>\n"; }

// buildScanCandidates' list for the Xbox system in default-scan mode, with a card in when card.
std::vector<std::string> defaultCandidates(bool card) {
  std::vector<std::string> c = {"/data/media/0/ROMs/xbox", "/sdcard/ROMs/xbox", "/storage/emulated/0/ROMs/xbox"};
  if (card) {
    for (const char* base : {"/storage/1A2B-3C4D", "/mnt/media_rw/1A2B-3C4D"}) {
      c.push_back(std::string(base) + "/ROMs/xbox");
      c.push_back(std::string(base) + "/roms/xbox");
      c.push_back(std::string(base) + "/xbox");
    }
  }
  return c;
}

Settings defaults(bool card) {
  Settings s;
  s.candidates = defaultCandidates(card);
  s.defaultScan = true;
  s.makeable = {"/data/media/0/ROMs/xbox"};
  return s;
}

struct Drives {
  bool card = false;          // mounted
  std::set<std::string> dirs;  // folders there
  std::function<bool(const std::string&)> mounted() const {
    return [this](const std::string& v) { return v == "primary" || (card && v == "1A2B-3C4D"); };
  }
  std::function<bool(const std::string&)> exists() const {
    return [this](const std::string& p) { return dirs.count(p) > 0; };
  }
};

std::vector<std::string> uris(const std::vector<Folder>& v) {
  std::vector<std::string> out;
  for (const auto& f : v) out.push_back(folderUri(f));
  return out;
}

}  // namespace

TEST(HakuxSetup, FolderOf) {
  Folder f;
  ASSERT_TRUE(folderOf("/data/media/0/ROMs/xbox", &f));
  EXPECT_EQ("primary", f.volume);
  EXPECT_EQ("ROMs/xbox", f.rel);
  for (const char* p : {"/sdcard/ROMs/xbox/", "/storage/emulated/0/roms/xbox", "/storage/self/primary/ROMs/xbox"}) {
    ASSERT_TRUE(folderOf(p, &f)) << p;
    EXPECT_EQ((Folder{"primary", "ROMs/xbox"}), f) << p;
  }
  for (const char* p : {"/storage/1A2B-3C4D/ROMs/Xbox", "/mnt/media_rw/1A2B-3C4D/roms/Xbox"}) {
    ASSERT_TRUE(folderOf(p, &f)) << p;
    EXPECT_EQ((Folder{"1A2B-3C4D", "ROMs/Xbox"}), f) << p;
  }
  for (const char* p : {"/storage/1A2B-3C4D/xbox", "/data/media/0/ROMs", "/data/media/0/ROMs/xbox/saves",
                        "/data/media/0/Games/xbox", "/storage/myshare/ROMs/xbox", "/storage/emulated/ROMs/xbox",
                        "/mnt/shares/nas/ROMs/xbox", "/data/media/0/ROMs/x.box", "/data/media/0/ROMs/..",
                        "relative/ROMs/xbox", ""})
    EXPECT_FALSE(folderOf(p, &f)) << p;
}

TEST(HakuxSetup, FolderTooLongToGrantIsLeftOut) {
  Folder f;
  const std::string n32(32, 'a'), n33(33, 'a'), n30(30, 'b'), n31(31, 'b');
  EXPECT_TRUE(folderOf("/data/media/0/ROMs/" + n32, &f));
  EXPECT_FALSE(folderOf("/data/media/0/ROMs/" + n33, &f));
  EXPECT_TRUE(folderOf("/storage/1A2B-3C4D/ROMs/" + n30, &f));
  EXPECT_FALSE(folderOf("/storage/1A2B-3C4D/ROMs/" + n31, &f));
  // Only that folder: the others are still given.
  Drives d;
  Settings s;
  s.candidates = {"/data/media/0/ROMs/Microsoft_Xbox_Original_Games_Collection", "/data/media/0/ROMs/xbox"};
  d.dirs = {s.candidates[0], s.candidates[1]};
  EXPECT_EQ(std::vector<std::string>{kInternal}, uris(resolveFolders(s, {}, d.mounted(), d.exists())));
}

TEST(HakuxSetup, Uris) {
  EXPECT_EQ(kInternal, folderUri({"primary", "ROMs/xbox"}));
  EXPECT_EQ(kCard, folderUri({"1A2B-3C4D", "ROMs/xbox"}));
  Folder f;
  ASSERT_TRUE(uriFolder(kCard, &f));
  EXPECT_EQ((Folder{"1A2B-3C4D", "ROMs/xbox"}), f);
  EXPECT_FALSE(uriFolder(kMine, &f));                                  // not a ROM folder
  EXPECT_FALSE(uriFolder(std::string(kTree) + "primary:ROMs/xbox", &f));   // not as Android writes it
  EXPECT_FALSE(uriFolder(kInternal + "/document/primary%3AROMs%2Fxbox%2Fa.iso", &f));
}

TEST(HakuxSetup, ResolveInternalOnly) {
  Drives d;
  EXPECT_EQ(std::vector<std::string>{kInternal}, uris(resolveFolders(defaults(false), {}, d.mounted(), d.exists())));
}

TEST(HakuxSetup, ResolveInternalAndCard) {
  Drives d;
  d.card = true;
  d.dirs = {"/storage/1A2B-3C4D/ROMs/xbox", "/mnt/media_rw/1A2B-3C4D/ROMs/xbox", "/data/media/0/ROMs/xbox"};
  const auto got = uris(resolveFolders(defaults(true), {}, d.mounted(), d.exists()));
  EXPECT_EQ((std::vector<std::string>{kInternal, kCard}), got);
  // A card without the folder: Nano makes system folders on internal storage only.
  d.dirs = {};
  EXPECT_EQ(std::vector<std::string>{kInternal}, uris(resolveFolders(defaults(true), {}, d.mounted(), d.exists())));
}

TEST(HakuxSetup, ResolveCardRemovedFromSettings) {
  Drives d;
  d.card = true;
  d.dirs = {"/storage/1A2B-3C4D/ROMs/xbox"};
  const std::vector<Folder> before = {{"primary", "ROMs/xbox"}, {"1A2B-3C4D", "ROMs/xbox"}};
  Settings chosen;   // the user's own folders in Game Settings: internal storage's only
  chosen.candidates = {"/storage/emulated/0/ROMs/xbox"};
  EXPECT_EQ(std::vector<std::string>{kInternal}, uris(resolveFolders(chosen, before, d.mounted(), d.exists())));
  d.card = false;   // out as well: still not named
  EXPECT_EQ(std::vector<std::string>{kInternal}, uris(resolveFolders(chosen, before, d.mounted(), d.exists())));
  Settings none;    // Xbox no longer plays hakuX
  EXPECT_TRUE(resolveFolders(none, before, d.mounted(), d.exists()).empty());
}

TEST(HakuxSetup, ResolveCardTemporarilyOut) {
  Drives d;   // no card mounted: its folders are not candidates
  const std::vector<Folder> before = {{"primary", "ROMs/xbox"}, {"1A2B-3C4D", "ROMs/xbox"}};
  EXPECT_EQ((std::vector<std::string>{kInternal, kCard}), uris(resolveFolders(defaults(false), before, d.mounted(), d.exists())));
  // A chosen card folder stays listed while the card is out.
  Settings chosen;
  chosen.candidates = {"/storage/1A2B-3C4D/ROMs/xbox"};
  EXPECT_EQ(std::vector<std::string>{kCard}, uris(resolveFolders(chosen, before, d.mounted(), d.exists())));
}

TEST(HakuxSetup, ResolveOrder) {
  Drives d;
  d.card = true;
  Settings s;
  s.candidates = {"/storage/1A2B-3C4D/ROMs/xbox", "/data/media/0/ROMs/xboxgames", "/data/media/0/ROMs/xbox"};
  d.dirs = {s.candidates[0], s.candidates[1], s.candidates[2]};
  EXPECT_EQ((std::vector<std::string>{std::string(kTree) + "primary%3AROMs%2Fxboxgames", kInternal, kCard}),
            uris(resolveFolders(s, {}, d.mounted(), d.exists())));
}

TEST(HakuxSetup, PrefsTextAsAndroidWritesIt) {
  const std::string prefs = prefsWith("    <string name=\"gamesFolders\">a&amp;b&#10;c&lt;d&gt;&quot;&#x9;e</string>\n");
  EXPECT_EQ("a&b\nc<d>\"\te", prefsText(prefs, "gamesFolders"));
  EXPECT_EQ((std::vector<std::string>{"a&b", "c<d>\"\te"}), prefsLines(prefs, "gamesFolders"));
  EXPECT_EQ("", prefsText(prefs, "gamesFolderUri"));
  std::string out;
  ASSERT_TRUE(prefsSetText(&out, "gamesFolders", "a&b\nc<d>\"\te"));
  EXPECT_EQ(prefsWith("    <string name=\"gamesFolders\">a&amp;b&#10;c&lt;d&gt;&quot;&#9;e</string>\n"), out);
  EXPECT_EQ("a&b\nc<d>\"\te", prefsText(out, "gamesFolders"));
}

TEST(HakuxSetup, PlanForAFreshInstall) {
  const Plan p = planSync("", {kInternal});
  ASSERT_TRUE(p.write);
  EXPECT_EQ(std::vector<std::string>{kInternal}, p.grant);
  EXPECT_TRUE(p.revoke.empty());
  EXPECT_EQ(prefsWith("    <string name=\"gamesFolders\">" + kInternal + "</string>\n"
                      "    <string name=\"gamesFolderUri\">" + kInternal + "</string>\n"
                      "    <boolean name=\"setup_complete\" value=\"true\" />\n"
                      "    <string name=\"thimsy_folders\">" + kInternal + "</string>\n"),
            p.prefs);
  EXPECT_FALSE(planSync(p.prefs, {kInternal}).write);   // then nothing
}

TEST(HakuxSetup, PlanAddsACardAndKeepsTheOtherKeys) {
  const std::string body = "    <string name=\"app_theme\">dark</string>\n"
                           "    <string name=\"gamesFolders\">" + kInternal + "</string>\n"
                           "    <string name=\"gamesFolderUri\">" + kInternal + "</string>\n"
                           "    <int name=\"render_scale\" value=\"2\" />\n"
                           "    <boolean name=\"setup_complete\" value=\"true\" />\n"
                           "    <string name=\"thimsy_folders\">" + kInternal + "</string>\n";
  const Plan p = planSync(prefsWith(body), {kInternal, kCard});
  ASSERT_TRUE(p.write);
  EXPECT_EQ(std::vector<std::string>{kCard}, p.grant);
  EXPECT_EQ(prefsWith("    <string name=\"app_theme\">dark</string>\n"
                      "    <string name=\"gamesFolders\">" + kInternal + "&#10;" + kCard + "</string>\n"
                      "    <string name=\"gamesFolderUri\">" + kInternal + "</string>\n"
                      "    <int name=\"render_scale\" value=\"2\" />\n"
                      "    <boolean name=\"setup_complete\" value=\"true\" />\n"
                      "    <string name=\"thimsy_folders\">" + kInternal + "&#10;" + kCard + "</string>\n"),
            p.prefs);
}

TEST(HakuxSetup, PlanKeepsTheUsersFolderAfterThimsys) {
  // The user's folder from hakuX's own setup, before Thimsy managed any.
  const std::string mine = prefsWith("    <string name=\"gamesFolders\">" + kMine + "</string>\n"
                                     "    <string name=\"gamesFolderUri\">" + kMine + "</string>\n"
                                     "    <boolean name=\"setup_complete\" value=\"true\" />\n");
  Plan p = planSync(mine, {kInternal});
  ASSERT_TRUE(p.write);
  EXPECT_EQ((std::vector<std::string>{kInternal, kMine}), prefsLines(p.prefs, "gamesFolders"));
  EXPECT_EQ(kInternal, prefsText(p.prefs, "gamesFolderUri"));
  // A card taken out of the settings goes; the user's folder stays.
  const std::string before = prefsWith("    <string name=\"gamesFolders\">" + kInternal + "&#10;" + kCard + "&#10;" + kMine +
                                       "</string>\n    <string name=\"gamesFolderUri\">" + kInternal + "</string>\n"
                                       "    <boolean name=\"setup_complete\" value=\"true\" />\n"
                                       "    <string name=\"thimsy_folders\">" + kInternal + "&#10;" + kCard + "</string>\n");
  p = planSync(before, {kInternal});
  ASSERT_TRUE(p.write);
  EXPECT_TRUE(p.grant.empty());
  EXPECT_EQ(std::vector<std::string>{kCard}, p.revoke);
  EXPECT_EQ((std::vector<std::string>{kInternal, kMine}), prefsLines(p.prefs, "gamesFolders"));
  EXPECT_EQ(std::vector<std::string>{kInternal}, prefsLines(p.prefs, kThimsyKey));
  // None left of Thimsy's: the user's is the first.
  p = planSync(before, {});
  ASSERT_TRUE(p.write);
  EXPECT_EQ(std::vector<std::string>{kMine}, prefsLines(p.prefs, "gamesFolders"));
  EXPECT_EQ(kMine, prefsText(p.prefs, "gamesFolderUri"));
  EXPECT_EQ((std::vector<std::string>{kInternal, kCard}), p.revoke);
}

TEST(HakuxSetup, PlanNeverTakesAFolderTheUserPicked) {
  // The user picked ROMs/xbox in hakuX before Thimsy named it: listed first, but still the user's.
  const std::string mine = prefsWith("    <string name=\"gamesFolders\">" + kInternal + "</string>\n"
                                     "    <string name=\"gamesFolderUri\">" + kInternal + "</string>\n"
                                     "    <boolean name=\"setup_complete\" value=\"true\" />\n");
  Plan p = planSync(mine, {kInternal});
  EXPECT_TRUE(p.grant.empty());
  EXPECT_TRUE(prefsLines(p.write ? p.prefs : mine, kThimsyKey).empty());
  if (p.write) EXPECT_EQ(std::vector<std::string>{kInternal}, prefsLines(p.prefs, "gamesFolders"));
  // Once Xbox no longer names it, it stays, and its grant isn't taken back.
  p = planSync(mine, {});
  EXPECT_TRUE(p.revoke.empty());
  if (p.write) EXPECT_EQ(std::vector<std::string>{kInternal}, prefsLines(p.prefs, "gamesFolders"));
  // And nothing is rewritten while it stays so.
  EXPECT_FALSE(planSync(mine, {kInternal}).write);
}

TEST(HakuxSetup, PlanGivesAgainAFolderHakuxDropped) {
  // hakuX forgets a folder it can no longer read (AppPrefs.dropStale): given again.
  const std::string prefs = prefsWith("    <boolean name=\"setup_complete\" value=\"false\" />\n"
                                      "    <string name=\"thimsy_folders\">" + kInternal + "</string>\n");
  const Plan p = planSync(prefs, {kInternal});
  ASSERT_TRUE(p.write);
  EXPECT_EQ(std::vector<std::string>{kInternal}, p.grant);
  EXPECT_NE(std::string::npos, p.prefs.find("<boolean name=\"setup_complete\" value=\"true\" />"));
}

TEST(HakuxSetup, PlanNothingToDo) {
  EXPECT_FALSE(planSync("", {}).write);   // no folder, no settings file: none made
  const std::string mine = prefsWith("    <string name=\"gamesFolders\">" + kMine + "</string>\n"
                                     "    <string name=\"gamesFolderUri\">" + kMine + "</string>\n");
  EXPECT_FALSE(planSync(mine, {}).write);
}

TEST(HakuxSetup, PackageRunning) {
  TempDir proc{"hakux_proc"};
  auto process = [&](const char* pid, const std::string& cmdline) {
    mkdirs(proc / pid);
    put(proc / pid + "/cmdline", cmdline);
  };
  process("1", std::string("/system/bin/init\0second_stage\0", 30));
  process("200", std::string("com.rfandango.haku_x_other\0", 27));
  process("300", std::string("com.rfandango.haku\0", 19));
  mkdirs(proc / "self");
  EXPECT_FALSE(packageRunning(kPkg, proc.path()));
  process("400", std::string("com.rfandango.haku_x:core\0", 26));
  EXPECT_TRUE(packageRunning(kPkg, proc.path()));
  EXPECT_FALSE(packageRunning(kPkg, proc.path() + "/missing"));
}

class HakuxSync : public ::testing::Test {
protected:
  void SetUp() override {
    char buf[PATH_MAX];
    root = realpath(tmp.path().c_str(), buf) ? std::string(buf) : tmp.path();
    mkdirs(root + "/data/system");
    put(root + "/data/system/packages.list",
        "com.rfandango.haku_x 10123 0 /data/user/0/com.rfandango.haku_x default:targetSdkVersion=35 3003\n");
    mkdirs(root + "/data/data/com.rfandango.haku_x");
    mkdirs(root + "/data/media/0/ROMs/nes");
    chmod((root + "/data/media/0/ROMs").c_str(), 02771);
    env.root = root;
    env.read = [this](const std::string& p, std::string* out) {
      reads++;
      return android::nanobios::appPrefsRead(p, out, root);
    };
    env.write = [this](const std::string& p, const std::string& text) {
      writes++;
      return android::nanobios::appFileWrite(p, text, nullptr, root);
    };
    env.grant = [this](const Folder& f) {
      granted.push_back(folderUri(f));
      if (startDuringGrant) running = true;
      return grantOk && (grantsOk < 0 || (int)granted.size() <= grantsOk);
    };
    env.revoke = [this](const Folder& f) {
      revoked.push_back(folderUri(f));
      return true;
    };
    env.running = [this](const std::string& pkg) { return running && pkg == kPkg; };
    env.mounted = [this](const std::string& v) { return v == "primary" || (card && v == "1A2B-3C4D"); };
    env.exists = [this](const std::string& p) {
      struct stat st;
      const std::string raw = p.compare(0, 9, "/storage/") == 0 ? "/mnt/media_rw/" + p.substr(9) : p;
      return stat((root + raw).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    };
  }
  std::string prefs() const { return root + kPrefs; }

  TempDir tmp{"hakux"};
  std::string root;
  Env env;
  std::vector<std::string> granted, revoked;
  int reads = 0, writes = 0;
  bool grantOk = true, running = false, startDuringGrant = false, card = false;
  int grantsOk = -1;   // grants that succeed before the rest fail; -1: all (as grantOk says)
};

TEST_F(HakuxSync, FreshInstallGetsTheInternalFolder) {
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  EXPECT_EQ(std::vector<std::string>{kInternal}, granted);
  struct stat rom, roms;
  ASSERT_EQ(0, stat((root + "/data/media/0/ROMs/xbox").c_str(), &rom));
  ASSERT_EQ(0, stat((root + "/data/media/0/ROMs").c_str(), &roms));
  EXPECT_EQ(roms.st_mode & 07777, rom.st_mode & 07777);   // as the ROM folders are
  EXPECT_EQ(roms.st_uid, rom.st_uid);
  EXPECT_EQ(std::vector<std::string>{kInternal}, prefsLines(get(prefs()), "gamesFolders"));
  EXPECT_EQ(1, writes);
}

TEST_F(HakuxSync, NoChangeNoWrites) {
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  const std::string text = get(prefs());
  granted.clear();
  writes = 0;
  running = true;   // a check that writes nothing needs no wait for hakuX
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  EXPECT_EQ(0, writes);
  EXPECT_TRUE(granted.empty());
  EXPECT_EQ(text, get(prefs()));
}

TEST_F(HakuxSync, FollowsTheCard) {
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  card = true;
  mkdirs(root + "/mnt/media_rw/1A2B-3C4D/ROMs/xbox");
  granted.clear();
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(true)));
  EXPECT_EQ(std::vector<std::string>{kCard}, granted);
  EXPECT_EQ((std::vector<std::string>{kInternal, kCard}), prefsLines(get(prefs()), "gamesFolders"));
  // Out for a moment: nothing changes.
  card = false;
  writes = 0;
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  EXPECT_EQ(0, writes);
  // Taken out of the settings: dropped and taken back.
  Settings chosen;
  chosen.candidates = {"/storage/emulated/0/ROMs/xbox"};
  ASSERT_EQ(Outcome::kDone, sync(env, chosen));
  EXPECT_EQ(std::vector<std::string>{kCard}, revoked);
  EXPECT_EQ(std::vector<std::string>{kInternal}, prefsLines(get(prefs()), "gamesFolders"));
}

TEST_F(HakuxSync, KeepsTheUsersFolderAndSettings) {
  mkdirs(root + "/data/data/com.rfandango.haku_x/shared_prefs");
  put(prefs(), prefsWith("    <string name=\"app_theme\">dark</string>\n"
                         "    <string name=\"gamesFolders\">" + kMine + "</string>\n"
                         "    <string name=\"gamesFolderUri\">" + kMine + "</string>\n"
                         "    <boolean name=\"setup_complete\" value=\"true\" />\n"));
  chmod(prefs().c_str(), 0660);
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  const std::string text = get(prefs());
  EXPECT_EQ((std::vector<std::string>{kInternal, kMine}), prefsLines(text, "gamesFolders"));
  EXPECT_NE(std::string::npos, text.find("<string name=\"app_theme\">dark</string>"));
  struct stat st;
  ASSERT_EQ(0, stat(prefs().c_str(), &st));
  EXPECT_EQ(0660u, st.st_mode & 0777);
}

TEST_F(HakuxSync, WaitsWhileItRuns) {
  running = true;
  EXPECT_EQ(Outcome::kWait, sync(env, defaults(false)));
  EXPECT_TRUE(granted.empty());
  EXPECT_EQ("<none>", get(prefs()));
  running = false;
  startDuringGrant = true;
  EXPECT_EQ(Outcome::kWait, sync(env, defaults(false)));
  EXPECT_EQ("<none>", get(prefs()));
  EXPECT_EQ(std::vector<std::string>{kInternal}, revoked);   // given, then taken back
}

TEST_F(HakuxSync, RemakesADeletedFolder) {
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  ASSERT_EQ(0, rmdir((root + "/data/media/0/ROMs/xbox").c_str()));
  writes = 0;
  granted.clear();
  ASSERT_EQ(Outcome::kDone, sync(env, defaults(false)));
  struct stat st;
  EXPECT_EQ(0, stat((root + "/data/media/0/ROMs/xbox").c_str(), &st));
  EXPECT_EQ(0, writes);            // the settings already name it
  EXPECT_TRUE(granted.empty());    // and hakuX keeps its grant
}

TEST_F(HakuxSync, GrantsGivenBeforeAFailureAreTakenBack) {
  card = true;
  mkdirs(root + "/mnt/media_rw/1A2B-3C4D/ROMs/xbox");
  grantsOk = 1;   // the internal folder is given, the card's isn't
  EXPECT_EQ(Outcome::kFailed, sync(env, defaults(true)));
  EXPECT_EQ((std::vector<std::string>{kInternal, kCard}), granted);
  EXPECT_EQ(std::vector<std::string>{kInternal}, revoked);
  EXPECT_EQ("<none>", get(prefs()));
}

TEST_F(HakuxSync, NoSettingsWithoutTheGrant) {
  grantOk = false;
  EXPECT_EQ(Outcome::kFailed, sync(env, defaults(false)));
  EXPECT_EQ("<none>", get(prefs()));
}

TEST_F(HakuxSync, NotInstalled) {
  put(root + "/data/system/packages.list", "com.android.shell 2000 0 /data/user_de/0/com.android.shell platform 3003\n");
  EXPECT_EQ(Outcome::kNotInstalled, sync(env, defaults(false)));
  unlink((root + "/data/system/packages.list").c_str());
  EXPECT_EQ(Outcome::kNotInstalled, sync(env, defaults(false)));
  EXPECT_TRUE(granted.empty());
  EXPECT_EQ(0, reads);
}

TEST_F(HakuxSync, NeverThroughALinkedSettingsFile) {
  mkdirs(root + "/data/data/com.rfandango.haku_x/shared_prefs");
  put(root + "/data/system/secret.conf", "psk=hunter2\n");
  ASSERT_EQ(0, symlink((root + "/data/system/secret.conf").c_str(), prefs().c_str()));
  EXPECT_EQ(Outcome::kWait, sync(env, defaults(false)));
  EXPECT_TRUE(granted.empty());
  EXPECT_EQ("psk=hunter2\n", get(root + "/data/system/secret.conf"));
}
