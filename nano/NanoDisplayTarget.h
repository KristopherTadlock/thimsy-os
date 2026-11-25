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

// The display Nano and drastic-nano draw on. Docked (the default): while a monitor is plugged in it
// is Android's display 0 (layer stack 0) and the panel is switched off; unplugged, the panel is
// display 0 again (LogicalDisplayMapper.gammaosArrangeLocked). So the target is the display showing
// layer stack 0, except on a device with two panels, where persist.gammaos.nano.primary_display
// names the panel Nano draws on unless a monitor has display 0. A second screen is another panel:
// beside a panel target, or, in dual screen mode (persist.gammaos.display.mode=dual, where the panel
// stays on beside the monitor), beside a monitor target. In mirror mode the panel stays display 0 and
// the monitor shows layer stack 0 too: the panel is the target and the monitor is never a second
// screen. Pure, so host tests can use it; NanoDisplayTargetSf.h gathers the displays from
// SurfaceFlinger.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace android {
namespace nanodisplay {

// The layer stack DisplayManager gives a display it blanks or switches off.
constexpr uint32_t kBlankLayerStack = UINT32_MAX;

struct Display {
    uint64_t id = 0;            // physical display id
    int port = 0;
    bool hasLayerStack = false; // SurfaceFlinger reported its state
    uint32_t layerStack = 0;
    bool internal = true;       // a built-in panel, not a monitor
};

struct Choice {
    int target = -1;      // index into the displays; -1 when there are none
    int second = -1;      // a second panel to draw on, -1 for none
    int blankSecond = -1; // dual screen mode: the panel beside the monitor while it shows nothing
    bool monitor = false; // the target is a monitor (the touchscreen belongs to the second, if any)
    bool shown = false;   // a display shows layer stack 0 (none while the screens are off)
};

Choice choose(const std::vector<Display>& displays, int primaryPort, bool dualMode = false);

// A display SurfaceFlinger reports is a monitor, as system_server decides it
// (GammaosScreenArrangement.externalIsMonitor): one the composer calls external while a DisplayPort
// or HDMI connector has a monitor, and, on a device with two panels (where the composer calls the
// second panel external too), only one whose EDID names it.
bool isMonitor(bool external, bool connectorConnected, bool hasProductInfo, bool twoPanels);

// Dual screen mode: what Nano shows, for where its UI draws.
struct UiState {
    bool overGame = false;      // the overlay is up over a running game
    bool media = false;         // a video, photo, PDF, music or ES-DE media viewer is up
    bool menu = false;          // the Quick Menu, Applications, the button map or the ES-DE menu
    bool settingsOpen = false;  // inside Nano's settings (past the Settings category itself)
};

// The UI draws on the panel beside the monitor: over a game always (the game keeps the monitor),
// else for the menus and settings; media and the home stay on the monitor.
bool uiOnPanel(const UiState& s);

}  // namespace nanodisplay
}  // namespace android
