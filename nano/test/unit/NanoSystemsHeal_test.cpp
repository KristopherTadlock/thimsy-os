/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "NanoSystemsHeal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "nano_test_fs.h"

using namespace android::nanosys;

namespace {

SysCore core(const char* id, const char* coreSo, bool builtin = true) {
    SysCore s;
    s.id = id;
    s.romDir = id;
    s.builtin = builtin;
    s.libretro = true;
    s.coreSo = coreSo;
    return s;
}

SysCore app(const char* id, bool builtin = true) {
    SysCore s;
    s.id = id;
    s.romDir = id;
    s.builtin = builtin;
    return s;
}

// Nano's own table (kXmbSystemDefs in NanoMenuXmb.cpp), its RetroArch systems.
const std::vector<std::pair<std::string, std::string>> kNanoCores = {
    {"nes", "nestopia_libretro_android.so"},          {"snes", "snes9x_libretro_android.so"},
    {"gb", "gambatte_libretro_android.so"},           {"gbc", "gambatte_libretro_android.so"},
    {"gba", "gpsp_libretro_android.so"},              {"genesis", "genesis_plus_gx_libretro_android.so"},
    {"mastersystem", "genesis_plus_gx_libretro_android.so"}, {"gamegear", "genesis_plus_gx_libretro_android.so"},
    {"psx", "pcsx_rearmed_libretro_android.so"},      {"ngpc", "mednafen_ngp_libretro_android.so"},
    {"pico8", "fake08_libretro_android.so"},
};

// What Nano's first start saved (seedSystemsConfig with its own table).
std::vector<SysCore> nanoSeed() {
    std::vector<SysCore> v;
    for (const auto& c : kNanoCores) v.push_back(core(c.first.c_str(), c.second.c_str()));
    v.push_back(app("n64"));
    v.push_back(app("nds"));
    v.push_back(app("psp"));
    v.push_back(app("dreamcast"));
    return v;
}

// The build's systems (Thimsy's mods/nano-systems.txt), a part of them.
std::vector<SysCore> thimsyBundled() {
    return {
        core("nes", "mesen_libretro_android.so"),       core("snes", "snes9x_libretro_android.so"),
        core("gb", "sameboy_libretro_android.so"),      core("gbc", "sameboy_libretro_android.so"),
        core("gba", "mgba_libretro_android.so"),        app("n64"),
        app("nds"),                                     core("genesis", "genesis_plus_gx_libretro_android.so"),
        core("psx", "mednafen_psx_hw_libretro_android.so"), core("pico8", "fake08_libretro_android.so"),
        core("saturn", "yabasanshiro_libretro_android.so"), app("gc"),
    };
}

// The cores Thimsy's setup installs (mods/retroarch-cores.lock), a part of them.
std::function<bool(const std::string&)> thimsyCores(std::vector<std::string>* asked = nullptr) {
    return [asked](const std::string& so) {
        static const std::set<std::string> shipped = {
            "mesen_libretro_android.so",   "fceumm_libretro_android.so",   "snes9x_libretro_android.so",
            "sameboy_libretro_android.so", "gpsp_libretro_android.so",     "mgba_libretro_android.so",
            "genesis_plus_gx_libretro_android.so", "pcsx_rearmed_libretro_android.so",
            "mednafen_psx_hw_libretro_android.so", "mednafen_ngp_libretro_android.so",
            "fake08_libretro_android.so",  "yabasanshiro_libretro_android.so",
            "mednafen_wswan_libretro_android.so",
        };
        if (asked) asked->push_back(so);
        return shipped.count(so) > 0;
    };
}

std::vector<std::pair<std::string, std::string>> named(const std::vector<SysCore>& systems,
                                                       const std::vector<CoreRepair>& repairs) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& r : repairs) out.push_back({systems.at(r.index).id, r.coreSo});
    return out;
}

}  // namespace

// The test-27 report: NES and Game Boy said "emulator not installed" because the config held Nano's
// Nestopia and Gambatte, which Thimsy does not ship.
TEST(NanoSystemsHeal, SeededTableGetsTheBuildsCores) {
    const auto systems = nanoSeed();
    const auto repairs = staleCoreRepairs(systems, thimsyBundled(), kNanoCores, thimsyCores());
    const std::vector<std::pair<std::string, std::string>> want = {
        {"nes", "mesen_libretro_android.so"},
        {"gb", "sameboy_libretro_android.so"},
        {"gbc", "sameboy_libretro_android.so"},
    };
    EXPECT_EQ(named(systems, repairs), want);
}

