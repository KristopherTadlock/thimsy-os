// RetroArch's config rewrite when its folder moves to the microSD card (Move Emulator Files), and the
// file it leaves: RetroArch must still be able to read and save it.

#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "NanoHotkeys.h"
#include "NanoJson.h"
#include "NanoRaConfig.h"
#include "nano_test_fs.h"
#include "nano_test_util.h"

using namespace android::nanora;
using nanotest::TempDir;

namespace {

const std::string kCard = "/storage/1234-ABCD/emulator_config/retroarch";

std::string readFile(const std::string& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void writeFile(const std::string& p, const std::string& text) { std::ofstream(p) << text; }

void copyFile(const std::string& from, const std::string& to) { writeFile(to, readFile(from)); }

// key -> value of every `key = "value"` line, and how often each key appears.
std::map<std::string, std::string> parse(const std::string& text, std::map<std::string, int>* count = nullptr) {
  std::map<std::string, std::string> kv;
  std::istringstream in(text);
  std::string l;
  while (std::getline(in, l)) {
    const size_t eq = l.find(" = \"");
    if (eq == std::string::npos || l.back() != '"') continue;
    const std::string k = l.substr(0, eq);
    kv[k] = l.substr(eq + 4, l.size() - eq - 5);
    if (count) (*count)[k]++;
  }
  return kv;
}

// The move's rewrite of one config (NanoMenu::retroarchFolderRewrite).
bool rewriteForMove(const std::string& path, const std::string& from, const std::string& to) {
  const PathMap keys = raMoveKeys(from, to);
  return raRewriteFile(path, raPrefixMap(from, to), &keys);
}

class Umask {
 public:
  explicit Umask(mode_t m) : mOld(umask(m)) {}
  ~Umask() { umask(mOld); }

 private:
  mode_t mOld;
};

}  // namespace

TEST(RaConfig, MoveToCardPointsTheFoldersAtTheCard) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  copyFile(nanotest::fixture("retroarch/retroarch.cfg"), cfg);
  ASSERT_TRUE(rewriteForMove(cfg, "", kCard));
  const std::string text = readFile(cfg);
  std::map<std::string, int> count;
  auto kv = parse(text, &count);

  // Moved folders, including the ones set to "default" or left empty.
  EXPECT_EQ(kCard + "/system", kv["system_directory"]);
  EXPECT_EQ(kCard + "/saves", kv["savefile_directory"]);
  EXPECT_EQ(kCard + "/states", kv["savestate_directory"]);
  EXPECT_EQ(kCard + "/screenshots", kv["screenshot_directory"]);
  EXPECT_EQ(kCard + "/playlists", kv["playlist_directory"]);
  EXPECT_EQ(kCard + "/playlists/builtin/content_history.lpl", kv["content_history_path"]);
  EXPECT_EQ(kCard + "/config", kv["rgui_config_directory"]);
  EXPECT_EQ(kCard + "/logs", kv["log_dir"]);
  // Added where the config had none.
  EXPECT_EQ(kCard + "/recordings", kv["recording_output_directory"]);
  EXPECT_EQ(kCard + "/config/remaps", kv["input_remapping_directory"]);
  // Box art and RetroArch's own app folder stay.
  EXPECT_EQ(kRaDefThumbs, kv["thumbnails_directory"]);
  EXPECT_EQ("/data/user/0/com.retroarch.aarch64/shaders", kv["video_shader_dir"]);
  EXPECT_EQ("/data/user/0/com.retroarch.aarch64/overlays/gamepads/neo-retropad/neo-retropad.cfg", kv["input_overlay"]);
  EXPECT_EQ("/data/user/0/com.retroarch.aarch64/autoconfig", kv["joypad_autoconfig_dir"]);
  EXPECT_EQ("/data/user/0/com.retroarch.aarch64/cores", kv["libretro_directory"]);
  // The user's own folders, including one whose name only starts like RetroArch's.
  EXPECT_EQ("/storage/emulated/0/ROMs", kv["rgui_browser_directory"]);
  EXPECT_EQ("/storage/emulated/0/RetroArchCheats", kv["cheat_database_path"]);
  // Thimsy's own settings (the Back button's menu key among them) are kept.
  EXPECT_EQ("f1", kv["input_menu_toggle"]);
  EXPECT_EQ("97", kv["input_player1_a_btn"]);
  EXPECT_EQ("false", kv["menu_show_start_screen"]);

  for (const auto& c : count) EXPECT_EQ(1, c.second) << c.first;
  // RetroArch reaches the card only through /storage: a raw mount path would be a folder it cannot open.
  EXPECT_EQ(std::string::npos, text.find("/mnt/media_rw")) << text;
  EXPECT_EQ(std::string::npos, text.find("/data/media")) << text;
  for (const auto& e : kv)
    if (e.second.find("1234-ABCD") != std::string::npos) EXPECT_EQ(0u, e.second.find("/storage/1234-ABCD/")) << e.first;
}

