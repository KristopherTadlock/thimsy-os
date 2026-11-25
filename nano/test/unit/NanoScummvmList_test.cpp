// ScummVM's games as ScummVM lists them (NanoScummvmList): which scummvm.ini sections are games, the
// file each gets, and the folder kept to the list.

#include <gtest/gtest.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "NanoScummvmList.h"
#include "nano_test_fs.h"

using namespace android::nanoscummvm;
using nanotest::TempDir;

namespace {

const char kIni[] =
    "\xEF\xBB\xBF[scummvm]\r\n"
    "gfx_mode=opengl\r\n"
    "extrapath=/storage/emulated/0/BIOS/scummvm\r\n"
    "\r\n"
    "[sky]\r\n"
    "description=Beneath a Steel Sky (CD/DOS/English)\r\n"
    "path=/storage/emulated/0/ROMs/scummvm/Beneath a Steel Sky\r\n"
    "engineid=sky\r\n"
    "gameid=sky\r\n"
    "[keymapper]\r\n"
    "keymap_global_MENU=JOY_START\r\n"
    "[monkey2-fr]\r\n"
    "gameid=monkey2\r\n"
    "description=Monkey Island 2: LeChuck's Revenge (DOS/French)\r\n"
    "; a comment\r\n"
    "[-bad]\r\n"
    "gameid=queen\r\n"
    "[my game]\r\n"
    "gameid=queen\r\n"
    "[nogameid]\r\n"
    "description=Something\r\n"
    "[SKY]\r\n"
    "Language=en\r\n";

std::string slurp(const std::string& p) {
    std::string s;
    const int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) return s;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) s.append(buf, (size_t)n);
    close(fd);
    return s;
}

std::set<std::string> listDir(const std::string& dir) {
    std::set<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") out.insert(n);
    }
    closedir(d);
    return out;
}

Game game(const std::string& target, const std::string& gameid, const std::string& desc) {
    Game g;
    g.target = target;
    g.gameid = gameid;
    g.description = desc;
    return g;
}

}  // namespace

TEST(ScummvmList, Packages) {
    EXPECT_TRUE(isScummvmPkg("org.scummvm.scummvm"));
    EXPECT_TRUE(isScummvmPkg("org.scummvm.scummvm.debug"));
    EXPECT_FALSE(isScummvmPkg("com.retroarch.aarch64"));
    EXPECT_FALSE(isScummvmPkg(""));
    EXPECT_EQ("/data/data/org.scummvm.scummvm/files/scummvm.ini", iniPath("org.scummvm.scummvm"));
    EXPECT_EQ("/data/system/nano_scummvm/org.scummvm.scummvm/scummvm", mirrorDir("org.scummvm.scummvm"));
    EXPECT_TRUE(isMirrorPath(mirrorDir("org.scummvm.scummvm") + "/Beneath a Steel Sky.scummvm"));
    EXPECT_FALSE(isMirrorPath("/storage/emulated/0/ROMs/scummvm/Beneath a Steel Sky.scummvm"));
    EXPECT_FALSE(isMirrorPath("/data/system/nano_scummvmx/a.scummvm"));
}

TEST(ScummvmList, OnlyTheScummvmSystemListsGames) {
    EXPECT_TRUE(listsGames("scummvm", "org.scummvm.scummvm"));
    EXPECT_TRUE(listsGames("ScummVM", "org.scummvm.scummvm.debug"));
    EXPECT_FALSE(listsGames("ags", "org.scummvm.scummvm"));   // AGS's .scummvm files are scanned
    EXPECT_FALSE(listsGames("scummvm", "com.retroarch.aarch64"));
}

TEST(ScummvmList, ParsesGamesOnly) {
    const std::vector<Game> g = parseGames(kIni);
    ASSERT_EQ(2u, g.size());
    EXPECT_EQ("sky", g[0].target);
    EXPECT_EQ("sky", g[0].gameid);
    EXPECT_EQ("Beneath a Steel Sky (CD/DOS/English)", g[0].description);
    EXPECT_EQ("monkey2-fr", g[1].target);
    EXPECT_EQ("monkey2", g[1].gameid);
    EXPECT_EQ("Monkey Island 2: LeChuck's Revenge (DOS/French)", g[1].description);
}

TEST(ScummvmList, RepeatedSectionAddsToFirst) {
    const std::vector<Game> g = parseGames("[queen]\ndescription=Old\n[other]\nx=1\n[QUEEN]\nGameID=queen\ndescription=New\n");
    ASSERT_EQ(1u, g.size());
    EXPECT_EQ("queen", g[0].target);
    EXPECT_EQ("New", g[0].description);
}