// A core Nano's table names that the build ships still works: GBA stays on gpSP, PlayStation on
// PCSX ReARMed, though the build's defaults are mGBA and Beetle PSX HW.
TEST(NanoSystemsHeal, InstalledCoreIsKept) {
    std::vector<SysCore> systems = {core("gba", "gpsp_libretro_android.so"), core("psx", "pcsx_rearmed_libretro_android.so")};
    EXPECT_TRUE(staleCoreRepairs(systems, thimsyBundled(), kNanoCores, thimsyCores()).empty());
}

TEST(NanoSystemsHeal, UsersChoiceIsKept) {
    // FCEUmm picked in the Game Systems editor, and a core the user downloaded and later deleted:
    // neither is Nano's default for the system.
    std::vector<SysCore> systems = {core("nes", "fceumm_libretro_android.so"),
                                    core("snes", "bsnes_libretro_android.so")};
    EXPECT_TRUE(staleCoreRepairs(systems, thimsyBundled(), kNanoCores, thimsyCores()).empty());
    // An app chosen for a system Nano runs in RetroArch.
    systems = {app("nes")};
    EXPECT_TRUE(staleCoreRepairs(systems, thimsyBundled(), kNanoCores, thimsyCores()).empty());
}

TEST(NanoSystemsHeal, CustomSystemIsKept) {
    std::vector<SysCore> systems = {core("nes", "nestopia_libretro_android.so", false)};
    EXPECT_TRUE(staleCoreRepairs(systems, thimsyBundled(), kNanoCores, thimsyCores()).empty());
}

// Before the user is unlocked no core can be read: nothing changes then, the repair waits.
TEST(NanoSystemsHeal, NothingWhileNoCoreIsReadable) {
    const auto none = [](const std::string&) { return false; };
    EXPECT_TRUE(staleCoreRepairs(nanoSeed(), thimsyBundled(), kNanoCores, none).empty());
}

TEST(NanoSystemsHeal, BuildCoreMustBeInstalled) {
    auto bundled = thimsyBundled();
    for (auto& b : bundled)
        if (b.id == "nes") b.coreSo = "quicknes_libretro_android.so";   // not installed
    std::vector<SysCore> systems = {core("nes", "nestopia_libretro_android.so")};
    EXPECT_TRUE(staleCoreRepairs(systems, bundled, kNanoCores, thimsyCores()).empty());
}

TEST(NanoSystemsHeal, BuildRunningAnAppIsNotACoreRepair) {
    std::vector<SysCore> bundled = {app("nes")};
    std::vector<SysCore> systems = {core("nes", "nestopia_libretro_android.so")};
    EXPECT_TRUE(staleCoreRepairs(systems, bundled, kNanoCores, thimsyCores()).empty());
}

// A build without its own systems file (GammaOS Next) has nothing to repair to.
TEST(NanoSystemsHeal, NoBundledSystems) {
    EXPECT_TRUE(staleCoreRepairs(nanoSeed(), {}, kNanoCores, thimsyCores()).empty());
}

TEST(NanoSystemsHeal, AsksOnlyAboutCandidates) {
    std::vector<std::string> asked;
    std::vector<SysCore> systems = {core("snes", "bsnes_libretro_android.so"), app("nds"),
                                    core("nes", "nestopia_libretro_android.so")};
    staleCoreRepairs(systems, thimsyBundled(), kNanoCores, thimsyCores(&asked));
    const std::vector<std::string> want = {"nestopia_libretro_android.so", "mesen_libretro_android.so"};
    EXPECT_EQ(asked, want);
}

TEST(NanoSystemsHeal, MissingBundledSystems) {
    // Saturn and GameCube are the build's; Nano's seed has neither.
    const auto bundled = thimsyBundled();
    std::vector<std::string> ids;
    for (size_t i : missingBundledSystems(nanoSeed(), bundled)) ids.push_back(bundled.at(i).id);
    EXPECT_EQ(ids, (std::vector<std::string>{"saturn", "gc"}));
}

