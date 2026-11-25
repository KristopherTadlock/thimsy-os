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

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <set>

#include "NanoEsdeSystemName.h"

namespace android {
namespace nanosys {

std::vector<std::string> romFolderAliases(const std::string& romDir) {
    static const char* const kGroups[] = {
        "nes,famicom",
        // Add-ons and enhanced games with catalog platforms of their own (each its own group, never
        // merged into the console's): Satellaview, Sufami Turbo, Super Game Boy, 64DD, Sega CD 32X.
        "snes,sfc,snesna,superfamicom", "satellaview,bsx", "sufami,sufamiturbo", "sgb,supergameboy",
        "gb,gameboy",
        "gbc,gameboycolor",
        "gba,gameboyadvance",
        "n64,nintendo64", "n64dd,64dd",
        "nds,ds,nintendods",
        "genesis,megadrive,md,megadrivejp",
        "mastersystem,sms,master",
        "gamegear,gg",
        "psx,ps1,playstation,psone",
        "psp,playstationportable",
        "dreamcast,dc",
        "ngp",
        "ngpc",
        "pico8,pico-8",
        "ps2,playstation2",
        "gc,gamecube,ngc",
        "wii", "wiiu",
        "3ds,n3ds,nintendo3ds",
        "switch", "ps3,playstation3", "psvita,vita",
        "saturn,segasaturn,saturnjp",
        "segacd,megacd,megacdjp",
        "sega32x,sega32xjp,sega32xna,32x", "segacd32x,sega32xcd",
        "sg-1000,sg1000",
        // SuperGrafx and ZX81 games need cores of their own (mednafen_supergrafx, 81).
        "pcengine,tg16,pce,turbografx16", "supergrafx",
        "pcenginecd,tg-cd,pcecd,tgcd",
        // FBNeo runs Neo Geo carts from neogeo; Neo Geo CD discs need a CD core, so they stay apart.
        // FBNeo takes a disc for a Neo Geo CD one only from a folder named neocd.
        "neogeo", "neogeocd,neogeocdjp,neocd",
        "wonderswan,ws", "wonderswancolor,wsc",
        "atari2600,a2600", "atari5200", "atari7800",
        "atarilynx,lynx", "atarijaguar,jaguar", "atarijaguarcd,jaguarcd", "atarist,ast",
        "msx,msx1,msx2", "msxturbor,turbor", "spectravideo,svi", "colecovision,coleco", "intellivision,intv",
        "vectrex", "virtualboy,vb",
        // Keep distinct arcade platforms in their OWN groups so a per-board system (e.g. CPS1) does
        // not scan every other arcade folder and auto-add does not collapse them into one system.
        // "arcade" stays a synonym of MAME (the generic catch-all folder name); FBNeo keeps its FBA
        // alias; each CP System board is on its own.
        "arcade,mame,mame2003,mame2010",
        "fbneo,fba",
        // Vertical (TATE) arcade sets kept apart from the rest: VERTICAL / VARCADE (Anbernic
        // sets), TATE (Brick set), TateGame (MagicX sets). Their own catalog platform "Arcade
        // (Vertical)" (varcade) so they auto-add and name as such instead of folding into MAME.
        "varcade,vertical,tate,tategame,verticalarcade",
        "cps1", "cps2", "cps3", "stv", "naomi2", "model2", "model3", "triforce",
        "namcops2,namco2x6,namco246", "laserdisc,daphne",
        "c64,commodore64", "c128,commodore128", "amiga", "amigacd32,cd32", "cdtv", "amstradcpc,cpc", "gx4000",
        "zxspectrum,spectrum", "zx81", "atari800,atarixe,atari8bit,a800", "macintosh,mac", "appleii,apple2",
        "moto,to8,thomson", "scv,supercassettevision", "crvision,creativision", "cdi,cdimono1",
        "windows", "windows3x,win3x", "windows9x,win9x", "flash,swf", "openbor", "symbian", "vircon32",
        "zmachine,infocom", "lutro", "chailove", "vpinball,visualpinball", "ags", "rpgmaker,easyrpg",
        "scummvm", "ports", "dos,pc", "fds", "naomi,naomigd", "atomiswave", "pokemini",
        "channelf", "odyssey2,videopac", "x68000,x68k",
        "supervision,watara", "gameandwatch,gw", "3do",
        // Systems MAME4droid plays (ES-DE's folder first, then the MAME driver's name where it differs).
        "adam,colecoadam", "apple2gs,appleiigs", "archimedes,acornarchimedes", "astrocde,astrocade",
        "atom,acornatom", "coco,tandycoco", "dragon32,dragon,dragon64,tanodragon", "electron,acornelectron",
        "fm7,fm77av", "fmtowns,fmtownsmarty,fmtmarty", "gamate", "gamecom", "gmaster,gamemaster", "oric,orica",
        "pv1000", "samcoupe", "supracan,superacan", "ti99,ti994a,ti99_4a", "trs-80,trs80", "vsmile",
        "studio2", "advision,adventurevision", "apf,apfm1000", "microvision,microvsn",
    };
    auto low = [](const std::string& in) {
        std::string o; for (char c : in) o += (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); return o;
    };
    std::string lower = low(romDir);
    std::vector<std::string> out; out.push_back(romDir);
    for (const char* g : kGroups) {
        std::vector<std::string> toks; std::string t;
        for (const char* p = g; ; p++) {
            if (*p == ',' || *p == '\0') { if (!t.empty()) toks.push_back(t); t.clear(); if (!*p) break; }
            else t += (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        }
        bool match = false; for (auto& tk : toks) if (tk == lower) { match = true; break; }
        if (!match) continue;
        for (auto& tk : toks) {
            if (tk == lower) continue;
            bool dup = false; for (auto& o : out) if (low(o) == tk) { dup = true; break; }
            if (!dup) out.push_back(tk);
        }
    }
    return out;
}

int platformForFolder(const std::string& folder, const std::vector<std::string>& platformIds) {
    std::vector<std::string> names;
    for (const auto& a : romFolderAliases(folder)) {
        std::string l = a;
        for (auto& ch : l) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        names.push_back(l);
    }
    for (size_t i = 0; i < platformIds.size(); i++)
        if (platformIds[i] == names[0]) return (int)i;
    for (size_t i = 0; i < platformIds.size(); i++)
        for (const auto& n : names)
            if (platformIds[i] == n) return (int)i;
    return -1;
}

const std::vector<std::string>& splitOffFolders() {
    static const std::vector<std::string> kFolders = {
        "satellaview", "bsx", "sufami", "sufamiturbo", "sgb", "supergameboy", "n64dd", "64dd",
        "neogeocd", "neogeocdjp", "neocd", "supergrafx", "zx81", "segacd32x", "sega32xcd",
        "msxturbor", "turbor", "spectravideo", "svi", "c128", "commodore128", "amigacd32", "cd32",
        "atarijaguarcd", "jaguarcd", "cdtv", "gx4000",
    };
    return kFolders;
}

bool folderHasRom(const std::string& path, const std::string& acceptExts) {
    auto low = [](std::string v) {
        for (auto& c : v) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        return v;
    };
    std::vector<std::string> exts;
    { std::string t;
      for (char c : acceptExts) {
          if (c == ',') { if (!t.empty()) exts.push_back(low(t)); t.clear(); }
          else t += c;
      }
      if (!t.empty()) exts.push_back(low(t)); }
    exts.push_back(".zip"); exts.push_back(".7z"); exts.push_back(".chd");
    auto fileMatches = [&](const char* nm) {
        const char* dot = strrchr(nm, '.');
        if (!dot) return false;
        const std::string e = low(dot);
        for (const auto& x : exts) if (x == e) return true;
        return false;
    };
    std::vector<std::string> subdirs;
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    int examined = 0;
    for (struct dirent* de; (de = readdir(d)) != nullptr && examined < 4000;) {
        if (de->d_name[0] == '.') continue;
        examined++;
        const std::string full = path + "/" + de->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISREG(st.st_mode)) { if (fileMatches(de->d_name)) { closedir(d); return true; } }
        else if (S_ISDIR(st.st_mode) && subdirs.size() < 12) subdirs.push_back(full);
    }
    closedir(d);
    // One level deep (per-game-folder layouts, e.g. some disc systems), bounded.
    for (const auto& sd : subdirs) {
        DIR* sdd = opendir(sd.c_str());
        if (!sdd) continue;
        int n2 = 0;
        for (struct dirent* se; (se = readdir(sdd)) != nullptr && n2 < 400;) {
            if (se->d_name[0] == '.') continue;
            n2++;
            if (fileMatches(se->d_name)) { closedir(sdd); return true; }
        }
        closedir(sdd);
    }
    return false;
}

bool drivesServed(const std::string& mediaRw, const std::string& mounts) {
    std::set<std::string> points;   // "<source> <mount point> <type> ...": volume names have no spaces
    for (size_t pos = 0; pos < mounts.size();) {
        size_t eol = mounts.find('\n', pos);
        if (eol == std::string::npos) eol = mounts.size();
        const size_t a = mounts.find(' ', pos);
        if (a != std::string::npos && a < eol) {
            size_t b = mounts.find(' ', a + 1);
            if (b == std::string::npos || b > eol) b = eol;
            points.insert(mounts.substr(a + 1, b - a - 1));
        }
        pos = eol + 1;
    }
    DIR* d = opendir(mediaRw.c_str());
    if (!d) return true;
    bool served = true;
    for (struct dirent* de; served && (de = readdir(d)) != nullptr;) {
        if (de->d_name[0] == '.') continue;
        struct stat st;
        if (stat((mediaRw + "/" + de->d_name).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        served = points.count(std::string("/storage/") + de->d_name) > 0;
    }
    closedir(d);
    return served;
}

std::vector<FoundFolder> findSplitOffFolders(const std::vector<std::string>& roots,
                                             const std::vector<std::string>& platformIds,
                                             const std::function<std::string(int)>& extsFor) {
    std::vector<FoundFolder> out;
    std::set<std::string> seen;
    for (const auto& root : roots) {
        DIR* d = opendir(root.c_str());
        if (!d) continue;
        int examined = 0;
        for (struct dirent* de; (de = readdir(d)) != nullptr && examined < 4000;) {
            if (de->d_name[0] == '.') continue;
            examined++;
            const std::string name = de->d_name;
            bool split = false;
            for (const auto& f : splitOffFolders())
                if (strcasecmp(f.c_str(), name.c_str()) == 0) { split = true; break; }
            if (!split) continue;
            const std::string full = root + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || !seen.insert(full).second) continue;
            const int pi = platformForFolder(name, platformIds);
            if (pi < 0 || !folderHasRom(full, extsFor(pi))) continue;
            out.push_back({name, full, pi});
        }
        closedir(d);
    }
    return out;
}

namespace {

std::string lowerAscii(const std::string& in) {
    std::string o = in;
    for (auto& ch : o) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    return o;
}

std::string trimSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string baseName(const std::string& path) {
    const std::string p = trimSlashes(path);
    const size_t sl = p.find_last_of('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

// Storage folders compare in any letter case.
bool sameOrInside(const std::string& a, const std::string& b) {
    const std::string x = lowerAscii(trimSlashes(a)), y = lowerAscii(trimSlashes(b));
    if (x.empty() || y.empty() || x[0] != '/' || y[0] != '/') return false;
    auto under = [](const std::string& in, const std::string& dir) {
        return in == dir || (in.size() > dir.size() && in.compare(0, dir.size(), dir) == 0
                             && (dir == "/" || in[dir.size()] == '/'));
    };
    return under(x, y) || under(y, x);
}

// The folder names a system's games are under: its id's and folder's groups, and its chosen folders'.
std::set<std::string> folderKeys(const SysCore& s) {
    std::set<std::string> keys;
    auto add = [&keys](const std::string& name) {
        if (name.empty()) return;
        for (const auto& a : romFolderAliases(name)) keys.insert(lowerAscii(a));
    };
    add(s.id);
    add(s.romDir);
    for (const auto& f : s.folders) add(baseName(f));
    return keys;
}

}  // namespace

bool sameSystem(const SysCore& a, const SysCore& b) {
    if (!a.id.empty() && a.id == b.id) return true;
    const auto ka = folderKeys(a), kb = folderKeys(b);
    for (const auto& k : ka)
        if (kb.count(k)) return true;
    const std::string ea = lowerAscii(esdesys::esdePlatformSystemName(a.romDir, a.shortname));
    if (!ea.empty() && ea == lowerAscii(esdesys::esdePlatformSystemName(b.romDir, b.shortname))) return true;
    for (const auto& fa : a.folders)
        for (const auto& fb : b.folders)
            if (sameOrInside(fa, fb)) return true;
    return false;
}

std::vector<SplitAdd> splitFolderAdds(const std::vector<SysCore>& systems, const std::vector<FoundFolder>& found,
                                      const std::vector<std::string>& platformIds) {
    std::vector<SplitAdd> out;
    for (const auto& f : found) {
        if (f.platform < 0 || f.platform >= (int)platformIds.size()) continue;
        SysCore c;
        c.id = c.romDir = c.shortname = platformIds[f.platform];
        c.folders = {f.path};
        bool scanned = false;
        for (const auto& s : systems)
            if (sameSystem(s, c)) { scanned = true; break; }
        if (scanned) continue;
        SplitAdd* add = nullptr;
        for (auto& a : out) if (a.platform == f.platform) { add = &a; break; }
        if (!add) { out.push_back({f.platform, {}}); add = &out.back(); }
        if (std::find(add->folders.begin(), add->folders.end(), f.path) == add->folders.end())
            add->folders.push_back(f.path);
    }
    return out;
}

std::vector<CoreRepair> staleCoreRepairs(const std::vector<SysCore>& systems, const std::vector<SysCore>& bundled,
                                         const std::vector<std::pair<std::string, std::string>>& builtinCores,
                                         const std::function<bool(const std::string&)>& coreInstalled) {
    std::vector<CoreRepair> out;
    for (size_t i = 0; i < systems.size(); i++) {
        const SysCore& s = systems[i];
        if (!s.builtin || !s.libretro || s.coreSo.empty()) continue;
        bool nanoDefault = false;
        for (const auto& d : builtinCores)
            if (d.first == s.id && d.second == s.coreSo) { nanoDefault = true; break; }
        if (!nanoDefault) continue;   // chosen by the user or the build
        const SysCore* b = nullptr;
        for (const auto& c : bundled)
            if (c.builtin && c.id == s.id) { b = &c; break; }
        if (!b || !b->libretro || b->coreSo.empty() || b->coreSo == s.coreSo) continue;
        if (coreInstalled(s.coreSo) || !coreInstalled(b->coreSo)) continue;
        out.push_back({i, b->coreSo});
    }
    return out;
}

std::vector<size_t> missingBundledSystems(const std::vector<SysCore>& systems, const std::vector<SysCore>& bundled) {
    std::vector<size_t> out;
    for (size_t i = 0; i < bundled.size(); i++) {
        const SysCore& b = bundled[i];
        if (!b.builtin || b.id.empty()) continue;
        bool there = false;
        for (const auto& s : systems)
            if (sameSystem(s, b)) { there = true; break; }
        if (!there) out.push_back(i);
    }
    return out;
}

std::string playerLabel(const std::string& name) {
    static const char kRa[] = "RetroArch 64 - ";   // also spelt "Retroarch 64 - " in the catalog
    if (strncasecmp(name.c_str(), kRa, sizeof(kRa) - 1) == 0) return name.substr(sizeof(kRa) - 1) + " (RetroArch)";
    return name;
}

std::vector<size_t> playerChoices(const std::vector<Player>& players, const std::function<bool(const Player&)>& runs,
                                  bool* noneReady) {
    std::vector<size_t> ready, all;
    for (size_t i = 0; i < players.size(); i++) {
        all.push_back(i);
        if (runs(players[i])) ready.push_back(i);
    }
    *noneReady = ready.empty();
    std::vector<size_t> order = *noneReady ? all : ready;
    std::stable_sort(order.begin(), order.end(), [&players](size_t a, size_t b) {
        // By the launch, not the name: the catalog also spells it "Retroarch 64 - ...".
        if (players[a].libretro != players[b].libretro) return players[a].libretro;
        return strcasecmp(playerLabel(players[a].name).c_str(), playerLabel(players[b].name).c_str()) < 0;
    });
    return order;
}

int playerRepair(const SysCore& system, const std::vector<Player>& players,
                 const std::function<bool(const std::string&)>& coreInstalled,
                 const std::function<bool(const std::string&)>& packageInstalled) {
    if (system.builtin) return -1;
    if (system.libretro ? system.coreSo.empty() : system.package.empty()) return -1;   // not a catalog player
    bool catalogs = false;
    for (const auto& p : players)
        if (p.libretro == system.libretro && (p.libretro ? p.coreSo == system.coreSo : p.package == system.package)) {
            catalogs = true;
            break;
        }
    if (!catalogs) return -1;
    auto runs = [&](const Player& p) {
        if (p.libretro) return !p.coreSo.empty() && coreInstalled(p.coreSo);
        return !p.package.empty() && packageInstalled(p.package);
    };
    if (system.libretro ? coreInstalled(system.coreSo) : packageInstalled(system.package)) return -1;
    bool noneReady = true;
    const std::vector<size_t> order = playerChoices(players, runs, &noneReady);
    return noneReady || order.empty() ? -1 : (int)order[0];
}

}  // namespace nanosys
}  // namespace android