TEST(RaConfig, MoveBackToTheDefaults) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  writeFile(cfg, "savefile_directory = \"" + kCard + "/saves\"\n"
                 "video_shader = \"" + kCard + "/shaders/crt.slangp\"\n"
                 "thumbnails_directory = \"" + std::string(kRaDefThumbs) + "\"\n");
  ASSERT_TRUE(rewriteForMove(cfg, kCard, ""));
  auto kv = parse(readFile(cfg));
  EXPECT_EQ("/storage/emulated/0/RetroArch/saves", kv["savefile_directory"]);
  // An earlier version's card copy of the shaders lands in the RetroArch folder, not the app folder.
  EXPECT_EQ("/storage/emulated/0/RetroArch/shaders/crt.slangp", kv["video_shader"]);
  EXPECT_EQ(kRaDefThumbs, kv["thumbnails_directory"]);
  EXPECT_EQ("/data/user/0/com.retroarch.aarch64/autoconfig", kv["joypad_autoconfig_dir"]);
}

TEST(RaConfig, PrefixMapIsLongestFirst) {
  const PathMap m = raPrefixMap("", kCard);
  ASSERT_EQ(2u, m.size());
  EXPECT_EQ(PathMap::value_type(kRaDefThumbs, kRaDefThumbs), m[0]);
  EXPECT_EQ(PathMap::value_type(kRaDefSd, kCard), m[1]);
  const PathMap back = raPrefixMap(kCard, "");
  for (size_t i = 1; i < back.size(); i++) EXPECT_GE(back[i - 1].first.size(), back[i].first.size());
}

// Test 26: root's rewrite (umask 077) left RetroArch's default config owned by root, mode 0600.
// RetroArch could not read it (it started with its defaults: no Back button) nor save it ("Failed
// saving config to").
TEST(RaConfig, RewriteKeepsTheFilesModeAndOwner) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  copyFile(nanotest::fixture("retroarch/retroarch.cfg"), cfg);
  ASSERT_EQ(0, chmod(cfg.c_str(), 0664));
  // A group of ours other than the default one, when there is one, shows the group is kept too.
  std::vector<gid_t> groups(64);
  const int n = getgroups((int)groups.size(), groups.data());
  for (int i = 0; i < n; i++)
    if (groups[i] != getegid()) { ASSERT_EQ(0, chown(cfg.c_str(), (uid_t)-1, groups[i])); break; }
  struct stat before, after;
  ASSERT_EQ(0, stat(cfg.c_str(), &before));
  {
    Umask u(077);   // as init starts Nano
    ASSERT_TRUE(rewriteForMove(cfg, "", kCard));
  }
  ASSERT_EQ(0, stat(cfg.c_str(), &after));
  EXPECT_NE(before.st_ino, after.st_ino);   // replaced, not written over
  EXPECT_EQ(0664u, after.st_mode & 0777);
  EXPECT_EQ(before.st_uid, after.st_uid);
  EXPECT_EQ(before.st_gid, after.st_gid);
  EXPECT_NE(0, access((cfg + ".nanotmp").c_str(), F_OK));
}

TEST(RaConfig, UnchangedFileIsLeftAlone) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  writeFile(cfg, "input_menu_toggle = \"f1\"\n");
  struct stat before, after;
  ASSERT_EQ(0, stat(cfg.c_str(), &before));
  ASSERT_TRUE(raRewriteFile(cfg, raPrefixMap("", kCard), nullptr));
  ASSERT_EQ(0, stat(cfg.c_str(), &after));
  EXPECT_EQ(before.st_ino, after.st_ino);
  EXPECT_FALSE(raRewriteFile(t / "missing.cfg", raPrefixMap("", kCard), nullptr));
}

TEST(RaConfig, TreeRewritesConfigsAndPresetsOnly) {
  TempDir t("racfg");
  const std::string dir = t / "config";
  const std::string core = dir + "/Snes9x";
  ASSERT_EQ(0, mkdir(dir.c_str(), 0755));
  ASSERT_EQ(0, mkdir(core.c_str(), 0755));
  for (const char* f : {"Snes9x.cfg", "Snes9x.slangp", "notes.txt"})
    copyFile(nanotest::fixture(std::string("retroarch/config/Snes9x/") + f), core + "/" + f);
  // A config that is a link to a file elsewhere is not written through.
  const std::string outside = t / "outside.cfg";
  writeFile(outside, "savefile_directory = \"/storage/emulated/0/RetroArch/saves\"\n");
  ASSERT_EQ(0, symlink(outside.c_str(), (core + "/link.cfg").c_str()));

  raRewriteTree(dir, raPrefixMap("", kCard));
  EXPECT_EQ(kCard + "/shaders/crt.slangp", parse(readFile(core + "/Snes9x.cfg"))["video_shader"]);
  EXPECT_EQ("/data/user/0/com.retroarch.aarch64/overlays/gamepads/snes.cfg",
            parse(readFile(core + "/Snes9x.cfg"))["input_overlay"]);
  EXPECT_EQ(kCard + "/shaders/shaders_slang/crt/crt-easymode.slang", parse(readFile(core + "/Snes9x.slangp"))["shader0"]);
  EXPECT_EQ(readFile(nanotest::fixture("retroarch/config/Snes9x/notes.txt")), readFile(core + "/notes.txt"));
  EXPECT_EQ("savefile_directory = \"/storage/emulated/0/RetroArch/saves\"\n", readFile(outside));
}

