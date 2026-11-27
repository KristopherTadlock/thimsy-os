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
// chosen set goes while it is up (loadedSetGone).
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

}  // namespace esdethemefb
}  // namespace android
