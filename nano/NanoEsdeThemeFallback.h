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

// A chosen ES-DE theme set that is not installed (docs/linux/DECISIONS.md OA-37): for example one
// that was only in a removed card's ES-DE/themes. The ES-DE home shows the default set in its place
// without changing the stored choice (persist.gammaos.nano.esde.themeset), remembers the set it
// wants, and loads it once it is there again (NanoThemeEngine.cpp, esdeMissingSetRecheck). These are
// the decisions; the theme engine does the file look-ups and the loads. The same holds when the
// chosen set goes while it is up (loadedSetGone). The stand-in follows each system's variant
// trigger (triggerBase, standInVariant), and at boot the first load waits a little for storage
// before the default stands in (waitForStorage).
#pragma once

#include <cstdint>
#include <string>

namespace android {
namespace esdethemefb {

// How long after the last look a missing set is looked for again when nothing else prompted a
// look (storage coming or going, the theme folder changing, the home raised, the theme menu opened).
constexpr int64_t kRecheckMs = 5000;

// The set a theme load builds: the chosen one when it is installed, else the default when that is,
// else none (the empty string; the load is skipped and the home shows its "not loaded" notice).
inline std::string setToLoad(const std::string& want, bool wantFound, const std::string& def, bool defFound) {
    if (wantFound) return want;
    return defFound ? def : std::string();
}

// The set to look for again after a load: the chosen one when it was not found, unless it is the
// default itself (nothing else could stand in for it). Empty when nothing is missing.
inline std::string missingSet(const std::string& want, bool wantFound, const std::string& def) {
    return wantFound || want == def ? std::string() : want;
}

// Whether the rate-limited look for a missing set is due at nowMs, the last look having been at
// lastMs.
inline bool recheckDue(const std::string& missing, int64_t nowMs, int64_t lastMs) {
    return !missing.empty() && nowMs - lastMs >= kRecheckMs;
}

// Whether the set on screen has gone from under the home: a theme is loaded, the set shown is the
// chosen one (not a stand-in for it), it is not the default (nothing else could stand in for it),
// and its folder is no longer found (its card was pulled, or swapped for one without it). The
// theme is then loaded again, which brings in the default in its place (setToLoad, missingSet).
inline bool loadedSetGone(bool loaded, const std::string& shown, const std::string& chosen,
                          const std::string& def, bool chosenFound) {
    return loaded && !chosen.empty() && shown == chosen && chosen != def && !chosenFound;
}

// The variant whose noMedia/noVideos trigger applies to the default standing in for a set: the one
// it was loaded with (sel) when the theme declares it, else the first the theme declares (first,
// empty when it declares none), which is the one a load selects then (nanoesde::Theme::load). The
// stand-in is loaded with no variant chosen, so it is the theme's first.
inline std::string triggerBase(const std::string& sel, bool selDeclared, const std::string& first) {
    return selDeclared || first.empty() ? sel : first;
}

// The variant the stand-in is parsed with for a system: the trigger's (eff, what the trigger check
// made of base) when one fired, else the stand-in's own selection (sel), so a parse that changes
// nothing has the stand-in load's key.
inline std::string standInVariant(const std::string& sel, const std::string& base, const std::string& eff) {
    return eff == base ? sel : eff;
}

// How long the first load waits for storage at most once the boot screen's startup sound is ready
// (waitForStorage).
constexpr int64_t kStorageWaitMs = 3000;

// Whether the first load at boot waits for storage before the default stands in for a chosen set
// that is not found: the set may be on that storage (its ES-DE/themes), and the wait spares a load
// of the default followed at once by the set's. Only while the boot screen is up (it covers the
// home, so the wait shows nothing) and storage is not readable yet, and for at most kStorageWaitMs
// after the boot screen's startup sound is ready (soundReadyMs; 0 while it is not), as from then on
// the wait is what keeps the boot screen up.
inline bool waitForStorage(bool bootScreen, bool storageReady, int64_t nowMs, int64_t soundReadyMs) {
    return bootScreen && !storageReady && (soundReadyMs == 0 || nowMs - soundReadyMs < kStorageWaitMs);
}

}  // namespace esdethemefb
}  // namespace android
