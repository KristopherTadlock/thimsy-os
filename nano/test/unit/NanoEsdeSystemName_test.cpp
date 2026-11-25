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

#include "NanoEsdeSystemName.h"
#include "NanoJson.h"

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <ziparchive/zip_archive.h>

#include "nano_test_util.h"

using namespace android::esdesys;
namespace njson = android::njson;

namespace {

// Catalog platforms ES-DE has no system for: themes look their art up under the platform id. Slate
// has none (it draws its default); the Thimsy build adds Colorful pictures for them.
const std::set<std::string> kNoEsdeSystem = {
    "advision", "apf", "atom", "cannonball", "cavestory", "chip8", "ebook", "elektor", "flashback", "idtech",
    "microvision", "moonlight", "namcops2", "pet", "pico", "quake2", "studio2", "varcade", "vc4000", "videos",
    "xcloud",
};

std::string dataDir() {
    const std::string fx = nanotest::fixtureDir();
    return fx.substr(0, fx.size() - strlen("/test/fixtures"));
}

// The uniqueId of every platform in Nano's emulator catalog.
std::vector<std::string> catalogPlatforms() {
    std::vector<std::string> out;
    const std::string dir = dataDir() + "/ps3xmb/daijishou";
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    for (dirent* e = readdir(d); e; e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() <= 5 || n.compare(n.size() - 5, 5, ".json") != 0) continue;
        std::ifstream f(dir + "/" + n);
        std::stringstream ss;
        ss << f.rdbuf();
        njson::Value root;
        if (!njson::parse(ss.str(), &root) || !root.isObject()) continue;
        const njson::Value* pl = root.find("platform");
        if (pl && pl->isObject() && !pl->getString("uniqueId").empty()) out.push_back(pl->getString("uniqueId"));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// The member names of a zip, relative to the theme set (the folder holding capabilities.xml).
std::set<std::string> zipNames(const std::string& zip) {
    std::vector<std::string> all;
    ZipArchiveHandle za = nullptr;
    if (OpenArchive(zip.c_str(), &za) != 0) { CloseArchive(za); return {}; }
    void* cookie = nullptr;
    if (StartIteration(za, &cookie) == 0) {
        ZipEntry64 e;
        std::string name;
        while (Next(cookie, &e, &name) == 0) all.push_back(name);
        EndIteration(cookie);
    }
    CloseArchive(za);
    std::string top;
    for (const auto& n : all) {
        const size_t sl = n.find('/');
        if (n == "capabilities.xml") { top.clear(); break; }
        if (sl != std::string::npos && n.compare(sl, std::string::npos, "/capabilities.xml") == 0) top = n.substr(0, sl + 1);
    }
    std::set<std::string> names;
    for (const auto& n : all)
        if (n.size() > top.size() && n.compare(0, top.size(), top) == 0) names.insert(n.substr(top.size()));
    return names;
}

// The catalog platforms whose theme system has no art at pathOf(system) in the zip.
std::vector<std::string> platformsWithoutArt(const std::set<std::string>& names,
                                             std::string (*pathOf)(const std::string&)) {
    std::vector<std::string> missing;
    for (const auto& uid : catalogPlatforms()) {
        if (kNoEsdeSystem.count(uid)) continue;
        if (!names.count(pathOf(esdeThemeSystemName(uid, std::string())))) missing.push_back(uid);
    }
    return missing;
}

std::string slateLogo(const std::string& sys) { return sys + "/images/logo.svg"; }
std::string colorfulArt(const std::string& sys) { return "_inc/systems/images/" + sys + ".webp"; }

}  // namespace

TEST(EsdeSystemName, KnownNamesAndFolderAliases) {
    EXPECT_EQ("colecovision", esdeThemeSystemName("colecovision", "CV"));
    EXPECT_EQ("psx", esdeThemeSystemName("PS", ""));
    EXPECT_EQ("atarilynx", esdeThemeSystemName("lynx", ""));
    EXPECT_EQ("megadrive", esdeThemeSystemName("", "MD"));
    EXPECT_EQ("Coleco ColecoVision", esdeSystemFullName("colecovision"));
    EXPECT_EQ("", esdeSystemFullName("coleco"));
}

// A system the microSD move or a folder scan adds from the catalog keeps the platform id as its
// folder; themes name their art after the ES-DE system.
TEST(EsdeSystemName, CatalogPlatformIdsDrawAsTheirEsdeSystem) {
    EXPECT_EQ("colecovision", esdeThemeSystemName("coleco", "COLECOVISION"));
    EXPECT_EQ("colecovision", esdeThemeSystemName("COLECO", ""));
    EXPECT_EQ("n3ds", esdeThemeSystemName("3ds", ""));
    EXPECT_EQ("tg-cd", esdeThemeSystemName("tgcd", ""));
    EXPECT_EQ("mastersystem", esdeThemeSystemName("master", ""));
    EXPECT_EQ("psvita", esdeThemeSystemName("vita", ""));
    EXPECT_EQ("Nintendo 3DS", esdeSystemFullName(esdeThemeSystemName("3ds", "")));
    // gamelists/ and downloaded_media/ keep the folders they always used.
    EXPECT_EQ("coleco", esdeCanonSystemName("coleco", ""));
    EXPECT_EQ("my_games", esdeThemeSystemName("my_games", "MG"));
}

// WiiWare draws with the Wii's art, though it is not the Wii system.
TEST(EsdeSystemName, GamesForAConsoleDrawAsTheConsole) {
    EXPECT_EQ("wii", esdeThemeSystemName("wiiware", ""));
    EXPECT_EQ("wiiware", esdePlatformSystemName("wiiware", ""));
    EXPECT_EQ("genesis", esdeThemeSystemName("genesismsu", ""));
    EXPECT_EQ("genesismsu", esdePlatformSystemName("genesismsu", ""));
    EXPECT_EQ("colecovision", esdePlatformSystemName("coleco", ""));
    EXPECT_EQ("n3ds", esdePlatformSystemName("3ds", ""));
}

// The catalog's pico is the Sega Pico, which ES-DE has no system for; PICO-8 is pico8.
TEST(EsdeSystemName, SegaPicoIsNotPico8) {
    EXPECT_EQ("pico", esdeThemeSystemName("pico", ""));
    EXPECT_EQ("pico", esdePlatformSystemName("pico", ""));
    EXPECT_EQ("pico", esdeCanonSystemName("pico", ""));
    EXPECT_EQ("pico8", esdeThemeSystemName("pico8", ""));
    EXPECT_EQ("pico8", esdeThemeSystemName("PICO8", ""));
}

// The Videopac+ draws with the Videopac's art, though it is not the Videopac system.
TEST(EsdeSystemName, VideopacPlusDrawsAsTheVideopac) {
    EXPECT_EQ("videopac", esdeThemeSystemName("g7400", ""));
    EXPECT_EQ("g7400", esdePlatformSystemName("g7400", ""));
}

// Boards and models ES-DE has no system for draw as their family's when it is the same machine; the
// Namco System 246 keeps its own name (the Thimsy build draws it rather than a generic arcade).
TEST(EsdeSystemName, ModelsDrawAsTheirFamily) {
    EXPECT_EQ("c64", esdeThemeSystemName("c128", ""));
    EXPECT_EQ("c128", esdePlatformSystemName("c128", ""));
    EXPECT_EQ("sega32x", esdeThemeSystemName("segacd32x", ""));
    EXPECT_EQ("namcops2", esdeThemeSystemName("namcops2", ""));
}

// ES-DE calls the Bally Astrocade astrocde; themes name its art astrocade.
TEST(EsdeSystemName, AstrocadeArtIsNamedAstrocade) {
    EXPECT_EQ("astrocade", esdeThemeSystemName("astrocade", ""));
    EXPECT_EQ("astrocade", esdeThemeSystemName("astrocde", ""));
    EXPECT_EQ("astrocde", esdePlatformSystemName("astrocade", ""));
    EXPECT_EQ("Bally Astrocade", esdeSystemFullName(esdeThemeSystemName("astrocade", "")));
    EXPECT_EQ("Bally Astrocade", esdeSystemFullName("astrocde"));
}

TEST(EsdeSystemName, EmulatorKeys) {
    EXPECT_EQ("core:snes9x", esdeEmulatorKey(true, "snes9x_libretro_android.so", ""));
    EXPECT_EQ("core:snes9x", esdeEmulatorKey(true, "/data/data/x/cores/snes9x_libretro_android.so", "x"));
    EXPECT_EQ("", esdeEmulatorKey(true, "snes9x.so", ""));
    EXPECT_EQ("pkg:org.ppsspp.ppsspp", esdeEmulatorKey(false, "", "org.ppsspp.ppsspp"));
    EXPECT_EQ("", esdeEmulatorKey(false, "snes9x_libretro_android.so", ""));
}

// Nano's emulators by the labels ES-DE gives them (a gamelist's <altemulator> and <alternativeEmulator>).
TEST(EsdeSystemName, EmulatorLabels) {
    EXPECT_EQ("Snes9x - Current", esdeLabelForKey("snes", "core:snes9x"));
    EXPECT_EQ("Beetle PSX HW", esdeLabelForKey("psx", "core:mednafen_psx_hw"));
    EXPECT_EQ("PPSSPP (Standalone)", esdeLabelForKey("psp", "pkg:org.ppsspp.ppsspp"));
    EXPECT_EQ("M64Plus FZ (Standalone)", esdeLabelForKey("n64", "pkg:org.mupen64plusae.v3.fzurita"));
    // melonDS's own command can start WatermelonDS too; WatermelonDS's is the one made for it.
    EXPECT_EQ("WatermelonDS (Standalone)", esdeLabelForKey("nds", "pkg:me.magnum.melondualds"));
    EXPECT_EQ("SeedlessDS (Standalone)", esdeLabelForKey("nds", "pkg:com.seedlessds.app"));
    // One app with a command per media type: none of them is the app's.
    EXPECT_EQ("", esdeLabelForKey("adam", "pkg:com.seleuco.mame4d2024"));
    EXPECT_EQ("Snes9x - Current", esdeDefaultLabel("snes"));
    EXPECT_EQ("core:mesen-s", esdeKeysForLabel("snes", "Mesen-S").at(0));
    EXPECT_EQ("", esdeLabelForKey("snes", "core:not_a_core"));
    EXPECT_EQ("", esdeLabelForKey("not_a_system", "core:snes9x"));
    EXPECT_EQ(std::vector<std::string>({"core:snes9x"}), esdeKeysForLabel("snes", "Snes9x - Current"));
    EXPECT_EQ(std::vector<std::string>(), esdeKeysForLabel("snes", "No Such Emulator"));
}

TEST(EsdeSystemName, CommandsAndDefinitions) {
    const std::vector<EsdeCommand> cmds = esdeSystemCommands("3do");
    ASSERT_FALSE(cmds.empty());
    EXPECT_EQ("Opera", cmds[0].label);   // ES-DE's default first
    EXPECT_NE(std::string::npos, cmds[0].command.find("/cores/opera_libretro_android.so %EXTRA_ROM%=%ROM%"));
    EXPECT_EQ(0u, cmds[0].command.find("%EMULATOR_RETROARCH% "));
    std::string ext, platform, theme;
    ASSERT_TRUE(esdeSystemDef("n64dd", &ext, &platform, &theme));
    EXPECT_NE(std::string::npos, ext.find(".z64"));
    EXPECT_EQ("n64", platform);
    EXPECT_EQ("n64dd", theme);
    EXPECT_FALSE(esdeSystemDef("not_a_system", &ext, &platform, &theme));
}

// The emulators table and the full names table are from the same es_systems.xml.
TEST(EsdeSystemName, EveryEsdeSystemHasItsCommands) {
    for (const char* sys : {"3do", "arcade", "gb", "gba", "n3ds", "nds", "psx", "psp", "snes", "tg-cd"}) {
        EXPECT_TRUE(esdeKnownSystem(sys)) << sys;
        EXPECT_FALSE(esdeSystemCommands(sys).empty()) << sys;
        EXPECT_TRUE(esdeSystemDef(sys, nullptr, nullptr, nullptr)) << sys;
    }
}

TEST(EsdeSystemName, EveryCatalogPlatformHasAnEsdeSystemOrIsListed) {
    const std::vector<std::string> uids = catalogPlatforms();
    ASSERT_GT(uids.size(), 100u) << dataDir();
    std::set<std::string> seen;
    for (const auto& uid : uids) {
        seen.insert(uid);
        const std::string sys = esdeThemeSystemName(uid, std::string());
        if (kNoEsdeSystem.count(uid))
            EXPECT_FALSE(esdeKnownSystem(sys)) << uid << " maps to " << sys << ": drop it from kNoEsdeSystem";
        else
            EXPECT_TRUE(esdeKnownSystem(sys) || sys == "astrocade") << uid << " has no ES-DE system: map it in esdeThemeSystemName "
                                                 "or list it in kNoEsdeSystem";
    }
    for (const auto& uid : kNoEsdeSystem) EXPECT_TRUE(seen.count(uid)) << uid << " is not in the catalog";
}

TEST(EsdeSystemName, BundledSlateHasALogoForEveryCatalogPlatform) {
    const std::set<std::string> names = zipNames(dataDir() + "/nano_esde_themes/slate-es-de.zip");
    ASSERT_TRUE(names.count("capabilities.xml")) << "slate-es-de.zip not found under " << dataDir();
    EXPECT_EQ(std::vector<std::string>(), platformsWithoutArt(names, slateLogo));
}

// Colorful ships in the thimsy build repo, not this tree: checked when the build cache is present
// (or NANO_TEST_COLORFUL_ZIP names the zip). The build leaves out quake.webp (id Software art).
TEST(EsdeSystemName, ColorfulHasArtForEveryCatalogPlatform) {
    std::string zip;
    if (const char* env = getenv("NANO_TEST_COLORFUL_ZIP")) zip = env;
    else if (const char* home = getenv("HOME"))
        zip = std::string(home) +
              "/thimsy/work/cache/nano/colorful-simplified-es-de-669e7f6a2de57c5ab69fd865e70d1cd357dbabbb.zip";
    if (access(zip.c_str(), R_OK) != 0) GTEST_SKIP() << zip << " is not on this host";
    std::set<std::string> names = zipNames(zip);
    ASSERT_TRUE(names.count("capabilities.xml")) << zip;
    names.erase(colorfulArt("quake"));
    EXPECT_EQ(std::vector<std::string>({"quake"}), platformsWithoutArt(names, colorfulArt));
}