TEST(NanoSystemsHeal, MissingBundledSystemsMatchesIdOrFolder) {
    std::vector<SysCore> systems = nanoSeed();
    SysCore saturn = core("saturn_2", "mednafen_saturn_libretro_android.so", false);
    saturn.romDir = "Saturn";   // the user's own system for the same folder
    systems.push_back(saturn);
    systems.push_back(core("gc", "dolphin_libretro_android.so", false));   // a custom system with the id
    EXPECT_TRUE(missingBundledSystems(systems, thimsyBundled()).empty());
}

TEST(NanoSystemsHeal, MissingBundledSystemsSkipsCustomOnes) {
    std::vector<SysCore> bundled = {core("extra", "mesen_libretro_android.so", false)};
    EXPECT_TRUE(missingBundledSystems({}, bundled).empty());
}

namespace {

// A system the emulator catalog made from a card folder (gsAutoAddFromRoot after a microSD move):
// the catalog's platform id as id and folder, the card folder chosen.
SysCore catalogMade(const char* id, const char* shortname, const char* cardFolder) {
    SysCore s = core(id, "", false);
    s.libretro = false;
    s.shortname = shortname;
    s.folders = {std::string("/storage/1234-ABCD/ROMs/") + cardFolder};
    return s;
}

std::vector<SysCore> moreBundled() {
    std::vector<SysCore> v = {core("colecovision", "gearcoleco_libretro_android.so"), app("n3ds"),
                              core("pcenginecd", "mednafen_pce_fast_libretro_android.so"),
                              core("pcengine", "mednafen_pce_fast_libretro_android.so"),
                              core("fds", "mesen_libretro_android.so")};
    v[0].shortname = "CV";
    v[1].shortname = "3DS";
    v[2].shortname = "TGCD";
    v[3].shortname = "TG16";
    v[4].shortname = "FDS";
    return v;
}

std::vector<std::string> missingIds(const std::vector<SysCore>& systems, const std::vector<SysCore>& bundled) {
    std::vector<std::string> ids;
    for (size_t i : missingBundledSystems(systems, bundled)) ids.push_back(bundled.at(i).id);
    return ids;
}

}  // namespace

TEST(NanoSystemsHeal, CatalogColecoIsColecoVision) {
    auto systems = nanoSeed();
    systems.push_back(catalogMade("coleco", "COL", "coleco"));
    const auto ids = missingIds(systems, moreBundled());
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "colecovision"), 0);
}

TEST(NanoSystemsHeal, Catalog3dsIsN3ds) {
    auto systems = nanoSeed();
    systems.push_back(catalogMade("3ds", "3DS", "3ds"));
    const auto ids = missingIds(systems, moreBundled());
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "n3ds"), 0);
}

TEST(NanoSystemsHeal, CatalogTgcdIsPcEngineCd) {
    auto systems = nanoSeed();
    systems.push_back(catalogMade("tgcd", "TGCD", "tgcd"));
    const auto ids = missingIds(systems, moreBundled());
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "pcenginecd"), 0);
    // TurboGrafx-16 is another system.
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "pcengine"), 1);
}

// A second catalog system for the platform gets a numbered id; its card folder still names it.
TEST(NanoSystemsHeal, NumberedCatalogSystemMatchesByItsFolder) {
    auto systems = nanoSeed();
    systems.push_back(catalogMade("coleco_2", "COL", "ColecoVision"));
    const auto ids = missingIds(systems, moreBundled());
    EXPECT_EQ(std::count(ids.begin(), ids.end(), "colecovision"), 0);
}

TEST(NanoSystemsHeal, OverlappingFoldersAreOneSystem) {
    SysCore a = core("mine", "fceumm_libretro_android.so", false);
    a.folders = {"/storage/1234-ABCD/Games/Retro"};
    SysCore b = core("theirs", "mesen_libretro_android.so");
    b.folders = {"/storage/1234-abcd/games/retro/nes/"};
    EXPECT_TRUE(sameSystem(a, b));
    EXPECT_TRUE(sameSystem(b, a));
    b.folders = {"/storage/1234-ABCD/Games/RetroArch"};   // a neighbour, not inside
    EXPECT_FALSE(sameSystem(a, b));
}