// Root rewrites the default config in RetroArch's own folder, which RetroArch (and any core it
// downloaded) can change: a link there must not make root read or replace another file.
TEST(RaConfig, RewriteNeverFollowsALinkToTheConfig) {
  TempDir t("racfg");
  const std::string outside = t / "secret.txt";
  const std::string body = "token = \"/storage/emulated/0/RetroArch/x\"\n";
  writeFile(outside, body);
  const std::string link = t / "retroarch.cfg";
  ASSERT_EQ(0, symlink(outside.c_str(), link.c_str()));
  EXPECT_FALSE(rewriteForMove(link, "", kCard));
  EXPECT_EQ(body, readFile(outside));
  struct stat st;
  ASSERT_EQ(0, lstat(link.c_str(), &st));
  EXPECT_TRUE(S_ISLNK(st.st_mode));
}

TEST(RaConfig, RewriteNeverGoesThroughALinkedFolder) {
  TempDir t("racfg");
  const std::string real = t / "elsewhere";
  ASSERT_EQ(0, mkdir(real.c_str(), 0755));
  const std::string body = "savefile_directory = \"\"\n";
  writeFile(real + "/retroarch.cfg", body);
  ASSERT_EQ(0, symlink(real.c_str(), (t / "files").c_str()));
  EXPECT_FALSE(rewriteForMove(t / "files/retroarch.cfg", "", kCard));
  EXPECT_EQ(body, readFile(real + "/retroarch.cfg"));
  EXPECT_NE(0, access((real + "/retroarch.cfg.nanotmp").c_str(), F_OK));
  EXPECT_FALSE(raRepairOwner(t / "files/retroarch.cfg"));
}

// A FIFO in the config's place would block the move's worker in open() for good.
TEST(RaConfig, RewriteDoesNotWaitOnAFifo) {
  TempDir t("racfg");
  const std::string fifo = t / "retroarch.cfg";
  ASSERT_EQ(0, mkfifo(fifo.c_str(), 0600));
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    alarm(5);
    _exit(rewriteForMove(fifo, "", kCard) ? 0 : 1);
  }
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status)) << "blocked on the FIFO";
  EXPECT_EQ(1, WEXITSTATUS(status));
}

// Lines longer than any read buffer stay one line, bytes after a NUL included.
TEST(RaConfig, RewriteKeepsLongLinesWhole) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  const std::string longValue(6000, 'x');
  std::string nul = "binary = \"a";
  nul += '\0';
  nul += "b\"";
  const std::string text = "input_overlay = \"/storage/emulated/0/RetroArch/" + longValue + ".cfg\"\n"
                           "notes = \"" + longValue + "\"\n" + nul + "\n";
  writeFile(cfg, text);
  ASSERT_TRUE(raRewriteFile(cfg, raPrefixMap("", kCard), nullptr));
  const std::string want = "input_overlay = \"" + kCard + "/" + longValue + ".cfg\"\n"
                           "notes = \"" + longValue + "\"\n" + nul + "\n";
  EXPECT_EQ(want, readFile(cfg));
}

// A file far bigger than any config (a hostile one on the card) is not read into memory.
TEST(RaConfig, RewriteLeavesAHugeFileAlone) {
  TempDir t("racfg");
  const std::string cfg = t / "huge.cfg";
  writeFile(cfg, "savefile_directory = \"/storage/emulated/0/RetroArch/saves\"\n");
  ASSERT_EQ(0, truncate(cfg.c_str(), 64 << 20));   // sparse
  EXPECT_FALSE(raRewriteFile(cfg, raPrefixMap("", kCard), nullptr));
  struct stat st;
  ASSERT_EQ(0, stat(cfg.c_str(), &st));
  EXPECT_EQ(64 << 20, st.st_size);
  std::ifstream in(cfg);
  std::string first;
  std::getline(in, first);
  EXPECT_EQ("savefile_directory = \"/storage/emulated/0/RetroArch/saves\"", first);
}

TEST(RaConfig, RepairLeavesFilesNotOwnedByRootAlone) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  writeFile(cfg, "x = \"1\"\n");
  ASSERT_EQ(0, chmod(cfg.c_str(), 0600));
  if (geteuid() != 0) EXPECT_FALSE(raRepairOwner(cfg));
  EXPECT_FALSE(raRepairOwner(t / "missing.cfg"));
  EXPECT_FALSE(raRepairOwner("retroarch.cfg"));
  ASSERT_EQ(0, symlink(cfg.c_str(), (t / "link.cfg").c_str()));
  EXPECT_FALSE(raRepairOwner(t / "link.cfg"));
}

TEST(RaConfig, RepairGivesRootsFileToTheFoldersOwner) {
  if (geteuid() != 0) GTEST_SKIP() << "needs root to make a root-owned file in another owner's folder";
  TempDir t("racfg");
  const std::string dir = t / "files";
  ASSERT_EQ(0, mkdir(dir.c_str(), 0770));
  ASSERT_EQ(0, chown(dir.c_str(), 10123, 1078));
  const std::string cfg = dir + "/retroarch.cfg";
  {
    Umask u(077);
    writeFile(cfg, "x = \"1\"\n");
  }
  ASSERT_TRUE(raRepairOwner(cfg));
  struct stat st;
  ASSERT_EQ(0, stat(cfg.c_str(), &st));
  EXPECT_EQ(10123u, st.st_uid);
  EXPECT_EQ(1078u, st.st_gid);
  EXPECT_EQ(0600u, st.st_mode & 0600);
  EXPECT_FALSE(raRepairOwner(cfg));   // once
}

