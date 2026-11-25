// NanoI18n - runtime, resource-file based translation for the nano UIs.
//
// Translations live in per-language JSON resource files (English-keyed objects)
// so they can be edited/extended without recompiling:
//   /data/system/nano_i18n/<code>.json   (user/OTA override, checked first)
//   /system/etc/gammaos-nano/i18n/<code>.json   (shipped)
// where <code> is "es", "fr", "ja", "zh-cn", "zh-tw", ... ("en" = passthrough).
//
// trDyn(english) returns the translation for the loaded locale, or the original
// English pointer when there is no entry - so dynamic/non-UI strings (SSIDs,
// numbers, paths) pass straight through. Both gammaos-nano and drastic-nano
// compile this and share the same resource files via the system locale.
#pragma once

#include <string>

namespace android {

// Make the given resource code's translations current (see above); nullptr/empty/"en" is English
// passthrough. Each language's file is read once; cheap to call on every locale change. Pointers
// trDyn returned stay valid after a change, and trDyn may be called from any thread.
void i18nLoad(const char* localeCode);

// i18nLoad from the text of a <code>.json resource instead of the file (tests, tools). A language
// loaded before is replaced, but its strings stay valid for anyone holding them.
void i18nLoadFromText(const char* localeCode, const std::string& jsonText);

// Translate an English UI string for the currently loaded locale. Returns the
// argument unchanged when there is no translation (or the locale is English).
// format_arg: a translated format is checked against the call's arguments by its English key.
const char* trDyn(const char* english) __attribute__((format_arg(1)));

// Whether a translation may replace an English key that is used as a printf format: both have
// the same conversions in the same order. Translations that fail this are not loaded.
bool i18nFormatSafe(const std::string& key, const std::string& value);

} // namespace android