// The neighbouring systems stay apart: restored beside one another.
TEST(NanoSystemsHeal, RelatedSystemsAreNotTheSame) {
    EXPECT_FALSE(sameSystem(core("nes", ""), core("fds", "")));
    EXPECT_FALSE(sameSystem(core("gb", ""), core("gbc", "")));
    EXPECT_FALSE(sameSystem(core("pcengine", ""), core("pcenginecd", "")));
    EXPECT_FALSE(sameSystem(core("genesis", ""), core("segacd", "")));
    EXPECT_FALSE(sameSystem(core("wonderswan", ""), core("wonderswancolor", "")));
    const auto ids = missingIds(nanoSeed(), moreBundled());
    EXPECT_EQ(ids, (std::vector<std::string>{"colecovision", "n3ds", "pcenginecd", "pcengine", "fds"}));
}

// WiiWare and MSU-MD games draw with their console's art, but are systems of their own: the build's
// Wii and Genesis still come back beside them.
TEST(NanoSystemsHeal, GamesForAConsoleAreNotTheConsole) {
    EXPECT_FALSE(sameSystem(catalogMade("wiiware", "WIIWARE", "wiiware"), app("wii")));
    EXPECT_FALSE(sameSystem(catalogMade("genesismsu", "MSU-MD", "genesismsu"), core("genesis", "")));
    std::vector<SysCore> systems = {catalogMade("wiiware", "WIIWARE", "wiiware")};
    EXPECT_EQ(missingIds(systems, {app("wii")}), (std::vector<std::string>{"wii"}));
}

TEST(NanoSystemsHeal, AliasesKeepTheGivenFolderFirst) {
    const auto a = romFolderAliases("N3DS");
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a.front(), "N3DS");
    EXPECT_NE(std::find(a.begin(), a.end(), "3ds"), a.end());
}

namespace {

// WonderSwan's catalog players (ps3xmb/daijishou/WonderSwan.json) as Nano keeps them: the 64-bit
// RetroArch one and the app, uid prefix dropped.
std::vector<Player> wonderSwanPlayers() {
    Player app;
    app.name = "com.explusalpha.SwanEmu";
    app.package = "com.explusalpha.SwanEmu";
    Player ra;
    ra.name = "Retroarch 64 - Beetle Cygne";
    ra.libretro = true;
    ra.coreSo = "mednafen_wswan_libretro_android.so";
    return {app, ra};
}

// What the microSD move's auto-add made of a card's ws folder before it checked players.
SysCore swanEmuSystem() {
    SysCore s = catalogMade("ws", "WS", "ws");
    s.package = "com.explusalpha.SwanEmu";
    return s;
}

const auto kNoApps = [](const std::string&) { return false; };

}  // namespace

TEST(NanoSystemsHeal, SwanEmuSystemGetsBeetleCygne) {
    const auto players = wonderSwanPlayers();
    const int pi = playerRepair(swanEmuSystem(), players, thimsyCores(), kNoApps);
    ASSERT_GE(pi, 0);
    EXPECT_TRUE(players[pi].libretro);
    EXPECT_EQ(players[pi].coreSo, "mednafen_wswan_libretro_android.so");
}

TEST(NanoSystemsHeal, NothingWhenNoPlayerRuns) {
    const auto none = [](const std::string&) { return false; };   // also: before unlock, no core readable
    EXPECT_EQ(playerRepair(swanEmuSystem(), wonderSwanPlayers(), none, kNoApps), -1);
}

TEST(NanoSystemsHeal, InstalledPlayerIsKept) {
    const auto swanEmu = [](const std::string& pkg) { return pkg == "com.explusalpha.SwanEmu"; };
    EXPECT_EQ(playerRepair(swanEmuSystem(), wonderSwanPlayers(), thimsyCores(), swanEmu), -1);
    // A core that is installed, whoever chose it.
    SysCore s = catalogMade("ws", "WS", "ws");
    s.libretro = true;
    s.coreSo = "mednafen_wswan_libretro_android.so";
    EXPECT_EQ(playerRepair(s, wonderSwanPlayers(), thimsyCores(), kNoApps), -1);
}

TEST(NanoSystemsHeal, PlayerRepairLeavesOtherSystems) {
    // Built-in systems are the core repair's.
    SysCore builtin = swanEmuSystem();
    builtin.builtin = true;
    EXPECT_EQ(playerRepair(builtin, wonderSwanPlayers(), thimsyCores(), kNoApps), -1);
    // A launch the catalog does not have for the platform (typed in by the user).
    SysCore typed = swanEmuSystem();
    typed.package = "org.example.myswan";
    EXPECT_EQ(playerRepair(typed, wonderSwanPlayers(), thimsyCores(), kNoApps), -1);
}