// Which config the launches pass (raLaunchConfig), from the cases RootWindowContainer's copy of the
// rule runs too (test/java/NanoRetroArchConfigCheck.java).
TEST(RaConfig, LaunchConfigSharedCases) {
  std::ifstream in(nanotest::fixture("retroarch/launch_config_cases.tsv"));
  ASSERT_TRUE(in.good());
  const std::string def = std::string(kRaDefCfgDir) + "/retroarch.cfg";
  std::string line;
  int cases = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    std::istringstream ls(line);
    for (std::string col; std::getline(ls, col, '\t');) f.push_back(col);
    ASSERT_EQ(4u, f.size()) << line;
    const std::string dir = f[0] == "-" ? "" : f[0];
    std::string mounts;
    if (f[1] != "-") {
      std::istringstream ms(f[1]);
      for (std::string mp; ms >> mp;) mounts += "/dev/fuse " + mp + " fuse rw,nosuid 0 0\n";
    }
    const std::string root = raVolumeRoot(dir);
    const bool mounted = root.empty() || raMountListed(mounts, root);
    EXPECT_EQ(f[3] == "default" ? def : f[3], raLaunchConfig(dir, mounted, f[2] == "1")) << line;
    cases++;
  }
  EXPECT_GE(cases, 10);
}

TEST(RaConfig, VolumeRoot) {
  EXPECT_EQ("/storage/1234-ABCD", raVolumeRoot(kCard));
  EXPECT_EQ("/storage/1234-ABCD", raVolumeRoot("/storage/1234-ABCD"));
  EXPECT_EQ("/storage/emulated", raVolumeRoot("/storage/emulated/0/RetroArch"));
  EXPECT_EQ("/storage/emulated", raVolumeRoot("/storage/self/primary/RetroArch"));
  EXPECT_EQ("", raVolumeRoot("/storage/"));
  EXPECT_EQ("", raVolumeRoot("/storage"));
  EXPECT_EQ("", raVolumeRoot("/mnt/media_rw/1234-ABCD/retroarch"));
  EXPECT_EQ("", raVolumeRoot(""));
}

TEST(RaConfig, MountListedMatchesTheMountPointOnly) {
  const std::string m =
      "/dev/block/vold/public:179,1 /mnt/media_rw/1234-ABCD vfat rw 0 0\n"
      "/dev/fuse /storage/emulated fuse rw 0 0\n"
      "/dev/fuse /storage/1234-ABCD fuse rw 0 0";   // the last line has no newline
  EXPECT_TRUE(raMountListed(m, "/storage/1234-ABCD"));
  EXPECT_TRUE(raMountListed(m, "/storage/emulated"));
  EXPECT_FALSE(raMountListed(m, "/storage/1234-ABC"));
  EXPECT_FALSE(raMountListed(m, "/storage"));
  EXPECT_FALSE(raMountListed(m, "fuse"));
  EXPECT_FALSE(raMountListed(m, "/dev/fuse"));
  EXPECT_FALSE(raMountListed(m, ""));
  EXPECT_FALSE(raMountListed("", "/storage/1234-ABCD"));
  EXPECT_FALSE(raMountListed("garbage\n\n /\n", "/storage/1234-ABCD"));
}

TEST(RaConfig, RawPath) {
  EXPECT_EQ("/data/media/0", raRawPath("/storage/emulated/0"));
  EXPECT_EQ("/data/media/0/RetroArch", raRawPath("/storage/emulated/0/RetroArch"));
  EXPECT_EQ("/mnt/media_rw/1234-ABCD/emulator_config/retroarch", raRawPath(kCard));
  EXPECT_EQ("/storage/self/primary/RetroArch", raRawPath("/storage/self/primary/RetroArch"));
  EXPECT_EQ("/data/media/0x", raRawPath("/data/media/0x"));
}

TEST(RaConfig, LaunchConfigNowLooksForTheFile) {
  TempDir t("racfg");
  const std::string def = std::string(kRaDefCfgDir) + "/retroarch.cfg";
  const std::string dir = t / "ra";
  ASSERT_EQ(0, mkdir(dir.c_str(), 0755));
  std::string verified = "stale";
  EXPECT_EQ(def, raLaunchConfigNow(dir, &verified));
  EXPECT_EQ("", verified);
  writeFile(dir + "/retroarch.cfg", "x = \"1\"\n");
  EXPECT_EQ(dir + "/retroarch.cfg", raLaunchConfigNow(dir, &verified));
  EXPECT_EQ(dir, verified);
  EXPECT_EQ(dir + "/retroarch.cfg", raLaunchConfigNow(dir, nullptr));
  EXPECT_EQ(def, raLaunchConfigNow("", &verified));
  EXPECT_EQ("", verified);
  // A card that is not mounted here.
  EXPECT_EQ(def, raLaunchConfigNow("/storage/0000-NONE/emulator_config/retroarch", &verified));
  EXPECT_EQ("", verified);
}