TEST(ScummvmList, EmptyAndGarbage) {
    EXPECT_TRUE(parseGames("").empty());
    EXPECT_TRUE(parseGames("gameid=sky\n").empty());   // outside any section
    EXPECT_TRUE(parseGames(std::string("[sky\0x]\ngameid=sky\n", 19)).empty());
    EXPECT_TRUE(parseGames("[]\ngameid=sky\n").empty());
    EXPECT_TRUE(parseGames("[sky]\ngameid=\n").empty());
}

TEST(ScummvmList, OldEntryWithAPathButNoGameId) {
    const std::vector<Game> g = parseGames("[scummvm]\npath=/x\n[cloud]\npath=/y\n[keymapper]\npath=/z\n"
                                           "[monkey]\npath=/storage/emulated/0/ROMs/scummvm/Monkey\n"
                                           "[empty]\npath=\n");
    ASSERT_EQ(1u, g.size());
    EXPECT_EQ("monkey", g[0].target);
    EXPECT_EQ("monkey", g[0].gameid);
}

TEST(ScummvmList, IniComplete) {
    EXPECT_TRUE(iniComplete(kIni));
    EXPECT_TRUE(iniComplete("[sky]\ngameid=sky\n[ScummVM ]\n"));
    EXPECT_FALSE(iniComplete(""));                              // truncated for a rewrite
    EXPECT_FALSE(iniComplete("[sky]\ngameid=sky\n"));          // cut off before [scummvm]
    EXPECT_FALSE(iniComplete("[scummvmx]\n"));
}

TEST(ScummvmList, FileNames) {
    std::vector<Game> g = {
        game("sky", "sky", "Beneath a Steel Sky (CD/DOS/English)"),
        game("monkey2-fr", "monkey2", "Monkey Island 2: LeChuck's Revenge (DOS/French)"),
        game("queen", "queen", ""),
        game("odd", "odd", "  .Hidden? <Name>*|x/y.  "),
        game("blank", "blank", "(CD)"),
        game("ctl", "ctl", "Tab\there\x01"),
    };
    const std::vector<std::string> n = fileNames(g);
    ASSERT_EQ(g.size(), n.size());
    EXPECT_EQ("Beneath a Steel Sky.scummvm", n[0]);
    EXPECT_EQ("Monkey Island 2 - LeChuck's Revenge.scummvm", n[1]);
    EXPECT_EQ("queen.scummvm", n[2]);
    EXPECT_EQ("Hidden Name-x, y.scummvm", n[3]);
    EXPECT_EQ("(CD).scummvm", n[4]);
    EXPECT_EQ("Tab here.scummvm", n[5]);
}

TEST(ScummvmList, VariantsKeepTheirDescriptions) {
    std::vector<Game> g = {
        game("monkey", "monkey", "The Secret of Monkey Island (EGA/DOS/English)"),
        game("monkey-cd", "monkey", "The Secret of Monkey Island (CD/DOS/English)"),
        game("sky", "sky", "Beneath a Steel Sky (CD/DOS/English)"),
        game("sky-1", "sky", "beneath a steel sky (CD/DOS/English)"),
        game("a", "x", "Same"),
        game("b", "x", "Same [a]"),
    };
    const std::vector<std::string> n = fileNames(g);
    EXPECT_EQ("The Secret of Monkey Island (EGA, DOS, English).scummvm", n[0]);
    EXPECT_EQ("The Secret of Monkey Island (CD, DOS, English).scummvm", n[1]);
    EXPECT_EQ("Beneath a Steel Sky (CD, DOS, English) [sky].scummvm", n[2]);
    EXPECT_EQ("beneath a steel sky (CD, DOS, English) [sky-1].scummvm", n[3]);
    EXPECT_EQ("Same.scummvm", n[4]);
    EXPECT_EQ("Same [a].scummvm", n[5]);
    std::set<std::string> lowered;
    for (std::string s : n) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        EXPECT_TRUE(lowered.insert(s).second) << s;
    }
}

TEST(ScummvmList, NameCollidingWithASuffixedOneStillUnique) {
    std::vector<Game> g = {game("a", "x", "Same"), game("b", "x", "Same"), game("c", "x", "Same [a]")};
    const std::vector<std::string> n = fileNames(g);
    EXPECT_EQ("Same [a].scummvm", n[0]);
    EXPECT_EQ("Same [b].scummvm", n[1]);
    EXPECT_EQ("Same [a] (2).scummvm", n[2]);
}

