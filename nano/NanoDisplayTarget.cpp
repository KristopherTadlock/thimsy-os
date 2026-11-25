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

#include "NanoDisplayTarget.h"

namespace android {
namespace nanodisplay {

Choice choose(const std::vector<Display>& displays, int primaryPort, bool dualMode) {
    Choice c;
    if (displays.empty()) return c;
    int display0 = -1, atPort = -1;
    for (size_t i = 0; i < displays.size(); i++) {
        const Display& d = displays[i];
        // A panel showing layer stack 0 is display 0, even beside a monitor mirroring it.
        if (d.hasLayerStack && d.layerStack == 0 &&
            (display0 < 0 || (d.internal && !displays[display0].internal)))
            display0 = (int)i;
        if (atPort < 0 && primaryPort > 0 && d.port == primaryPort && d.internal) atPort = (int)i;
    }
    c.shown = display0 >= 0;
    if (display0 >= 0 && !displays[display0].internal) {
        c.target = display0;
        c.monitor = true;
        if (!dualMode) return c;
        // Only once DisplayManager has switched the panel on beside the monitor: until then it is
        // blanked (docked), and the mode property alone says nothing about the screens. The screens
        // also blank one after the other around sleep: blankSecond lets dual screen stay on then.
        for (size_t i = 0; i < displays.size(); i++) {
            const Display& d = displays[i];
            if (d.internal && (atPort < 0 || (int)i == atPort)) {
                if (d.hasLayerStack && d.layerStack != kBlankLayerStack) c.second = (int)i;
                else c.blankSecond = (int)i;
                break;
            }
        }
        return c;
    }
    c.target = atPort >= 0 ? atPort : (display0 >= 0 ? display0 : 0);
    c.monitor = !displays[c.target].internal;
    if (c.monitor) return c;
    for (size_t i = 0; i < displays.size(); i++)
        if ((int)i != c.target && displays[i].internal) { c.second = (int)i; break; }
    return c;
}

bool isMonitor(bool external, bool connectorConnected, bool hasProductInfo, bool twoPanels) {
    return external && connectorConnected && (hasProductInfo || !twoPanels);
}

bool uiOnPanel(const UiState& s) {
    if (s.overGame) return true;
    if (s.media) return false;
    return s.menu || s.settingsOpen;
}

}  // namespace nanodisplay
}  // namespace android