TEST(NanoSystemsHeal, PlayerChoicesListRetroArchFirst) {
    // Both run: the core comes first though the catalog spells it "Retroarch".
    const auto players = wonderSwanPlayers();
    bool noneReady = true;
    const auto order = playerChoices(players, [](const Player&) { return true; }, &noneReady);
    EXPECT_FALSE(noneReady);
    EXPECT_EQ(order, (std::vector<size_t>{1, 0}));
    // None runs: all of them, the same order.
    const auto all = playerChoices(players, [](const Player&) { return false; }, &noneReady);
    EXPECT_TRUE(noneReady);
    EXPECT_EQ(all, (std::vector<size_t>{1, 0}));
}

TEST(NanoSystemsHeal, PlayerLabel) {
    EXPECT_EQ(playerLabel("RetroArch 64 - mgba"), "mgba (RetroArch)");
    EXPECT_EQ(playerLabel("com.explusalpha.SwanEmu"), "com.explusalpha.SwanEmu");
    EXPECT_EQ(playerLabel("Retroarch 64 - Beetle Cygne"), "Beetle Cygne (RetroArch)");
}

// Neo Geo CD discs need a CD core: a neogeocd folder is not FBNeo's Neo Geo system.
TEST(NanoSystemsHeal, ArcadeBoardsAreSystemsOfTheirOwn) {
    EXPECT_FALSE(sameSystem(core("neogeo", "fbneo_libretro_android.so"), catalogMade("neogeocd", "NGCD", "neogeocd")));
    EXPECT_FALSE(sameSystem(catalogMade("naomi", "NAOMI", "naomi"), catalogMade("naomi2", "NAOMI2", "naomi2")));
    EXPECT_FALSE(sameSystem(catalogMade("naomi", "NAOMI", "naomi"), catalogMade("atomiswave", "AW", "atomiswave")));
    EXPECT_FALSE(sameSystem(catalogMade("stv", "STV", "stv"), core("saturn", "yabasanshiro_libretro_android.so")));
    EXPECT_FALSE(sameSystem(catalogMade("stv", "STV", "stv"), core("arcade", "mamearcade_libretro_android.so")));
    EXPECT_FALSE(sameSystem(catalogMade("cps1", "CPS1", "cps1"), catalogMade("cps2", "CPS2", "cps2")));
    // ES-DE's other folder names for the same boards.
    EXPECT_TRUE(sameSystem(catalogMade("neogeocd", "NGCD", "neogeocdjp"), catalogMade("neogeocd", "NGCD", "neogeocd")));
    EXPECT_TRUE(sameSystem(catalogMade("naomi", "NAOMI", "naomigd"), catalogMade("naomi", "NAOMI", "naomi")));
}

// The folder auto-add's platform for a ROM folder, the catalog's platforms in its order (by name).
TEST(NanoSystemsHeal, PlatformForFolder) {
    const std::vector<std::string> ids = {"fbneo", "mame", "varcade", "atomiswave", "cps1", "neogeo", "neogeocd",
                                          "naomi", "naomi2", "stv", "ps3", "switch"};
    auto platform = [&](const char* folder) {
        const int i = platformForFolder(folder, ids);
        return i < 0 ? std::string() : ids[i];
    };
    EXPECT_EQ(platform("neogeo"), "neogeo");
    EXPECT_EQ(platform("neogeocd"), "neogeocd");
    EXPECT_EQ(platform("NeoGeoCDJP"), "neogeocd");
    EXPECT_EQ(platform("neocd"), "neogeocd");   // the folder FBNeo needs for discs
    EXPECT_EQ(platform("naomigd"), "naomi");
    EXPECT_EQ(platform("naomi2"), "naomi2");
    EXPECT_EQ(platform("STV"), "stv");
    EXPECT_EQ(platform("arcade"), "mame");
    EXPECT_EQ(platform("fba"), "fbneo");
    EXPECT_EQ(platform("TATE"), "varcade");
    EXPECT_EQ(platform("playstation3"), "ps3");
    EXPECT_EQ(platform("media"), "");
    EXPECT_EQ(platform(""), "");
}