TEST(ScummvmList, AddingAVariantKeepsTheFirstName) {
    const Game ega = game("monkey", "monkey", "The Secret of Monkey Island (EGA/DOS/English)");
    const Game cd = game("monkey-cd", "monkey", "The Secret of Monkey Island (CD/DOS/English)");
    EXPECT_EQ("The Secret of Monkey Island.scummvm", fileNames({ega})[0]);
    const std::vector<std::string> n =
        fileNames({ega, cd}, {{"monkey", "The Secret of Monkey Island.scummvm"}});
    EXPECT_EQ("The Secret of Monkey Island.scummvm", n[0]);
    EXPECT_EQ("The Secret of Monkey Island (CD, DOS, English).scummvm", n[1]);
    // The variant gone again: each keeps the name it has.
    EXPECT_EQ("The Secret of Monkey Island (CD, DOS, English).scummvm",
              fileNames({cd}, {{"monkey-cd", "The Secret of Monkey Island (CD, DOS, English).scummvm"}})[0]);
    // A " (k)" added for uniqueness is kept too.
    EXPECT_EQ("The Secret of Monkey Island (2).scummvm",
              fileNames({ega}, {{"monkey", "The Secret of Monkey Island (2).scummvm"}})[0]);
}

TEST(ScummvmList, NewDescriptionOrClashRenames) {
    const Game ega = game("monkey", "monkey", "Monkey Island (EGA/DOS/English)");
    // The description changed in ScummVM: the name follows it.
    EXPECT_EQ("Monkey Island.scummvm", fileNames({ega}, {{"monkey", "The Secret of Monkey Island.scummvm"}})[0]);
    // A name another game already keeps is not taken twice.
    const Game other = game("other", "monkey", "Monkey Island (CD/DOS/English)");
    const std::vector<std::string> n =
        fileNames({ega, other}, {{"monkey", "Monkey Island.scummvm"}, {"other", "Monkey Island.scummvm"}});
    EXPECT_EQ("Monkey Island.scummvm", n[0]);
    EXPECT_EQ("Monkey Island (CD, DOS, English).scummvm", n[1]);
}

TEST(ScummvmList, SyncDirKeepsANameWhenAVariantIsAdded) {
    TempDir t("scummvmlist");
    const std::string dir = t / "scummvm";
    const Game ega = game("monkey", "monkey", "The Secret of Monkey Island (EGA/DOS/English)");
    ASSERT_TRUE(syncDir(dir, {ega}));
    const std::string first = dir + "/The Secret of Monkey Island.scummvm";
    const struct timespec old[2] = {{1000000, 0}, {1000000, 0}};
    ASSERT_EQ(0, utimensat(AT_FDCWD, first.c_str(), old, 0));
    ASSERT_TRUE(syncDir(dir, {ega, game("monkey-cd", "monkey", "The Secret of Monkey Island (CD/DOS/English)")}));
    EXPECT_EQ((std::set<std::string>{"The Secret of Monkey Island.scummvm",
                                     "The Secret of Monkey Island (CD, DOS, English).scummvm"}),
              listDir(dir));
    struct stat st;
    ASSERT_EQ(0, stat(first.c_str(), &st));
    EXPECT_EQ(1000000, st.st_mtime);
    EXPECT_EQ("monkey\nmonkey\n", slurp(first));
}

TEST(ScummvmList, LongNamesCutOnACharacter) {
    std::string d;
    for (int i = 0; i < 100; i++) d += "\xC3\xA9";   // é, two bytes each
    const std::string n = fileNames({game("e", "e", d)})[0];
    const std::string stem = n.substr(0, n.size() - 8);
    EXPECT_LE(stem.size(), 150u);
    EXPECT_EQ(0u, stem.size() % 2);
}

TEST(ScummvmList, FileText) {
    EXPECT_EQ("monkey2-fr\nmonkey2\n", fileText(game("monkey2-fr", "monkey2", "x")));
}

TEST(ScummvmList, GameIdOnlyFromTheMirror) {
    TempDir t("scummvmlist");
    const std::string p = t / "Beneath a Steel Sky.scummvm";
    ASSERT_TRUE(syncDir(t.path(), {game("sky", "sky", "Beneath a Steel Sky")}));
    EXPECT_EQ("sky\nsky\n", slurp(p));
    EXPECT_EQ("", mirrorGameId(p));   // not under kMirrorRoot
    EXPECT_EQ("sky", mirrorGameId(p, t.path()));   // under the given root: the scrape's game ID
    EXPECT_EQ("", mirrorGameId(p, t.path() + "x"));
    EXPECT_EQ("", mirrorGameId(mirrorDir("org.scummvm.scummvm") + "/missing.scummvm"));
}

