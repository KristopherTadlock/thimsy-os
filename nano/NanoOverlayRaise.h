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

// What a raise of the overlay prepares (NanoMenuOverlay.cpp). PhoneWindowManager asks for the
// raise on the chord's press (sys.gammaos.nano.qs_summon); the button map shows alone over the
// game, so its raise skips the XMB's lists and GPU working set and the map draws at once.
#pragma once

namespace android {
namespace overlayraise {

// sys.gammaos.nano.qs_summon
constexpr int kSummonNone = 0;
constexpr int kSummonQuickMenu = 1;     // the function button let go on its own
constexpr int kSummonApplications = 2;  // function button + Start
constexpr int kSummonButtonMap = 3;     // function button + the Button Map Button

// The raise is for the button map alone over a running app: the XMB waits.
inline bool mapOnly(int summon, bool wallpaper) { return summon == kSummonButtonMap && !wallpaper; }

// A raise that left the XMB waiting catches up once the overlay is up and the map alone is not
// what it shows (the map closed into the menu, or another summon opened it).
inline bool catchUp(bool deferred, bool shown, bool mapAloneUp) { return deferred && shown && !mapAloneUp; }

// The offscreen wave behind the scrim (overlay.wave, or the music player's Waves visualizer) feeds
// the XMB's glass icons. The map alone draws none, and a deferred raise left the wave shut down with
// the parked GPU working set: drawing it would reload it, unfrozen, on the map's first frame.
inline bool scrimWave(bool waveOn, bool musicWaves, bool mapAloneUp) { return (waveOn || musicWaves) && !mapAloneUp; }

}  // namespace overlayraise
}  // namespace android