namespace {

// Thimsy's RetroArch hotkey profile's rule (mods/hotkeys/retroarch.json in the build repo).
android::njson::Value raHotkeyRule() {
  android::njson::Value rule;
  EXPECT_TRUE(android::njson::parse(
      "{\"file\": \"${sdcard}/Android/data/com.retroarch.aarch64/files/retroarch.cfg\","
      " \"format\": \"retroarch_cfg\", \"mode\": \"add\", \"values\": {\"input_save_state\": \"f2\","
      " \"input_load_state\": \"f4\", \"input_toggle_fast_forward\": \"space\", \"input_rewind\": \"r\"}}",
      &rule));
  return rule;
}

}  // namespace

// Launches use the card's config when RetroArch's folder is there, so the hotkeys are bound in it too.
TEST(RaConfig, HotkeysCoverTheConfigLaunchesUse) {
  using android::nanohotkeys::ruleFiles;
  const std::string def = std::string(kRaDefCfgDir) + "/retroarch.cfg";
  const std::string card = kCard + "/retroarch.cfg";
  EXPECT_EQ((std::vector<std::string>{def, card}), ruleFiles(def, card));
  EXPECT_EQ(std::vector<std::string>{def}, ruleFiles(def, def));
  EXPECT_EQ(std::vector<std::string>{def}, ruleFiles(def, ""));
  const std::string other = "/storage/emulated/0/Android/data/org.ppsspp.ppsspp/files/PSP/SYSTEM/ppsspp.ini";
  EXPECT_EQ(std::vector<std::string>{other}, ruleFiles(other, card));
}

TEST(RaConfig, HotkeysBindKeysInTheCardConfig) {
  TempDir t("racfg");
  const std::string dir = t / "retroarch";
  ASSERT_EQ(0, mkdir(dir.c_str(), 0775));
  const std::string cfg = dir + "/retroarch.cfg";
  writeFile(cfg, "input_menu_toggle = \"f1\"\ninput_save_state = \"\"\ninput_rewind = \"backspace\"\n");
  ASSERT_EQ(0, chmod(cfg.c_str(), 0664));
  struct stat before, after;
  ASSERT_EQ(0, stat(cfg.c_str(), &before));
  {
    Umask u(077);
    android::nanohotkeys::provisionFile(cfg, raHotkeyRule());
  }
  std::map<std::string, int> count;
  auto kv = parse(readFile(cfg), &count);
  EXPECT_EQ("f1", kv["input_menu_toggle"]);
  EXPECT_EQ("f2", kv["input_save_state"]);
  EXPECT_EQ("f4", kv["input_load_state"]);
  EXPECT_EQ("space", kv["input_toggle_fast_forward"]);
  EXPECT_EQ("backspace", kv["input_rewind"]);   // the user's own key stays
  for (const auto& c : count) EXPECT_EQ(1, c.second) << c.first;
  ASSERT_EQ(0, stat(cfg.c_str(), &after));
  EXPECT_EQ(0664u, after.st_mode & 0777);
  EXPECT_EQ(before.st_uid, after.st_uid);
  EXPECT_EQ(before.st_gid, after.st_gid);
  EXPECT_NE(0, access((cfg + ".nanohotkeys").c_str(), F_OK));
  EXPECT_NE(0, access((cfg + ".nanotmp").c_str(), F_OK));

  android::nanohotkeys::provisionFile(cfg, raHotkeyRule());   // nothing left to bind
  struct stat again;
  ASSERT_EQ(0, stat(cfg.c_str(), &again));
  EXPECT_EQ(after.st_ino, again.st_ino);
}

TEST(RaConfig, HotkeysNeverWriteThroughALink) {
  TempDir t("racfg");
  const std::string outside = t / "secret.cfg";
  const std::string body = "input_menu_toggle = \"f1\"\n";
  writeFile(outside, body);
  const std::string dir = t / "retroarch";
  ASSERT_EQ(0, mkdir(dir.c_str(), 0775));
  ASSERT_EQ(0, symlink(outside.c_str(), (dir + "/retroarch.cfg").c_str()));
  android::nanohotkeys::provisionFile(dir + "/retroarch.cfg", raHotkeyRule());
  EXPECT_EQ(body, readFile(outside));

  const std::string real = t / "elsewhere";
  ASSERT_EQ(0, mkdir(real.c_str(), 0775));
  writeFile(real + "/retroarch.cfg", body);
  ASSERT_EQ(0, symlink(real.c_str(), (t / "linked").c_str()));
  android::nanohotkeys::provisionFile(t / "linked/retroarch.cfg", raHotkeyRule());
  EXPECT_EQ(body, readFile(real + "/retroarch.cfg"));
}

// ---- Move Emulator Files: the shipped config, other spellings, read-back, and the heal ----

namespace {

const std::string kInternal = "/storage/emulated/0/RetroArch";

std::string slurp(const std::string& p) { return readFile(p); }

ino_t inode(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? st.st_ino : 0;
}

}  // namespace

