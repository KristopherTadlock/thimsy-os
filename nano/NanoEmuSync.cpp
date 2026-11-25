// GammaOS Nano: emulator settings in app-private folders, kept in step with the microSD card.
//
// Move Emulator Files to microSD points each emulator at its folder on the card where it has a
// setting for one (NanoSdMove.cpp). Some of what makes an emulator yours has no such setting: its
// Android preferences, the button maps several keep beside them, ScummVM's scummvm.ini, Vita3K's
// config.yml, RetroArch's controller profiles. Those stay in the app's folder, and the card keeps a
// copy of each in <card>/emulator_config/<emu>/app (app/external for a file in Android/data).
//
// The two are kept in step file by file, three ways: what changed since the last sync goes to the
// other side (a file deleted on one side is deleted on the other); changed on both, or never synced
// on this card (a reinstall, another card), the card's copy wins. So the card is the master: a file
// edited on it is what the emulator gets, and a reinstalled Thimsy gets its settings back from it.
// What looks like a mass deletion (an emulator installed again, a card folder deleted or unreadable)
// is taken as never synced, so what is left is copied back rather than deleted; an app copy another
// card's sync wrote, unchanged since, gives way to this card's.
// Settings that name the emulator_config folder of a card that is out (a copy of the card with
// another id, the card this one replaced) are pointed at this card when they come in.
// Nano syncs when a card is adopted, every few seconds at the home screen, and for an emulator just
// before it starts; never into an emulator that is running.

#define LOG_TAG "GammaOSNano"

#include "NanoEmuSync.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace android {
namespace emusync {

const std::vector<Item>& items() {
    static const std::vector<Item> kItems = {
        {"retroarch", "com.retroarch.aarch64", false, "autoconfig", {}, true},   // controller profiles
        {"ppsspp", "org.ppsspp.ppsspp", false, "shared_prefs", {}},
        {"m64plus", "org.mupen64plusae.v3.fzurita", false, "shared_prefs", {}},
        {"m64plus", "org.mupen64plusae.v3.fzurita", false, "files/Profiles", {}},   // controller and emulation profiles
        {"dolphin", "org.dolphinemu.dolphinemu", false, "shared_prefs", {}},
        {"armsx2", "com.armsx2", false, "shared_prefs", {}},
        {"scummvm", "org.scummvm.scummvm", false, "shared_prefs", {}},
        {"scummvm", "org.scummvm.scummvm", false, "files/scummvm.ini", {}},   // settings, games, key maps
        {"vita3k", "org.vita3k.emulator", false, "shared_prefs", {}},
        {"vita3k", "org.vita3k.emulator", true, "config.yml", {}},
        {"azahar", "org.azahar_emu.azahar", false, "shared_prefs", {}},   // its Android settings and button maps
        {"watermelonds", "me.magnum.melondualds", false, "shared_prefs", {}},
        {"watermelonds", "me.magnum.melondualds", false, "files/controller_config.json", {}},
        {"seedlessds", "com.seedlessds.app", false, "shared_prefs", {}},   // settings, keymap.xml
        // Its saves and states move with the move when kept here (emulator_config/seedlessds).
        {"seedlessds", "com.seedlessds.app", false, "files/SeedlessDS/user", {"backup", "savestates"}},
        {"hakux", "com.rfandango.haku_x", false, "shared_prefs", {}},
        {"hakux", "com.rfandango.haku_x", false, "files/controllers.json", {}},
        {"drastic", "com.dsemu.drastic", false, "shared_prefs", {}},
    };
    return kItems;
}

std::string appPath(const Item& it) {
    return it.media ? std::string("/data/media/0/Android/data/") + it.pkg + "/files/" + it.rel
                    : std::string("/data/data/") + it.pkg + "/" + it.rel;
}

std::string cardPath(const std::string& card, const Item& it) {
    return card + "/emulator_config/" + it.emu + (it.media ? "/app/external/" : "/app/") + it.rel;
}

bool skipFile(const std::string& n) {
    auto ends = [&](const char* s) {
        const size_t l = strlen(s);
        return n.size() >= l && n.compare(n.size() - l, l, s) == 0;
    };
    auto starts = [&](const char* s) { return n.compare(0, strlen(s), s) == 0; };
    // Libraries' own preferences (ids, crash and WebView state): the device's, not the user's.
    if (starts("com.google.") || starts("com.facebook.") || starts("com.crashlytics") || starts("FirebaseHeartBeat")
        || starts("WebViewChromiumPrefs"))
        return true;
    return n.empty() || n[0] == '.' || ends(".bak") || ends(".nanotmp") || ends(".tmp") || ends("~");
}

Act decide(bool haveBase, int64_t baseApp, int64_t baseCard, int64_t app, int64_t card, bool preferApp) {
    if (!haveBase) {   // never synced on this card: the card's copy wins (or this device's), else the one there is
        if (preferApp && app >= 0) return app == card ? Act::None : Act::ToCard;
        if (card >= 0) return app == card ? Act::None : Act::ToApp;
        return app >= 0 ? Act::ToCard : Act::None;
    }
    const bool appChanged = app != baseApp, cardChanged = card != baseCard;
    if (cardChanged) {
        if (card >= 0) return Act::ToApp;   // also when both changed: the card wins
        return app >= 0 ? Act::DeleteApp : Act::None;
    }
    if (appChanged) return app >= 0 ? Act::ToCard : (card >= 0 ? Act::DeleteCard : Act::None);
    return Act::None;
}

std::string baseKey(const std::string& cardId, const std::string& emu, const std::string& rel) {
    return cardId + "\x1f" + emu + "\x1f" + rel;
}

// One line per file: key, then the app state, the card state and the card copy's hash, tab separated.
std::string baseSave(const Base& b) {
    std::string out;
    for (const auto& kv : b)
        out += kv.first + "\t" + std::to_string(kv.second.app) + "\t" + std::to_string(kv.second.card) + "\t" +
               std::to_string(kv.second.hash) + "\n";
    return out;
}

Base baseLoad(const std::string& text) {
    Base b;
    for (size_t p = 0; p < text.size();) {
        size_t nl = text.find('\n', p);
        if (nl == std::string::npos) nl = text.size();
        const std::string l = text.substr(p, nl - p);
        p = nl + 1;
        const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1);
        const size_t t3 = t2 == std::string::npos ? t2 : l.find('\t', t2 + 1);
        if (t3 == std::string::npos || t1 == 0) continue;
        b[l.substr(0, t1)] = {atoll(l.c_str() + t1 + 1), atoll(l.c_str() + t2 + 1), strtoull(l.c_str() + t3 + 1, nullptr, 10)};
    }
    return b;
}

