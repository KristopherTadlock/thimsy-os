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

// Nano's emulator catalog (ps3xmb/daijishou): every platform can be made from its ROM folder, draws a
// controller, and has players Nano can launch.

#include <dirent.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "NanoEsdeSystemName.h"
#include "NanoJson.h"
#include "NanoLaunch.h"
#include "NanoSystemsHeal.h"
#include "nano_test_util.h"

namespace njson = android::njson;
using nanolaunch::isValidComponent;

namespace {

std::string dataDir() {
    const std::string fx = nanotest::fixtureDir();
    return fx.substr(0, fx.size() - strlen("/test/fixtures"));
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Platform {
    std::string file, uid;
    std::vector<const njson::Value*> players;
    njson::Value root;
};

const std::vector<Platform>& catalog() {
    static const std::vector<Platform> all = [] {
        std::vector<Platform> out;
        const std::string dir = dataDir() + "/ps3xmb/daijishou";
        DIR* d = opendir(dir.c_str());
        if (!d) return out;
        std::vector<std::string> names;
        for (dirent* e = readdir(d); e; e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n);
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        out.reserve(names.size());
        for (const auto& n : names) {
            Platform p;
            p.file = n;
            if (!njson::parse(slurp(dir + "/" + n), &p.root) || !p.root.isObject()) continue;
            const njson::Value* pf = p.root.find("platform");
            p.uid = pf ? pf->getString("uniqueId") : std::string();
            out.push_back(std::move(p));
        }
        for (auto& p : out)
            if (const njson::Value* pl = p.root.find("playerList"); pl && pl->isArray())
                for (const auto& v : pl->arr) p.players.push_back(&v);
        return out;
    }();
    return all;
}

std::vector<std::string> platformIds() {
    std::vector<std::string> ids;
    for (const auto& p : catalog()) {
        std::string l = p.uid;
        for (auto& c : l) c = (char)tolower((unsigned char)c);
        ids.push_back(l);
    }
    return ids;
}

}  // namespace

TEST(Catalog, Loads) {
    ASSERT_GE(catalog().size(), 150u) << dataDir();
    for (const auto& p : catalog()) EXPECT_FALSE(p.uid.empty()) << p.file;
}

// The folder auto-add makes each platform of a folder named like it, and of a folder named as ES-DE
// names its system ("colecovision" is coleco, "daphne" laserdisc).
TEST(Catalog, EachPlatformIsMadeFromItsFolders) {
    const std::vector<std::string> ids = platformIds();
    std::map<std::string, int> esdeUsers;
    for (const auto& id : ids) esdeUsers[android::esdesys::esdePlatformSystemName(id, "")]++;
    for (size_t i = 0; i < ids.size(); i++) {
        SCOPED_TRACE(ids[i]);
        EXPECT_EQ((int)i, android::nanosys::platformForFolder(ids[i], ids));
        const std::string esde = android::esdesys::esdePlatformSystemName(ids[i], "");
        if (esde == ids[i] || !android::esdesys::esdeKnownSystem(esde) || esdeUsers[esde] > 1) continue;
        EXPECT_EQ((int)i, android::nanosys::platformForFolder(esde, ids)) << "ES-DE folder " << esde;
    }
}

// Every player starts a valid component; a placeholder other than {file.*} (Daijisho's {tags.*}) is
// one Nano cannot fill, so only the players that had one before Nano knew of them keep it.
TEST(Catalog, PlayersLaunch) {
    static const std::set<std::string> kTagPlayers = {
        "idtech.com.karin.idTech4Amm", "moonlight.com.limelight", "steam.app.gamenative", "steam.banner.hub",
        "steam.banner.hub.lite", "steam.gamehub.lite", "steam.gamehub.lite.antutu", "steam.gamehub.lite.ludashi",
        "steam.gamehub.lite.pubg", "vita.com.sbro.emucorev", "vita.org.vita3k.emulator",
        "vita.org.vita3k.emulator.ikhoeyZX", "windows.app.gamenative.amazon", "windows.app.gamenative.custom",
        "windows.app.gamenative.epic", "windows.app.gamenative.gog", "windows.app.gamenative.steam",
        "windows.banner.hub.lite.local", "windows.banner.hub.local", "windows.gamehub.lite.local",
        "windows.gamehub.lite.local.antutu", "windows.gamehub.lite.local.pubg", "windows.gamehub.lite.ludashi",
        "xbplay.com.studio08.xbgamestream",
    };
    static const std::set<std::string> kNoPlayers = {"elektor"};   // the catalog has no emulator for it
    const std::regex comp("-n\\s+(\\S+)"), placeholder("\\{[a-z]+\\.[A-Za-z0-9_]+\\}"), exts("\\(\\?:([a-z0-9|]+)\\)");
    std::set<std::string> seen;
    for (const auto& p : catalog()) {
        SCOPED_TRACE(p.file);
        if (!kNoPlayers.count(p.uid)) EXPECT_FALSE(p.players.empty());
        for (const njson::Value* v : p.players) {
            const std::string uid = v->getString("uniqueId"), am = v->getString("amStartArguments");
            SCOPED_TRACE(uid);
            EXPECT_TRUE(seen.insert(uid).second) << "uniqueId used twice";
            std::smatch m;
            ASSERT_TRUE(std::regex_search(am, m, comp));
            EXPECT_TRUE(isValidComponent(m[1].str())) << m[1].str();
            if (!kTagPlayers.count(uid))
                for (auto it = std::sregex_iterator(am.begin(), am.end(), placeholder); it != std::sregex_iterator(); ++it)
                    EXPECT_EQ(0u, it->str().rfind("{file.", 0)) << it->str();
            const std::string rx = v->getString("acceptedFilenameRegex");
            EXPECT_TRUE(std::regex_search(rx, m, exts)) << "no extension list in " << rx;
        }
    }
}

// The button map draws every platform's controller (or none, for the book reader and the like).
TEST(Catalog, EachPlatformHasAControllerDrawing) {
    njson::Value art;
    ASSERT_TRUE(njson::parse(slurp(dataDir() + "/ps3xmb/controllers/controllers.json"), &art));
    const njson::Value* platforms = art.find("platforms");
    const njson::Value* drawings = art.find("drawings");
    ASSERT_TRUE(platforms && drawings);
    for (const auto& p : catalog()) {
        SCOPED_TRACE(p.uid);
        const njson::Value* e = platforms->find(p.uid);
        ASSERT_NE(nullptr, e);
        const njson::Value* d = e->find("drawing");
        ASSERT_NE(nullptr, d);
        if (d->isString()) EXPECT_NE(nullptr, drawings->find(d->str)) << d->str;
    }
}

// Arcade boards and the platforms added with them: their players and the cores Thimsy ships.
TEST(Catalog, ArcadeBoards) {
    std::map<std::string, std::set<std::string>> players;
    for (const auto& p : catalog())
        for (const njson::Value* v : p.players) players[p.uid].insert(v->getString("uniqueId"));
    EXPECT_TRUE(players["stv"].count("stv.ra64.mednafen_saturn"));
    EXPECT_TRUE(players["stv"].count("stv.ra64.mamearcade"));
    EXPECT_TRUE(players["naomi2"].count("naomi2.com.flycast.emulator"));
    EXPECT_TRUE(players["naomi2"].count("naomi2.ra64.flycast"));
    for (const char* cps : {"cps1", "cps2", "cps3"}) {
        EXPECT_TRUE(players[cps].count(std::string(cps) + ".ra64.fbneo")) << cps;
        EXPECT_TRUE(players[cps].count(std::string(cps) + ".ra64.mame2003_plus")) << cps;
        EXPECT_TRUE(players[cps].count(std::string(cps) + ".ra64.mamearcade")) << cps;
    }
    EXPECT_TRUE(players["model3"].count("model3.ra64.supermodel"));
    EXPECT_TRUE(players["ps2"].count("ps2.com.armsx2"));
    EXPECT_TRUE(players["namcops2"].count("namcops2.com.armsx2"));
    EXPECT_FALSE(players["3ds"].count("3ds.com.antutu.ABenchMark"));
}

namespace {

// MAME4droid's split_in_args (MAME4droid-current myosd_droid.cpp): its cli_params extra as MAME's
// arguments.
std::vector<std::string> mame4droidSplit(const std::string& command) {
    std::vector<std::string> qargs;
    const int len = (int)command.length();
    bool sqot = false;
    for (int i = 0; i < len; i++) {
        int start = i, arglen;
        if (command[i] == '\'') sqot = true;
        if (sqot) {
            i++;
            start++;
            while (i < len && command[i] != '\'') i++;
            if (i < len) sqot = false;
            arglen = i - start;
            i++;
        } else {
            while (i < len && command[i] != ' ') i++;
            arglen = i - start;
        }
        qargs.push_back(command.substr(start, arglen));
    }
    return qargs;
}

std::string normalizeWs(const std::string& in) {   // NanoMenuPS3EmuCatalog.cpp's
    std::string s;
    bool sp = false;
    for (char c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') sp = true;
        else { if (sp && !s.empty()) s += ' '; sp = false; s += c; }
    }
    return s;
}

struct MameLaunch {
    const char* driver;
    std::vector<std::string> options;   // between the ROM path and the game's media
    const char* media;
};

}  // namespace

// The systems only MAME4droid 2024 plays (bring your own; Thimsy does not ship it): each player boots
// its MAME driver (the intent's data, as ES-DE passes it) with the game on the right media switch, and
// a game's path stays one MAME argument whatever its name.
TEST(Catalog, Mame4droidSystems) {
    const std::string bios = "/storage/emulated/0/Android/data/com.seleuco.mame4d2024/files/roms";
    const std::map<std::string, MameLaunch> want = {
        {"adam.m4d2024.diskette", {"adam", {}, "-flop1"}},
        {"adam.m4d2024.tape", {"adam", {}, "-cass1"}},
        {"adam.m4d2024.cartridge", {"adam", {}, "-cart1"}},
        {"advision.m4d2024", {"advision", {}, "-cart"}},
        {"apf.m4d2024", {"apfm1000", {}, "-cart"}},
        {"apple2gs.m4d2024", {"apple2gs", {"-gameio", "joy"}, "-flop3"}},
        {"archimedes.m4d2024.a3000", {"aa3000", {}, "-flop1"}},
        {"archimedes.m4d2024.a4401", {"aa4401", {}, "-flop1"}},
        {"astrocade.m4d2024", {"astrocde", {}, "-cart"}},
        {"atom.m4d2024", {"atom", {}, "-quik"}},
        {"coco.m4d2024.coco3cartridge", {"coco3", {}, "-cart"}},
        {"coco.m4d2024.cococartridge", {"coco", {}, "-cart"}},
        {"coco.m4d2024.tape", {"coco", {"-autoboot_delay", "2", "-autoboot_command", "cloadm:exec\\n"}, "-cass"}},
        {"dragon32.m4d2024.dragon32cartridge", {"dragon32", {}, "-cart"}},
        {"dragon32.m4d2024.dragon64cartridge", {"dragon64", {}, "-cart"}},
        {"dragon32.m4d2024.tape", {"dragon32", {"-autoboot_delay", "4", "-autoboot_command", "cloadm\"\"\\n"}, "-cass"}},
        {"electron.m4d2024.tape", {"electron64", {"-autoboot_delay", "2", "-autoboot_command", "*T.\\nCH.\"\"\\n"}, "-cass1"}},
        {"electron.m4d2024.adfsdiskette",
         {"electron64", {"-exp", "plus3", "-exp:plus3:fdc:1", "35dd", "-autoboot_delay", "2", "-autoboot_command",
                         "*CAT\\n\\n\\n\\n\\n\\n*RUN !BOOT\\n"}, "-flop1"}},
        {"fm7.m4d2024.fm7diskette", {"fm7", {}, "-flop1"}},
        {"fm7.m4d2024.fm77avdiskette", {"fm77av", {}, "-flop1"}},
        {"fm7.m4d2024.tape", {"fm7", {"-autoboot_delay", "5", "-autoboot_command", "load\\n\\n\\nrun\\n"}, "-cass1"}},
        {"fmtowns.m4d2024.fmtownsiiux", {"fmtownsux", {}, "-cdrm"}},
        {"fmtowns.m4d2024.fmtownsmarty", {"fmtmarty", {}, "-cdrm"}},
        {"gamate.m4d2024", {"gamate", {}, "-cart"}},
        {"gamecom.m4d2024", {"gamecom", {}, "-cart1"}},
        {"gmaster.m4d2024", {"gmaster", {}, "-cart"}},
        {"microvision.m4d2024", {"microvsn", {}, "-cart"}},
        {"oric.m4d2024.oric1", {"oric1", {"-autoboot_delay", "4", "-autoboot_command", "cload\"\"\\n"}, "-cass"}},
        {"oric.m4d2024.oricatmos", {"orica", {"-autoboot_delay", "4", "-autoboot_command", "cload\"\"\\n"}, "-cass"}},
        {"pv1000.m4d2024", {"pv1000", {}, "-cart"}},
        {"samcoupe.m4d2024", {"samcoupe", {"-autoboot_delay", "3", "-autoboot_command", "boot\\n"}, "-flop1"}},
        {"studio2.m4d2024", {"studio2", {}, "-cart"}},
        {"supracan.m4d2024", {"supracan", {}, "-cart"}},
        {"ti99.m4d2024", {"ti99_4a", {"-ioport", "peb", "-ioport:peb:slot3", "speechadapter"}, "-cart"}},
        {"trs-80.m4d2024.program", {"trs80l2", {}, "-quik"}},
        {"trs-80.m4d2024.diskette", {"trs80l2", {}, "-flop1"}},
        {"vsmile.m4d2024", {"vsmile", {}, "-cart"}},
    };
    std::set<std::string> seen;
    for (const auto& p : catalog()) {
        for (const njson::Value* v : p.players) {
            const std::string uid = v->getString("uniqueId"), am = normalizeWs(v->getString("amStartArguments"));
            if (am.find("cli_params") == std::string::npos) continue;
            SCOPED_TRACE(uid);
            const auto it = want.find(uid);
            ASSERT_NE(it, want.end()) << "a MAME4droid player this test does not know";
            seen.insert(uid);
            const MameLaunch& w = it->second;
            for (const char* name : {"Game (USA).dsk", "it is $(echo x) `id`; -flop2 x.dsk", "-cart x.bin"}) {
                SCOPED_TRACE(name);
                const std::string dir = "/storage/1234-ABCD/ROMs/" + p.uid;
                nanolaunch::LaunchVars lv;
                lv.filePath = "/mnt/media_rw/1234-ABCD/ROMs/" + p.uid + "/" + name;
                lv.fileUri = nanolaunch::safContentUri(lv.filePath, "");
                std::vector<std::string> toks;
                ASSERT_TRUE(nanolaunch::buildLaunchIntent(am, "", lv, toks));
                const std::vector<std::string> head = {"-n", "com.seleuco.mame4d2024/com.seleuco.mame4droid.MAME4droid",
                                                       "-a", "android.intent.action.VIEW", "-d", w.driver, "-e",
                                                       "cli_params"};
                ASSERT_EQ(toks.size(), head.size() + 3);
                EXPECT_EQ(std::vector<std::string>(toks.begin(), toks.begin() + head.size()), head);
                EXPECT_EQ(toks[head.size() + 1], "--activity-clear-task");
                EXPECT_EQ(toks[head.size() + 2], "--activity-clear-top");
                std::vector<std::string> args = {"-rompath", dir + ";" + bios};
                args.insert(args.end(), w.options.begin(), w.options.end());
                args.push_back(w.media);
                args.push_back(dir + "/" + name);
                EXPECT_EQ(mame4droidSplit(toks[head.size()]), args);
            }
            // Refused rather than letting a name end the argument (and add MAME options).
            nanolaunch::LaunchVars lv;
            lv.filePath = "/sdcard/ROMs/" + p.uid + "/x' -autoboot_script 'evil.lua";
            std::vector<std::string> toks;
            EXPECT_FALSE(nanolaunch::buildLaunchIntent(am, "", lv, toks));
        }
    }
    for (const auto& w : want) EXPECT_TRUE(seen.count(w.first)) << w.first << " is not in the catalog";
}