// The default retroarch.cfg the build ships (its folder and path settings): every internal folder
// moves, box art and RetroArch's app folder stay, and saves stay beside the games.
TEST(RaConfig, ShippedConfigMovesEveryInternalFolder) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  copyFile(nanotest::fixture("retroarch/retroarch_shipped.cfg"), cfg);
  const auto before = parse(slurp(cfg));
  ASSERT_TRUE(rewriteForMove(cfg, "", kCard));
  const auto kv = parse(slurp(cfg));
  int moved = 0;
  for (const auto& e : before) {
    const std::string& v = e.second;
    if (v.compare(0, kInternal.size() + 1, kInternal + "/") == 0 && v.compare(0, strlen(kRaDefThumbs), kRaDefThumbs) != 0) {
      EXPECT_EQ(kCard + v.substr(kInternal.size()), kv.at(e.first)) << e.first;
      moved++;
    } else if (!v.empty() && v != "default") {
      EXPECT_EQ(v, kv.at(e.first)) << e.first;
    }
  }
  EXPECT_EQ(16, moved);
  EXPECT_EQ(kRaDefThumbs, kv.at("thumbnails_directory"));
  EXPECT_EQ("true", kv.at("savefiles_in_content_dir"));
  EXPECT_EQ("true", kv.at("savestates_in_content_dir"));
  EXPECT_EQ(kCard + "/recordings", kv.at("recording_output_directory"));
  EXPECT_TRUE(raStaleKeys(slurp(cfg), raPrefixMap("", kCard), nullptr).empty());
}

// RetroArch, a frontend or the user may spell a storage folder another way: each is moved, in the
// /storage form apps use, whatever its letter case. Folders that only look alike stay.
TEST(RaConfig, OtherSpellingsOfTheFolderMove) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  writeFile(cfg,
            "system_directory = \"/sdcard/RetroArch/system\"\n"
            "savefile_directory = \"/storage/self/primary/RetroArch/saves\"\n"
            "screenshot_directory = \"/data/media/0/RetroArch/screenshots\"\n"
            "playlist_directory = \"/storage/emulated/0/retroarch/playlists\"\n"
            "savestate_directory = \"/storage/emulated/0/RetroArch/states/\"\n"
            "cache_directory = \":/temp\"\n"
            "log_dir = \"~/RetroArch/logs\"\n"
            "rgui_browser_directory = \"/sdcard/ROMs\"\n"
            "cheat_database_path = \"/sdcard/RetroArchCheats\"\n"
            "thumbnails_directory = \"/sdcard/RetroArch/thumbnails\"\n");
  ASSERT_TRUE(raRewriteFile(cfg, raPrefixMap("", kCard), nullptr));
  const auto kv = parse(slurp(cfg));
  EXPECT_EQ(kCard + "/system", kv.at("system_directory"));
  EXPECT_EQ(kCard + "/saves", kv.at("savefile_directory"));
  EXPECT_EQ(kCard + "/screenshots", kv.at("screenshot_directory"));
  EXPECT_EQ(kCard + "/playlists", kv.at("playlist_directory"));
  EXPECT_EQ(kCard + "/states/", kv.at("savestate_directory"));
  EXPECT_EQ(":/temp", kv.at("cache_directory"));
  EXPECT_EQ("~/RetroArch/logs", kv.at("log_dir"));
  EXPECT_EQ("/sdcard/ROMs", kv.at("rgui_browser_directory"));
  EXPECT_EQ("/sdcard/RetroArchCheats", kv.at("cheat_database_path"));
  EXPECT_EQ("/sdcard/RetroArch/thumbnails", kv.at("thumbnails_directory"));   // stays, as written

  // A card's raw mount path moves with the card's folder.
  writeFile(cfg, "savefile_directory = \"/mnt/media_rw/1234-ABCD/emulator_config/retroarch/saves\"\n");
  ASSERT_TRUE(raRewriteFile(cfg, raPrefixMap(kCard, ""), nullptr));
  EXPECT_EQ(kInternal + "/saves", parse(slurp(cfg)).at("savefile_directory"));

  // So does a folder an earlier version set by another spelling, in either spelling.
  for (const std::string from : {"/storage/self/primary/RetroArchSet", "/data/media/0/RetroArchSet"}) {
    writeFile(cfg, "savefile_directory = \"/storage/self/primary/RetroArchSet/saves\"\n"
                   "savestate_directory = \"/storage/emulated/0/RetroArchSet/states\"\n");
    ASSERT_TRUE(rewriteForMove(cfg, from, kCard));
    const auto moved = parse(slurp(cfg));
    EXPECT_EQ(kCard + "/saves", moved.at("savefile_directory")) << from;
    EXPECT_EQ(kCard + "/states", moved.at("savestate_directory")) << from;
    const PathMap keys = raMoveKeys(from, kCard);
    EXPECT_TRUE(raStaleKeys(slurp(cfg), raPrefixMap(from, kCard), &keys).empty()) << from;
  }
}

TEST(RaConfig, SecondRewriteChangesNothing) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  copyFile(nanotest::fixture("retroarch/retroarch_shipped.cfg"), cfg);
  ASSERT_TRUE(rewriteForMove(cfg, "", kCard));
  const std::string once = slurp(cfg);
  const ino_t ino = inode(cfg);
  ASSERT_TRUE(rewriteForMove(cfg, "", kCard));
  EXPECT_EQ(once, slurp(cfg));
  EXPECT_EQ(ino, inode(cfg));   // not even replaced
}