// A folder of games with a catalog platform of its own is another system, never folded into the
// console's: SuperGrafx (not Beetle PCE Fast), ZX81 (not Fuse), the SNES and N64 add-ons, Super
// Game Boy.
TEST(NanoSystemsHeal, FoldersGoToTheirOwnPlatform) {
    EXPECT_FALSE(sameSystem(core("pcengine", "mednafen_pce_fast_libretro_android.so"), catalogMade("supergrafx", "SGX", "supergrafx")));
    EXPECT_FALSE(sameSystem(catalogMade("zxspectrum", "ZXS", "zxspectrum"), catalogMade("zx81", "ZX81", "zx81")));
    EXPECT_FALSE(sameSystem(core("gb", "sameboy_libretro_android.so"), catalogMade("sgb", "SGB", "sgb")));
    EXPECT_FALSE(sameSystem(core("snes", "snes9x_libretro_android.so"), catalogMade("satellaview", "BSX", "satellaview")));
    EXPECT_FALSE(sameSystem(core("snes", "snes9x_libretro_android.so"), catalogMade("sufami", "ST", "sufami")));
    EXPECT_FALSE(sameSystem(app("n64"), catalogMade("n64dd", "64DD", "n64dd")));
    EXPECT_FALSE(sameSystem(catalogMade("msx", "MSX", "msx"), catalogMade("msxturbor", "MSXR", "msxturbor")));
    EXPECT_FALSE(sameSystem(catalogMade("c64", "C64", "c64"), catalogMade("c128", "C128", "c128")));
    EXPECT_FALSE(sameSystem(catalogMade("amiga", "AMI", "amiga"), catalogMade("amigacd32", "CD32", "amigacd32")));
    EXPECT_FALSE(sameSystem(catalogMade("windows", "WIN", "windows"), catalogMade("windows9x", "W9X", "windows9x")));
    EXPECT_TRUE(sameSystem(catalogMade("laserdisc", "LD", "daphne"), catalogMade("laserdisc", "LD", "laserdisc")));
    const std::vector<std::string> ids = {"gb", "laserdisc", "msx", "msxturbor", "n64", "n64dd", "satellaview", "sgb", "snes",
                                          "sufami", "supergrafx", "tg16", "zx81", "zxspectrum"};
    auto platform = [&](const char* folder) {
        const int i = platformForFolder(folder, ids);
        return i < 0 ? std::string() : ids[i];
    };
    EXPECT_EQ(platform("supergrafx"), "supergrafx");
    EXPECT_EQ(platform("pcengine"), "tg16");
    EXPECT_EQ(platform("zx81"), "zx81");
    EXPECT_EQ(platform("spectrum"), "zxspectrum");
    EXPECT_EQ(platform("sgb"), "sgb");
    EXPECT_EQ(platform("gameboy"), "gb");
    EXPECT_EQ(platform("snes"), "snes");
    EXPECT_EQ(platform("satellaview"), "satellaview");
    EXPECT_EQ(platform("sufami"), "sufami");
    EXPECT_EQ(platform("64dd"), "n64dd");
    EXPECT_EQ(platform("nintendo64"), "n64");
    EXPECT_EQ(platform("turbor"), "msxturbor");
    EXPECT_EQ(platform("daphne"), "laserdisc");
}

// ---- the split-off folders heal ----

namespace {

void touch(const std::string& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fd, 0) << path;
    close(fd);
}

void mkdirs(const std::string& path) {
    for (size_t p = path.find('/', 1); ; p = path.find('/', p + 1)) {
        mkdir(path.substr(0, p).c_str(), 0755);
        if (p == std::string::npos) break;
    }
}

const std::vector<std::string> kSplitIds = {"snes", "satellaview", "sgb", "sufami", "n64", "n64dd", "neogeo",
                                            "neogeocd", "tg16", "supergrafx", "zxspectrum", "zx81", "c64", "c128"};

std::string extsOf(int platform) {
    const std::string id = kSplitIds[platform];
    if (id == "satellaview") return ".bs,.sfc";
    if (id == "sgb") return ".gb,.gbc";
    if (id == "neogeocd") return ".cue";
    return ".rom";
}

FoundFolder found(const char* name, const std::string& path) {
    FoundFolder f;
    f.name = name;
    f.path = path;
    f.platform = platformForFolder(name, kSplitIds);
    return f;
}

}  // namespace

