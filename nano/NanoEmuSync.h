// The emulator settings that live in an app's private folder (Android preferences, button maps,
// ScummVM's scummvm.ini, Vita3K's config.yml, RetroArch's controller profiles), kept in step with a
// copy on the microSD card (NanoEmuSync.cpp has why and how). No Android code: host tested.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace emusync {

// One thing kept in step: rel under the app's private folder (/data/data/<pkg>), or with media under
// its shared one (Android/data/<pkg>/files); a file or a folder (all of it but skip, top level).
struct Item {
    const char* emu;    // its folder under <card>/emulator_config
    const char* pkg;
    bool media;
    const char* rel;
    std::vector<std::string> skip;
    bool many = false;   // many files: not checked just before the emulator starts (that waits for it)
};
const std::vector<Item>& items();

std::string appPath(const Item& it);                              // where the app keeps it
std::string cardPath(const std::string& card, const Item& it);    // <card>/emulator_config/<emu>/app[/external]/<rel>

// A file the sync never copies: an editor's or SharedPreferences' leftovers, libraries' own preferences.
bool skipFile(const std::string& name);

// A file's state on one side: -1 when it is not there, else its modification time and size in one number.
// haveBase: both sides as the last sync left them (none: never synced on this card, when the card's
// copy wins, or with preferApp this device's).
enum class Act { None, ToApp, ToCard, DeleteApp, DeleteCard };
Act decide(bool haveBase, int64_t baseApp, int64_t baseCard, int64_t app, int64_t card, bool preferApp = false);

// What the last sync left, per card, emulator and file ("<card>\x1f<emu>\x1f<rel>"): the app side's
// state, the card side's, and the card copy's contents hash (0: not known), which tells a card file
// whose time alone moved (a FAT card's times are local time) from one that was edited.
struct State {
    int64_t app = -1, card = -1;
    uint64_t hash = 0;
    bool operator==(const State& o) const { return app == o.app && card == o.card && hash == o.hash; }
    bool operator!=(const State& o) const { return !(*this == o); }
};
using Base = std::map<std::string, State>;
std::string baseKey(const std::string& cardId, const std::string& emu, const std::string& rel);
std::string baseSave(const Base& b);
Base baseLoad(const std::string& text);
uint64_t hash(const std::string& data);   // never 0

// A card volume id as Android names it (XXXX-XXXX, or 32 hex digits).
bool isVolumeId(const std::string& s);

// text with the emulator_config folder of a card that is out (absent(id)) named on the card with id
// instead: as a path (/storage/<id>/..., /mnt/media_rw/<id>/...) or in a content:// folder grant
// (<id>%3Aemulator_config, also encoded twice). A copy of the card with another id, or settings from
// a card that was replaced, then name this card's folders.
std::string repoint(const std::string& text, const std::string& id, const std::function<bool(const std::string&)>& absent);

// The external storage folder grants text names (content://com.android.externalstorage.documents/
// tree/<volume>%3A<path>, or ScummVM's /saf/<volume>%3A<path>): {volume, path}, path decoded.
std::vector<std::pair<std::string, std::string>> treeUris(const std::string& text);

// The files of a folder an app filled from its own package in one go (RetroArch's controller
// profiles, hundreds unpacked within a minute): those of the minute (and the next) holding at least
// minCount of mtimes (seconds), by index. What the user added or changed is outside it.
std::vector<size_t> unpackedTogether(const std::vector<int64_t>& mtimes, size_t minCount = 50);

}  // namespace emusync
}  // namespace android