TEST(RaConfig, MissingConfigIsReportedOrMade) {
  TempDir t("racfg");
  const PathMap keys = raMoveKeys("", kCard);
  EXPECT_EQ(ENOENT, raRewriteFileEx(t / "retroarch.cfg", raPrefixMap("", kCard), &keys));
  EXPECT_NE(0, access((t / "retroarch.cfg").c_str(), F_OK));
  EXPECT_EQ(0, raRewriteFileEx(t / "retroarch.cfg", raPrefixMap("", kCard), &keys, true));
  const auto kv = parse(slurp(t / "retroarch.cfg"));
  EXPECT_EQ(kCard + "/saves", kv.at("savefile_directory"));
  EXPECT_EQ(kRaDefThumbs, kv.at("thumbnails_directory"));
  struct stat st;
  ASSERT_EQ(0, stat((t / "retroarch.cfg").c_str(), &st));
  EXPECT_EQ(0660u, st.st_mode & 0777);
  std::vector<std::string> stale;
  EXPECT_EQ(0, raStaleKeysIn(t / "retroarch.cfg", raPrefixMap("", kCard), &keys, &stale));
  EXPECT_TRUE(stale.empty());
  EXPECT_EQ(ENOENT, raStaleKeysIn(t / "none.cfg", raPrefixMap("", kCard), &keys, &stale));
}

TEST(RaConfig, StaleKeysNameWhatStillPointsInside) {
  const PathMap map = raPrefixMap("", kCard);
  const PathMap keys = {{"system_directory", kCard + "/system"}, {"log_dir", kCard + "/logs"},
                        {"cache_directory", kCard + "/temp"}};
  const std::string text =
      "savefile_directory = \"/storage/emulated/0/RetroArch/saves\"\n"
      "screenshot_directory = \"/sdcard/RetroArch/screenshots\"\n"
      "thumbnails_directory = \"/storage/emulated/0/RetroArch/thumbnails\"\n"
      "rgui_browser_directory = \"/storage/emulated/0/ROMs\"\n"
      "system_directory = \"default\"\n"
      "log_dir = \"" + kCard + "/logs\"\n";
  EXPECT_EQ((std::vector<std::string>{"savefile_directory", "screenshot_directory", "system_directory", "cache_directory"}),
            raStaleKeys(text, map, &keys));
  EXPECT_EQ((std::vector<std::string>{"savefile_directory", "screenshot_directory"}), raStaleKeys(text, map, nullptr));
}

// A RetroArch session that could not read its config saves its internal defaults into it; the heal
// points the folders at the card again and leaves everything else.
TEST(RaConfig, HealPointsADefaultsSessionsConfigAtTheCard) {
  TempDir t("racfg");
  const std::string cfg = t / "retroarch.cfg";
  copyFile(nanotest::fixture("retroarch/retroarch_shipped.cfg"), cfg);
  std::string text = slurp(cfg) + "input_menu_toggle = \"f1\"\n";
  writeFile(cfg, text);
  ASSERT_EQ(0, chmod(cfg.c_str(), 0660));
  std::vector<std::string> fixed;
  ASSERT_EQ(0, raHealConfig(cfg, kCard, &fixed));
  EXPECT_FALSE(fixed.empty());
  EXPECT_NE(fixed.end(), std::find(fixed.begin(), fixed.end(), "savefile_directory"));
  const auto kv = parse(slurp(cfg));
  EXPECT_EQ(kCard + "/saves", kv.at("savefile_directory"));
  EXPECT_EQ(kCard + "/playlists/builtin/content_history.lpl", kv.at("content_history_path"));
  EXPECT_EQ("f1", kv.at("input_menu_toggle"));
  EXPECT_EQ(kRaDefThumbs, kv.at("thumbnails_directory"));
  struct stat st;
  ASSERT_EQ(0, stat(cfg.c_str(), &st));
  EXPECT_EQ(0660u, st.st_mode & 0777);
  // Right now: nothing to do, and the file is left as it is.
  const ino_t ino = inode(cfg);
  ASSERT_EQ(0, raHealConfig(cfg, kCard, &fixed));
  EXPECT_TRUE(fixed.empty());
  EXPECT_EQ(ino, inode(cfg));
  EXPECT_EQ(ENOENT, raHealConfig(t / "none.cfg", kCard, &fixed));
}

// Card to card: a per-core override, its core options and a playlist entry for content kept in the
// folder move with it; ROMs elsewhere stay.
TEST(RaConfig, TreeRewritesCoreOptionsAndPlaylists) {
  TempDir t("racfg");
  const std::string other = "/storage/9999-0000/emulator_config/retroarch";
  const std::string core = t / "config";
  ASSERT_EQ(0, mkdir(core.c_str(), 0755));
  writeFile(core + "/Snes9x.opt", "snes9x_bios = \"" + kCard + "/system/bs-x.bin\"\n");
  const std::string pl = t / "playlists";
  ASSERT_EQ(0, mkdir(pl.c_str(), 0755));
  const std::string lpl =
      "{\n  \"version\": \"1.5\",\n  \"items\": [\n    {\n"
      "      \"path\": \"" + kCard + "/downloads/game.zip\",\n"
      "      \"core_path\": \"/data/user/0/com.retroarch.aarch64/cores/snes9x_libretro_android.so\",\n"
      "      \"db_name\": \"Nintendo - SNES.lpl\"\n    },\n    {\n"
      "      \"path\": \"/storage/emulated/0/ROMs/snes/Mario.sfc\",\n"
      "      \"label\": \"" + kCard + "/not a path key\"\n    }\n  ]\n}\n";
  writeFile(pl + "/Nintendo - SNES.lpl", lpl);
  raRewriteTree(core, raPrefixMap(kCard, other));
  raRewriteTree(pl, raPrefixMap(kCard, other));
  EXPECT_EQ("snes9x_bios = \"" + other + "/system/bs-x.bin\"\n", slurp(core + "/Snes9x.opt"));
  std::string want = lpl;
  want.replace(want.find(kCard + "/downloads"), kCard.size(), other);
  EXPECT_EQ(want, slurp(pl + "/Nintendo - SNES.lpl"));
}

