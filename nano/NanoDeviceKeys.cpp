#define LOG_TAG "NanoDeviceKeys"

#include "NanoDeviceKeys.h"

#include <strings.h>

#include <algorithm>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <log/log.h>

#include "NanoJson.h"

namespace android {
namespace devkeys {

namespace {

bool hasCode(const std::vector<Key>& keys, int code) {
    return code >= 0 && std::any_of(keys.begin(), keys.end(), [code](const Key& k) { return k.linuxCode == code; });
}

std::vector<Key> builtinFunction() {
    return {{"Anbernic", 68, "F10", true}, {"Mode", 316, "BUTTON_MODE", false}};
}

std::vector<Key> builtinBack() { return {{"Back", 158, "BACK", false}}; }

std::vector<Key> parseKeys(const njson::Value& v) {
    std::vector<Key> out;
    if (!v.isArray()) return out;
    for (const auto& e : v.arr) {
        if (!e.isObject()) continue;
        Key k;
        k.label = e.getString("label");
        const njson::Value* code = e.find("linux");
        if (code && code->isNumber() && code->num >= 0 && code->num <= 0x2ff) k.linuxCode = code->asInt(-1);
        k.android = e.getString("android");
        k.gamepadOnly = e.getBool("gamepad_only", false);
        k.prompt = e.getString("prompt");
        if (k.linuxCode >= 0 || !k.android.empty()) out.push_back(k);
    }
    return out;
}

// A wanted value matches the property case-insensitively, or as a prefix when it ends in '*'.
bool valueMatches(const std::string& want, const std::string& have) {
    if (have.empty() || want.empty()) return false;
    if (want.back() == '*') {
        const size_t n = want.size() - 1;
        return n > 0 && have.size() >= n && strncasecmp(want.c_str(), have.c_str(), n) == 0;
    }
    return strcasecmp(want.c_str(), have.c_str()) == 0;
}

// Every property the alternative names matches one of its values.
bool alternativeMatches(const njson::Value& alt,
                        const std::function<std::string(const std::string&)>& prop) {
    if (!alt.isObject() || alt.obj.empty()) return false;
    for (const auto& [name, want] : alt.obj) {
        const std::string have = prop(name);
        bool any = false;
        if (want.isString()) {
            any = valueMatches(want.str, have);
        } else if (want.isArray()) {
            for (const auto& w : want.arr) any = any || (w.isString() && valueMatches(w.str, have));
        }
        if (!any) return false;
    }
    return true;
}

bool entryMatches(const njson::Value& entry,
                  const std::function<std::string(const std::string&)>& prop) {
    const njson::Value* match = entry.find("match");
    if (!match) return false;
    if (match->isObject()) return alternativeMatches(*match, prop);
    if (!match->isArray()) return false;
    for (const auto& alt : match->arr) {
        if (alternativeMatches(alt, prop)) return true;
    }
    return false;
}

}  // namespace

bool Map::isFunction(int linuxCode) const { return hasCode(function, linuxCode); }

bool Map::isBack(int linuxCode) const { return hasCode(back, linuxCode); }

std::vector<int> Map::modifierCodes() const {
    std::vector<int> out;
    for (const auto& k : function) {
        if (k.linuxCode >= 0) out.push_back(k.linuxCode);
    }
    if (sharedBack) {
        for (const auto& k : back) {
            if (k.linuxCode >= 0) out.push_back(k.linuxCode);
        }
    }
    return out;
}

bool Map::isHomeFunction(int linuxCode) const {
    return linuxCode >= 0 && std::any_of(function.begin(), function.end(), [linuxCode](const Key& k) {
        return k.linuxCode == linuxCode && k.android == "HOME";
    });
}

bool Map::hasHomeFunction() const {
    return std::any_of(function.begin(), function.end(), [](const Key& k) { return k.android == "HOME"; });
}

std::string Map::functionPrompt() const {
    for (const auto& k : sharedBack ? back : function) {
        if (!k.prompt.empty()) return k.prompt;
        if (!k.label.empty()) return k.label;
    }
    return sharedBack ? "Back" : "Anbernic";
}

FunctionRelease functionRelease(bool home, bool hotkeys, bool used, long heldMs) {
    if (used || heldMs < 0) return FunctionRelease::Nothing;
    const long limit = home || hotkeys ? kFunctionHoldMs : kRecoveryHoldMs;
    return heldMs < limit ? FunctionRelease::QuickMenu : FunctionRelease::Nothing;
}

FunctionHold functionHold(bool home, bool hotkeys) {
    if (home) return FunctionHold::Home;
    return hotkeys ? FunctionHold::Quit : FunctionHold::Nothing;
}

BackRelease backRelease(bool sharedBack, bool used, long heldMs) {
    if (used || heldMs < 0) return BackRelease::Nothing;
    if (sharedBack) {
        if (heldMs < kSharedBackTapMs) return BackRelease::Back;
        return heldMs < kSharedBackMenuMs ? BackRelease::QuickMenu : BackRelease::Nothing;
    }
    return heldMs < kRecoveryHoldMs ? BackRelease::Back : BackRelease::Nothing;
}

Map builtin() {
    Map m;
    m.function = builtinFunction();
    m.back = builtinBack();
    return m;
}

Map resolve(const std::string& json, const std::function<std::string(const std::string&)>& prop) {
    njson::Value root;
    if (!njson::parse(json, &root) || !root.isObject()) return builtin();
    Map m;
    m.id = "default";
    m.function = builtinFunction();
    m.back = builtinBack();
    if (const njson::Value* def = root.find("default"); def && def->isObject()) {
        if (const njson::Value* f = def->find("function")) m.function = parseKeys(*f);
        if (const njson::Value* b = def->find("back")) m.back = parseKeys(*b);
        m.sharedBack = def->getString("fallback") == "back";
    }
    if (const njson::Value* devices = root.find("devices"); devices && devices->isArray()) {
        for (const auto& entry : devices->arr) {
            if (!entry.isObject() || !entryMatches(entry, prop)) continue;
            m.id = entry.getString("id", "device");
            m.name = entry.getString("name");
            if (const njson::Value* f = entry.find("function")) m.function = parseKeys(*f);
            if (const njson::Value* b = entry.find("back")) m.back = parseKeys(*b);
            m.sharedBack = entry.getString("fallback") == "back";
            break;
        }
    }
    if (m.back.empty()) m.sharedBack = false;
    return m;
}

const Map& current() {
    static const Map map = [] {
        std::string json;
        if (!base::ReadFileToString(kTablePath, &json)) {
            ALOGI("devkeys: no %s, using the RG Cube's keys", kTablePath);
            return builtin();
        }
        Map m = resolve(json, [](const std::string& name) { return base::GetProperty(name, ""); });
        std::string fn;
        for (int c : m.modifierCodes()) fn += (fn.empty() ? "" : ",") + std::to_string(c);
        ALOGI("devkeys: %s, function/chord keys %s%s", m.id.c_str(), fn.c_str(),
              m.sharedBack ? " (Back doubles as the function button)" : "");
        return m;
    }();
    return map;
}

}  // namespace devkeys
}  // namespace android