TEST(NanoSystemsSplitHeal, SplitOffFolders) {
    const auto& f = splitOffFolders();
    for (const char* n : {"satellaview", "sufami", "sgb", "n64dd", "neogeocd", "neogeocdjp", "neocd", "supergrafx", "zx81",
                          "segacd32x", "msxturbor", "spectravideo", "c128", "amigacd32"})
        EXPECT_NE(std::find(f.begin(), f.end(), n), f.end()) << n;
    for (const char* n : {"snes", "n64", "neogeo", "pcengine", "zxspectrum", "gb", "nes"})
        EXPECT_EQ(std::find(f.begin(), f.end(), n), f.end()) << n;
    // Each is a group of its own now: none is another folder's alias any more.
    for (const auto& n : f)
        for (const auto& a : romFolderAliases(n))
            for (const char* parent : {"snes", "n64", "neogeo", "pcengine", "zxspectrum", "gb", "c64", "amiga", "msx"})
                EXPECT_NE(a, parent) << n;
}

TEST(NanoSystemsSplitHeal, FolderHasRom) {
    nanotest::TempDir t("splitheal");
    mkdirs(t / "empty");
    mkdirs(t / "other");
    touch(t / "other/readme.txt");
    touch(t / "other/.hidden.bs");
    mkdirs(t / "bs");
    touch(t / "bs/Game.BS");
    mkdirs(t / "deep/Game Folder");
    touch(t / "deep/Game Folder/Disc.cue");
    mkdirs(t / "zipped");
    touch(t / "zipped/Game.zip");
    EXPECT_FALSE(folderHasRom(t / "empty", ".bs,.sfc"));
    EXPECT_FALSE(folderHasRom(t / "other", ".bs,.sfc"));
    EXPECT_TRUE(folderHasRom(t / "bs", ".bs,.sfc"));
    EXPECT_FALSE(folderHasRom(t / "bs", ".gb"));
    EXPECT_TRUE(folderHasRom(t / "deep", ".cue"));
    EXPECT_TRUE(folderHasRom(t / "zipped", ".gb"));
    EXPECT_FALSE(folderHasRom(t / "missing", ".bs"));
}

TEST(NanoSystemsSplitHeal, FindsSplitOffFoldersWithGames) {
    nanotest::TempDir internal("splitint"), card("splitcard");
    mkdirs(internal / "satellaview");
    touch(internal / "satellaview/Game.bs");
    mkdirs(internal / "sgb");                       // empty: no system
    mkdirs(internal / "snes");                      // not split off
    touch(internal / "snes/Game.sfc");
    mkdirs(internal / "neocd");
    touch(internal / "neocd/notes.txt");            // no game of its platform
    mkdirs(card / "Satellaview");                   // any case
    touch(card / "Satellaview/Other.sfc");
    mkdirs(card / "NeoGeoCDJP/Game");
    touch(card / "NeoGeoCDJP/Game/Game.cue");
    mkdirs(card / "segacd32x");                     // no catalog platform here
    touch(card / "segacd32x/Game.rom");
    touch(card / "zx81");                           // a file, not a folder
    auto got = findSplitOffFolders({internal.path(), card.path(), card / "missing"}, kSplitIds, extsOf);
    std::sort(got.begin(), got.end(), [](const FoundFolder& a, const FoundFolder& b) { return a.path < b.path; });
    std::vector<std::string> paths, platforms;
    for (const auto& f : got) {
        paths.push_back(f.path);
        platforms.push_back(kSplitIds[f.platform]);
    }
    std::vector<std::string> wantPaths = {internal / "satellaview", card / "NeoGeoCDJP", card / "Satellaview"};
    std::sort(wantPaths.begin(), wantPaths.end());
    EXPECT_EQ(paths, wantPaths);
    for (size_t i = 0; i < got.size(); i++)
        EXPECT_EQ(platforms[i], got[i].path.find("NeoGeo") != std::string::npos ? "neogeocd" : "satellaview");
}