TEST(RaConfig, EditFileKeepsModeAndNeverFollowsALink) {
  TempDir t("racfg");
  const std::string f = t / "emu.cfg";
  writeFile(f, "a = 1\n");
  ASSERT_EQ(0, chmod(f.c_str(), 0640));
  {
    Umask u(077);
    EXPECT_EQ(0, raEditFile(f, [](std::string* s) { *s += "b = 2\n"; return true; }));
  }
  EXPECT_EQ("a = 1\nb = 2\n", slurp(f));
  struct stat st;
  ASSERT_EQ(0, stat(f.c_str(), &st));
  EXPECT_EQ(0640u, st.st_mode & 0777);
  ASSERT_EQ(0, symlink(f.c_str(), (t / "link.cfg").c_str()));
  EXPECT_EQ(ELOOP, raEditFile(t / "link.cfg", [](std::string* s) { s->clear(); return true; }));
  EXPECT_EQ("a = 1\nb = 2\n", slurp(f));
  EXPECT_EQ(ENOENT, raEditFile(t / "none.cfg", [](std::string*) { return true; }));
}

// The hotkey rules name PPSSPP's and Flycast's folders by their defaults; after the move they bind
// the keys where the emulator now keeps its files.
TEST(RaConfig, HotkeyRulesFollowTheMovedFolders) {
  using android::nanohotkeys::MovedFolders;
  using android::nanohotkeys::relocatedFile;
  const std::string psp = "/storage/emulated/0/PSP/SYSTEM/controls.ini";
  const std::string fly = "/storage/emulated/0/Android/data/com.flycast.emulator/files/mappings/Android_Virtual.cfg";
  MovedFolders m;
  EXPECT_EQ(psp, relocatedFile(psp, m));
  EXPECT_EQ(fly, relocatedFile(fly, m));
  m.ppssppMemstick = "/storage/1234-ABCD/emulator_config/ppsspp";
  m.flycastHome = "/storage/1234-ABCD/emulator_config/flycast/";
  EXPECT_EQ("/storage/1234-ABCD/emulator_config/ppsspp/PSP/SYSTEM/controls.ini", relocatedFile(psp, m));
  EXPECT_EQ("/storage/1234-ABCD/emulator_config/flycast/mappings/Android_Virtual.cfg", relocatedFile(fly, m));
  m.ppssppMemstick = "/storage/1234-ABCD/games/PSP";   // a memory stick that is itself the PSP folder
  EXPECT_EQ("/storage/1234-ABCD/games/PSP/SYSTEM/controls.ini", relocatedFile(psp, m));
  m.ppssppMemstick = "/storage/emulated/0";
  EXPECT_EQ(psp, relocatedFile(psp, m));
  const std::string other = "/storage/emulated/0/PSPX/a.ini";
  EXPECT_EQ(other, relocatedFile(other, m));
}

TEST(RaConfig, HotkeysWriteThroughOnlyTheMovesLinks) {
  using android::nanohotkeys::isCardEmulatorFolder;
  using android::nanohotkeys::throughMoveLinks;
  EXPECT_TRUE(isCardEmulatorFolder("/storage/1234-ABCD/emulator_config/dolphin/Config"));
  EXPECT_FALSE(isCardEmulatorFolder("/storage/emulated/0/emulator_config/dolphin/Config"));
  EXPECT_FALSE(isCardEmulatorFolder("/storage/1234-ABCD/emulator_config/../../data/system"));
  EXPECT_FALSE(isCardEmulatorFolder("/storage/1234-ABCD/emulator_config"));
  EXPECT_FALSE(isCardEmulatorFolder("/data/system/emulator_config/x"));

  TempDir t("racfg");
  const std::string card = t / "card";
  const std::string files = t / "files";
  ASSERT_EQ(0, mkdir(card.c_str(), 0755));
  ASSERT_EQ(0, mkdir((card + "/Config").c_str(), 0755));
  ASSERT_EQ(0, mkdir(files.c_str(), 0755));
  ASSERT_EQ(0, symlink((card + "/Config").c_str(), (files + "/Config").c_str()));
  auto onCard = [&](const std::string& p) { return p.compare(0, card.size() + 1, card + "/") == 0; };
  EXPECT_EQ(card + "/Config/Hotkeys.ini", throughMoveLinks(files + "/Config/Hotkeys.ini", onCard));
  EXPECT_EQ(files + "/Other/Hotkeys.ini", throughMoveLinks(files + "/Other/Hotkeys.ini", onCard));   // no link
  // A link somewhere the move never points: nothing is written.
  ASSERT_EQ(0, symlink(t.path().c_str(), (files + "/Elsewhere").c_str()));
  EXPECT_EQ("", throughMoveLinks(files + "/Elsewhere/Hotkeys.ini", onCard));
}
