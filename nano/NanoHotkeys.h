// Universal hotkeys (HOTKEYS.md in the project): the emulator profiles PhoneWindowManager's
// ThimsyHotkeyController reads. Nano lists them in Settings and applies their "provision" rules, which
// bind the keys a profile sends in the emulator's own settings files.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NanoJson.h"

namespace android {
namespace nanohotkeys {

struct Profile {
    std::string id;
    std::string name;
    std::vector<std::string> packages;
    njson::Value json;
};

// The built-in profiles, with a user file of the same id replacing a built-in one.
std::vector<Profile> loadProfiles();

// Applies every profile's provision rules, except for the profiles of skipPkg (the app running now,
// which may write its settings back when it exits). Only edits files that already exist.
void provision(const std::string& skipPkg);

// The files a provision rule naming `path` edits: path, and for RetroArch's default config also
// raConfig, the one its launches pass (on the card when its folder is there), when that is another.
std::vector<std::string> ruleFiles(const std::string& path, const std::string& raConfig);

// Where Move Emulator Files to microSD put the folders the rules name by their default place:
// PPSSPP's memory stick and Flycast's home folder. "" = not known (left at the default).
struct MovedFolders {
    std::string ppssppMemstick;
    std::string flycastHome;
};
MovedFolders movedFolders();   // read from the emulators' settings
// A rule's file in the folder its emulator now uses.
std::string relocatedFile(const std::string& path, const MovedFolders& moved);
// path with a link on its way (Dolphin's folders after the move) replaced by where it points, when
// `allowed` accepts that place; never through any other link. "" when the path holds a link that is
// not allowed (or a chain longer than a few links).
std::string throughMoveLinks(const std::string& path, const std::function<bool(const std::string&)>& allowed);
// A folder Move Emulator Files made: <volume>/emulator_config/<emulator>/... on a card or drive.
bool isCardEmulatorFolder(const std::string& path);

// One provision rule on the settings file at `path`.
void provisionFile(const std::string& path, const njson::Value& rule);

}  // namespace nanohotkeys
}  // namespace android
