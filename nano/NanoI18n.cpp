// NanoI18n - see NanoI18n.h.
#include "NanoI18n.h"
#include "NanoJson.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace android {

using I18nMap = std::unordered_map<std::string, std::string>;
// One map per language, built once and never changed or freed: trDyn runs on worker threads too,
// and callers keep the pointers it returns, so a language change only swaps which map is current.
static std::mutex sI18nLoadM;
static std::map<std::string, std::unique_ptr<const I18nMap>> sI18nMaps;
static std::vector<std::unique_ptr<const I18nMap>> sI18nRetired;   // replaced by i18nLoadFromText
static std::atomic<const I18nMap*> sI18nCur{nullptr};

static bool readFile(const char* path, std::string* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

// The printf conversions of a string, in order ("%-5.2lf" -> "lf"). Translations are used as
// formats, so one whose conversions differ from its English key's is dropped.
static std::string conversions(const std::string& s, bool spaceFlag) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') continue;
        if (i + 1 < s.size() && s[i + 1] == '%') { i++; continue; }
        size_t j = i + 1;
        while (j < s.size() && isdigit((unsigned char)s[j])) j++;
        if (j < s.size() && s[j] == '$') j++; else j = i + 1;      // %1$s
        while (j < s.size() && (strchr("-+#0'", s[j]) || (spaceFlag && s[j] == ' '))) j++;
        if (j < s.size() && s[j] == '*') j++; else while (j < s.size() && isdigit((unsigned char)s[j])) j++;
        if (j < s.size() && s[j] == '.') {
            j++;
            if (j < s.size() && s[j] == '*') j++; else while (j < s.size() && isdigit((unsigned char)s[j])) j++;
        }
        while (j < s.size() && strchr("hlLqjzt", s[j])) j++;
        if (j >= s.size() || !strchr("diouxXeEfFgGaAcspn", s[j])) continue;   // a literal '%'
        out.append(s, i, j + 1 - i);
        out += ' ';
        i = j;
    }
    return out;
}

// A translation of a format (a key with conversions) must have exactly its conversions, the
// space-flag kind ("% d") counted too; a key without any is plain text, where "50 % de" is literal.
bool i18nFormatSafe(const std::string& key, const std::string& value) {
    const std::string k = conversions(key, false);
    if (k != conversions(value, false)) return false;
    return k.empty() || conversions(key, true) == conversions(value, true);
}

static std::unique_ptr<const I18nMap> buildMap(const std::string& text) {
    auto m = std::make_unique<I18nMap>();
    njson::Value v;
    if (!text.empty() && njson::parse(text, &v) && v.isObject()) {
        m->reserve(v.obj.size());
        for (const auto& kv : v.obj) {
            if (kv.second.isString() && !kv.second.str.empty() && i18nFormatSafe(kv.first, kv.second.str))
                m->emplace(kv.first, kv.second.str);
        }
    }
    return m;
}

void i18nLoad(const char* localeCode) {
    const std::string code = (localeCode && localeCode[0]) ? localeCode : "en";
    if (code == "en") { sI18nCur.store(nullptr); return; }   // English: passthrough
    std::lock_guard<std::mutex> lk(sI18nLoadM);
    auto& slot = sI18nMaps[code];
    if (!slot) {   // nanoSetLocale runs every frame on the language picker's preview: read each file once
        char path[256];
        std::string text;
        // /data override first (user/OTA editable), then the shipped resource.
        snprintf(path, sizeof(path), "/data/system/nano_i18n/%s.json", code.c_str());
        if (!readFile(path, &text)) {
            text.clear();
            snprintf(path, sizeof(path), "/system/etc/gammaos-nano/i18n/%s.json", code.c_str());
            if (!readFile(path, &text)) text.clear();
        }
        slot = buildMap(text);
    }
    sI18nCur.store(slot->empty() ? nullptr : slot.get());
}

void i18nLoadFromText(const char* localeCode, const std::string& jsonText) {
    const std::string code = (localeCode && localeCode[0]) ? localeCode : "en";
    if (code == "en") { sI18nCur.store(nullptr); return; }
    std::lock_guard<std::mutex> lk(sI18nLoadM);
    auto& slot = sI18nMaps[code];
    if (slot) sI18nRetired.push_back(std::move(slot));
    slot = buildMap(jsonText);
    sI18nCur.store(slot->empty() ? nullptr : slot.get());
}

const char* trDyn(const char* english) {
    const I18nMap* m = sI18nCur.load();
    if (!english || !english[0] || !m) return english;
    auto it = m->find(english);
    return (it != m->end()) ? it->second.c_str() : english;
}

} // namespace android
