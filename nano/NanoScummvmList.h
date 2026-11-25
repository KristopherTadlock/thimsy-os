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

// ScummVM's games, as ScummVM lists them. A ScummVM game is a folder of data files (disc images,
// resources, sub-folders) that a file scan cannot tell from games, and the ScummVM app starts only a
// game in its own game list (scummvm.ini, one [target] section per game), never a folder or a file.
// So while a system's player is the ScummVM app, its games are that list: Nano keeps one small file
// per game, "<title>.scummvm" holding the target (the {file.content} launch) and the game ID (the
// scrape), in a folder of its own that the system scans in place of its ROM folders. Games are added
// and removed in ScummVM. No Android code, so host tests and the scrape worker can use it.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace android {
namespace nanoscummvm {

inline constexpr const char* kMirrorRoot = "/data/system/nano_scummvm";

// The ScummVM app, or its debug build.
bool isScummvmPkg(const std::string& pkg);
// The ScummVM system (ROM folder "scummvm") played by the ScummVM app: its games are the app's
// game list. Other systems the app plays (AGS's .scummvm files) are scanned as usual.
bool listsGames(const std::string& romDir, const std::string& pkg);
// /data/data/<pkg>/files/scummvm.ini
std::string iniPath(const std::string& pkg);
// kMirrorRoot/<pkg>/scummvm: named scummvm so that ES-DE store mode files the games under scummvm.
std::string mirrorDir(const std::string& pkg);
bool isMirrorPath(const std::string& path);

struct Game {
    std::string target;        // the section name, as the launch passes it
    std::string gameid;
    std::string description;
};

// Every game section whose name ScummVM's command line takes as a target ([A-Za-z0-9_-], 1 to 64,
// no leading '-'), in the file's order: one with a gameid, or with a path and no gameid (an old
// entry; ScummVM's launcher then takes the target as the gameid), never ScummVM's own sections. A
// repeated section adds to the first.
std::vector<Game> parseGames(const std::string& iniText);

// Whether the text is a whole scummvm.ini: ScummVM always writes its [scummvm] section, so a file
// without one is empty or caught while ScummVM rewrites it, and the list is left as it is.
bool iniComplete(const std::string& iniText);

// The file name of each game (parallel to games): its description without the trailing
// "(CD/DOS/English)", or the whole description where two games would share a name, or then
// " [target]" added. Safe on a FAT card (ES-DE media are named after it), never hidden, unique
// ignoring case. current (target -> file name now) keeps a game's file name while it is still one
// of these for the game's description, so adding a variant doesn't rename the first (Recently
// Played, the scrape and ES-DE media key on the path); a changed description renames it.
std::vector<std::string> fileNames(const std::vector<Game>& games,
                                   const std::map<std::string, std::string>& current = {});
// "<target>\n<gameid>\n"
std::string fileText(const Game& g);

// The game ID on the second line of a file in the mirror folder (under root, kMirrorRoot but in
// tests); "" for any other file.
std::string mirrorGameId(const std::string& path, const std::string& root = kMirrorRoot);

// Make dir hold exactly the games' files: written when missing or different (so an unchanged game
// keeps its file and its mtime), names kept as fileNames allows, other .scummvm files removed unless
// removeStale is false (a list that may be only part of ScummVM's). False when dir cannot be made.
bool syncDir(const std::string& dir, const std::vector<Game>& games, bool removeStale = true);

}  // namespace nanoscummvm
}  // namespace android
