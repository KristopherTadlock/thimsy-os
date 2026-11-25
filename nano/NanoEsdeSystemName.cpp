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

#include <ctype.h>
#include <string.h>

#include <utility>

namespace android {
namespace esdesys {

#include "NanoEsdeSystemNames.inc"
#include "NanoEsdeEmulators.inc"

bool esdeKnownSystem(const std::string& n) {
    for (const auto& kv : kEsdeSystemFullNames) if (n == kv.first) return true;
    return false;
}

std::string esdeSystemFullName(const std::string& esdeName) {
    if (esdeName.empty()) return std::string();
    const std::string name = esdeName == "astrocade" ? "astrocde" : esdeName;
    for (const auto& kv : kEsdeSystemFullNames)
        if (name == kv.first) return kv.second;
    return std::string();
}

static std::string lower(const std::string& s) {
    std::string low = s;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    return low;
}

// A system added from a folder is named after the folder, and handheld SD cards are often laid out
// MinUI style (PS, NGP, FC, MD): themes have no art under those names, so the system showed a blank
// logo/art and no game media.
std::string esdeCanonSystemName(const std::string& romDir, const std::string& shortname) {
    const std::string raw = romDir.empty() ? shortname : romDir;
    if (raw.empty() || esdeKnownSystem(raw)) return raw;
    const std::string low = lower(raw);
    if (esdeKnownSystem(low)) return low;
    static const std::pair<const char*, const char*> kAlias[] = {
        {"ps", "psx"}, {"ps1", "psx"}, {"psone", "psx"}, {"playstation", "psx"},
        {"fc", "nes"}, {"sfc", "snes"}, {"md", "megadrive"}, {"gen", "genesis"},
        {"sms", "mastersystem"}, {"ms", "mastersystem"}, {"gg", "gamegear"},
        {"pce", "pcengine"}, {"pcecd", "pcenginecd"}, {"gw", "gameandwatch"},
        {"ws", "wonderswan"}, {"wsc", "wonderswancolor"}, {"vb", "virtualboy"},
        {"a2600", "atari2600"}, {"a5200", "atari5200"}, {"a7800", "atari7800"},
        {"a800", "atari800"}, {"lynx", "atarilynx"}, {"poke", "pokemini"}, {"pm", "pokemini"},
        {"mdcd", "segacd"}, {"scd", "segacd"}, {"32x", "sega32x"}, {"sat", "saturn"},
        {"dc", "dreamcast"}, {"neocd", "neogeocd"}, {"java", "j2me"},
        {"sg1000", "sg-1000"}, {"ngpc", "ngpc"}, {"ngp", "ngp"},
    };
    for (const auto& a : kAlias)
        if (low == a.first && esdeKnownSystem(a.second)) return a.second;
    return raw;
}

namespace {

std::string mapCatalogId(const std::string& canon,
                         const std::pair<const char*, const char*>* table, size_t n) {
    const std::string low = lower(canon);
    for (size_t i = 0; i < n; i++)
        if (low == table[i].first && esdeKnownSystem(table[i].second)) return table[i].second;
    return canon;
}

}  // namespace

std::string esdePlatformSystemName(const std::string& romDir, const std::string& shortname) {
    const std::string canon = esdeCanonSystemName(romDir, shortname);
    if (canon.empty() || esdeKnownSystem(canon)) return canon;
    // Catalog platform ids (ps3xmb/daijishou uniqueId) whose ES-DE system has another name.
    static const std::pair<const char*, const char*> kCatalog[] = {
        {"coleco", "colecovision"}, {"3ds", "n3ds"}, {"tgcd", "tg-cd"}, {"master", "mastersystem"},
        {"vita", "psvita"}, {"cpc", "amstradcpc"}, {"appleii", "apple2"}, {"jaguar", "atarijaguar"},
        {"jaguarcd", "atarijaguarcd"}, {"cdi", "cdimono1"}, {"rpgmaker", "easyrpg"},
        {"astrocade", "astrocde"},
    };
    return mapCatalogId(canon, kCatalog, sizeof(kCatalog) / sizeof(kCatalog[0]));
}

std::string esdeThemeSystemName(const std::string& romDir, const std::string& shortname) {
    const std::string sys = esdePlatformSystemName(romDir, shortname);
    // Themes name the Bally Astrocade's art astrocade, not after ES-DE's system.
    if (sys == "astrocde") return "astrocade";
    if (sys.empty() || esdeKnownSystem(sys)) return sys;
    // The MSU, Minis, WiiWare and DSi platforms are games for the console they map to, and the
    // Videopac+ draws as the Videopac it extends.
    static const std::pair<const char*, const char*> kGamesFor[] = {
        {"genesismsu", "genesis"}, {"snesmsu1", "snes"}, {"pspminis", "psp"}, {"wiiware", "wii"},
        {"ndsi", "nds"}, {"g7400", "videopac"},
        // Boards and models ES-DE has no system for draw as their family's.
        {"c128", "c64"}, {"segacd32x", "sega32x"},
    };
    return mapCatalogId(sys, kGamesFor, sizeof(kGamesFor) / sizeof(kGamesFor[0]));
}

std::vector<EsdeCommand> esdeSystemCommands(const std::string& esdeName) {
    std::vector<EsdeCommand> out;
    for (const auto& r : kEsdeCommands) {
        if (esdeName != r.system) continue;
        EsdeCommand c;
        c.label = r.label;
        if (strncmp(r.command, "@ra:", 4) == 0) {
            c.command = kEsdeRetroArchCommand;
            c.command.replace(c.command.find("{core}"), 6, r.command + 4);
        } else {
            c.command = r.command;
        }
        for (const char* k = r.keys; *k;) {
            const char* sp = strchr(k, ' ');
            const size_t n = sp ? (size_t)(sp - k) : strlen(k);
            if (n > 0) c.keys.emplace_back(k, n);
            k += n + (sp ? 1 : 0);
        }
        out.push_back(std::move(c));
    }
    return out;
}

bool esdeSystemDef(const std::string& esdeName, std::string* extension, std::string* platform, std::string* theme) {
    for (const auto& r : kEsdeSystemDefs) {
        if (esdeName != r.system) continue;
        if (extension) *extension = r.extension;
        if (platform) *platform = r.platform;
        if (theme) *theme = r.theme;
        return true;
    }
    return false;
}

std::string esdeEmulatorKey(bool libretro, const std::string& coreSo, const std::string& package) {
    if (libretro) {
        static const std::string kSuffix = "_libretro_android.so";
        std::string core = coreSo.substr(coreSo.find_last_of('/') + 1);
        if (core.size() > kSuffix.size() && core.compare(core.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
            return "core:" + core.substr(0, core.size() - kSuffix.size());
        return std::string();
    }
    return package.empty() ? std::string() : "pkg:" + package;
}

std::string esdeLabelForKey(const std::string& esdeName, const std::string& key) {
    if (key.empty()) return std::string();
    std::string best;
    size_t bestAt = (size_t)-1;
    bool tie = false;
    for (const auto& c : esdeSystemCommands(esdeName)) {
        for (size_t i = 0; i < c.keys.size() && i <= bestAt; i++) {
            if (c.keys[i] != key || c.label.empty()) continue;
            if (i == bestAt) tie = true;   // one app, several commands (MAME4droid's [Tape] and [Diskette])
            else { best = c.label; bestAt = i; tie = false; }
            break;
        }
    }
    return tie ? std::string() : best;
}

std::string esdeDefaultLabel(const std::string& esdeName) {
    const std::vector<EsdeCommand> cmds = esdeSystemCommands(esdeName);
    return cmds.empty() ? std::string() : cmds[0].label;
}

std::vector<std::string> esdeKeysForLabel(const std::string& esdeName, const std::string& label) {
    if (label.empty()) return {};
    for (auto& c : esdeSystemCommands(esdeName))
        if (c.label == label) return std::move(c.keys);
    return {};
}

}  // namespace esdesys
}  // namespace android