uint64_t hash(const std::string& data) {   // FNV-1a
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) { h ^= c; h *= 1099511628211ULL; }
    return h ? h : 1;
}

static bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isVolumeId(const std::string& s) {
    if (s.size() == 9) {
        for (size_t i = 0; i < 9; i++)
            if (i == 4 ? s[i] != '-' : !isHex(s[i])) return false;
        return true;
    }
    if (s.size() != 32) return false;
    for (char c : s) if (!isHex(c)) return false;
    return true;
}

std::string repoint(const std::string& text, const std::string& id, const std::function<bool(const std::string&)>& absent) {
    static const char* const kAfter[] = {"/emulator_config", "%3Aemulator_config", "%253Aemulator_config", ":emulator_config"};
    std::string out;
    size_t from = 0;
    for (size_t at = 1; at < text.size(); at++) {   // at: where a volume id may start, after / or %2F
        const bool slash = text[at - 1] == '/' ||
                           (at >= 3 && text[at - 3] == '%' && text[at - 2] == '2' && (text[at - 1] == 'F' || text[at - 1] == 'f'));
        if (!slash || !isHex(text[at])) continue;
        size_t end = at;
        while (end < text.size() && (isHex(text[end]) || text[end] == '-') && end - at < 32) end++;
        const std::string vol = text.substr(at, end - at);
        if (!isVolumeId(vol) || vol == id) continue;
        bool follows = false;
        for (const char* a : kAfter) follows = follows || text.compare(end, strlen(a), a) == 0;
        if (!follows || !absent(vol)) continue;
        out.append(text, from, at - from);
        out += id;
        from = end;
        at = end - 1;
    }
    return from == 0 ? text : out + text.substr(from);
}

static std::string uriDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isHex(s[i + 1]) && isHex(s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> treeUris(const std::string& text) {
    // A grant's tree id, encoded: after content://.../tree/ (a document below it, ".../document/...",
    // names it again), or after ScummVM's /saf/ in its paths (Uri.encode leaves !'()* as they are).
    static const char* const kStarts[] = {"content://com.android.externalstorage.documents/tree/", "/saf/"};
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* start : kStarts) {
        const size_t n = strlen(start);
        for (size_t at = text.find(start); at != std::string::npos; at = text.find(start, at + 1)) {
            size_t end = at + n;
            while (end < text.size() && (isalnum((unsigned char)text[end]) || strchr("%-_.~!'()*", text[end]))) end++;
            const std::string doc = uriDecode(uriDecode(text.substr(at + n, end - at - n)));
            const size_t colon = doc.find(':');
            if (colon == std::string::npos || colon == 0) continue;
            std::pair<std::string, std::string> u{doc.substr(0, colon), doc.substr(colon + 1)};
            while (!u.second.empty() && u.second.back() == '/') u.second.pop_back();
            if (std::find(out.begin(), out.end(), u) == out.end()) out.push_back(u);
        }
    }
    return out;
}

std::vector<size_t> unpackedTogether(const std::vector<int64_t>& mtimes, size_t minCount) {
    std::map<int64_t, size_t> perMinute;
    for (int64_t t : mtimes) perMinute[t / 60]++;
    int64_t best = 0;
    size_t bestCount = 0;
    for (const auto& kv : perMinute) {
        auto next = perMinute.find(kv.first + 1);
        const size_t n = kv.second + (next == perMinute.end() ? 0 : next->second);
        if (n > bestCount) { best = kv.first; bestCount = n; }
    }
    std::vector<size_t> out;
    if (bestCount < minCount) return out;
    for (size_t i = 0; i < mtimes.size(); i++)
        if (mtimes[i] / 60 == best || mtimes[i] / 60 == best + 1) out.push_back(i);
    return out;
}

}  // namespace emusync
}  // namespace android