TEST(ScummvmList, SyncDirKeepsTheFolderToTheList) {
    TempDir t("scummvmlist");
    const std::string dir = t / "org.scummvm.scummvm/scummvm";
    ASSERT_TRUE(syncDir(dir, {game("sky", "sky", "Beneath a Steel Sky (CD/DOS/English)"),
                              game("queen", "queen", "Flight of the Amazon Queen (CD/DOS/English)")}));
    EXPECT_EQ((std::set<std::string>{"Beneath a Steel Sky.scummvm", "Flight of the Amazon Queen.scummvm"}),
              listDir(dir));
    EXPECT_EQ("queen\nqueen\n", slurp(dir + "/Flight of the Amazon Queen.scummvm"));

    // An unchanged game keeps its file (and its mtime: the scrape index keys on it).
    struct stat before, after;
    ASSERT_EQ(0, stat((dir + "/Beneath a Steel Sky.scummvm").c_str(), &before));
    const struct timespec old[2] = {{1000000, 0}, {1000000, 0}};
    ASSERT_EQ(0, utimensat(AT_FDCWD, (dir + "/Beneath a Steel Sky.scummvm").c_str(), old, 0));
    // A stray file, a stale temporary file, a folder and a non-.scummvm file.
    { const int fd = open((dir + "/Old Game.scummvm").c_str(), O_WRONLY | O_CREAT, 0644); ASSERT_GE(fd, 0); close(fd); }
    { const int fd = open((dir + "/.x.1.tmp").c_str(), O_WRONLY | O_CREAT, 0644); ASSERT_GE(fd, 0); close(fd); }
    ASSERT_EQ(0, utimensat(AT_FDCWD, (dir + "/.x.1.tmp").c_str(), old, 0));
    { const int fd = open((dir + "/.y.2.tmp").c_str(), O_WRONLY | O_CREAT, 0644); ASSERT_GE(fd, 0); close(fd); }
    ASSERT_EQ(0, mkdir((dir + "/Folder.scummvm").c_str(), 0755));
    { const int fd = open((dir + "/notes.txt").c_str(), O_WRONLY | O_CREAT, 0644); ASSERT_GE(fd, 0); close(fd); }

    ASSERT_TRUE(syncDir(dir, {game("sky", "sky", "Beneath a Steel Sky (CD/DOS/English)")}));
    EXPECT_EQ((std::set<std::string>{"Beneath a Steel Sky.scummvm", ".y.2.tmp", "Folder.scummvm", "notes.txt"}),
              listDir(dir));
    ASSERT_EQ(0, stat((dir + "/Beneath a Steel Sky.scummvm").c_str(), &after));
    EXPECT_EQ(1000000, after.st_mtime);

    // A changed target rewrites the file; an empty list empties the folder of games.
    ASSERT_TRUE(syncDir(dir, {game("sky-cd", "sky", "Beneath a Steel Sky (CD/DOS/English)")}));
    EXPECT_EQ("sky-cd\nsky\n", slurp(dir + "/Beneath a Steel Sky.scummvm"));
    ASSERT_TRUE(syncDir(dir, {}));
    EXPECT_EQ((std::set<std::string>{".y.2.tmp", "Folder.scummvm", "notes.txt"}), listDir(dir));
}

TEST(ScummvmList, SyncDirOnPartOfTheListOnlyAdds) {
    TempDir t("scummvmlist");
    const std::string dir = t / "scummvm";
    ASSERT_TRUE(syncDir(dir, {game("sky", "sky", "Beneath a Steel Sky"), game("queen", "queen", "Queen")}));
    // A list read while ScummVM rewrites it: the first game only, and a new one.
    ASSERT_TRUE(syncDir(dir, {game("sky", "sky", "Beneath a Steel Sky"), game("dig", "dig", "The Dig")}, false));
    EXPECT_EQ((std::set<std::string>{"Beneath a Steel Sky.scummvm", "Queen.scummvm", "The Dig.scummvm"}), listDir(dir));
}

TEST(ScummvmList, SyncDirFailsWhenTheFolderCannotBeMade) {
    TempDir t("scummvmlist");
    const std::string file = t / "file";
    { const int fd = open(file.c_str(), O_WRONLY | O_CREAT, 0644); ASSERT_GE(fd, 0); close(fd); }
    EXPECT_FALSE(syncDir(file + "/scummvm", {game("sky", "sky", "x")}));
}