TEST(NanoSystemsSplitHeal, AddsOneSystemPerPlatform) {
    const std::vector<SysCore> systems = {core("snes", "snes9x_libretro_android.so"), app("n64"),
                                          core("neogeo", "fbneo_libretro_android.so"),
                                          core("pcengine", "mednafen_pce_fast_libretro_android.so")};
    const std::vector<FoundFolder> f = {
        found("satellaview", "/storage/emulated/0/ROMs/satellaview"),
        found("neocd", "/storage/emulated/0/ROMs/neocd"),
        found("Satellaview", "/storage/1234-ABCD/ROMs/Satellaview"),
        found("supergrafx", "/storage/emulated/0/ROMs/supergrafx"),
        found("satellaview", "/storage/emulated/0/ROMs/satellaview"),   // found twice
        found("media", "/storage/emulated/0/ROMs/media"),              // no platform
    };
    const auto adds = splitFolderAdds(systems, f, kSplitIds);
    ASSERT_EQ(adds.size(), 3u);
    EXPECT_EQ(kSplitIds[adds[0].platform], "satellaview");
    EXPECT_EQ(adds[0].folders, (std::vector<std::string>{"/storage/emulated/0/ROMs/satellaview",
                                                         "/storage/1234-ABCD/ROMs/Satellaview"}));
    EXPECT_EQ(kSplitIds[adds[1].platform], "neogeocd");
    EXPECT_EQ(adds[1].folders, (std::vector<std::string>{"/storage/emulated/0/ROMs/neocd"}));
    EXPECT_EQ(kSplitIds[adds[2].platform], "supergrafx");
}

// Never a second system for games a system already shows.
TEST(NanoSystemsSplitHeal, NoDuplicates) {
    const std::vector<FoundFolder> f = {found("satellaview", "/storage/emulated/0/ROMs/satellaview"),
                                        found("sgb", "/storage/1234-ABCD/ROMs/sgb"),
                                        found("n64dd", "/storage/emulated/0/ROMs/n64dd")};
    // A Satellaview system the folder auto-add made, a system with that card folder as a source, a
    // system scanning the whole ROMs folder.
    SysCore bsx = catalogMade("satellaview", "BSX", "satellaview");
    SysCore gb = core("gb", "sameboy_libretro_android.so");
    gb.folders = {"/storage/1234-ABCD/ROMs/sgb"};
    SysCore all = app("n64");
    all.folders = {"/storage/emulated/0/ROMs"};
    EXPECT_TRUE(splitFolderAdds({bsx, gb, all}, f, kSplitIds).empty());
    // The same, with the Satellaview one under another folder name: the same platform.
    SysCore bsx2 = catalogMade("satellaview2", "BSX", "bsx");
    const auto adds = splitFolderAdds({bsx2}, f, kSplitIds);
    ASSERT_EQ(adds.size(), 2u);
    EXPECT_EQ(kSplitIds[adds[0].platform], "sgb");
    EXPECT_EQ(kSplitIds[adds[1].platform], "n64dd");
    // And nothing without folders.
    EXPECT_TRUE(splitFolderAdds({}, {}, kSplitIds).empty());
}

// The heal waits for every drive vold has mounted to be mounted for apps too, so a card's folders are
// looked for only once they can be seen.
TEST(NanoSystemsSplitHeal, DrivesServed) {
    nanotest::TempDir rw("splitrw");
    const std::string internal = "/dev/fuse /storage/emulated fuse rw,lazytime 0 0\n";
    EXPECT_TRUE(drivesServed(rw / "missing", ""));
    EXPECT_TRUE(drivesServed(rw.path(), internal));   // no drive
    mkdirs(rw / "1234-ABCD");
    touch(rw / "stray");                               // not a drive
    EXPECT_FALSE(drivesServed(rw.path(), internal));
    EXPECT_FALSE(drivesServed(rw.path(), internal + "/dev/block/vold/public:179,1 /mnt/media_rw/1234-ABCD vfat rw 0 0\n"));
    EXPECT_FALSE(drivesServed(rw.path(), internal + "/dev/fuse /storage/1234-ABCD/sub fuse rw 0 0\n"));
    EXPECT_TRUE(drivesServed(rw.path(), internal + "/dev/fuse /storage/1234-ABCD fuse rw,nosuid 0 0"));
    mkdirs(rw / "5678-EF01");
    EXPECT_FALSE(drivesServed(rw.path(), internal + "/dev/fuse /storage/1234-ABCD fuse rw 0 0\n"));
    EXPECT_TRUE(drivesServed(rw.path(), "/dev/fuse /storage/5678-EF01 fuse rw 0 0\n/dev/fuse /storage/1234-ABCD fuse rw 0 0\n"));
}
