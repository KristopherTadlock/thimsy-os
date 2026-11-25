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

// NanoDisplayTarget's displays, from SurfaceFlinger: shared by gammaos-nano and drastic-nano.
#pragma once

#include <cutils/properties.h>
#include <dirent.h>
#include <gui/SurfaceComposerClient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ui/DisplayState.h>
#include <ui/StaticDisplayInfo.h>

#include <string>
#include <vector>

#include "NanoDisplayTarget.h"

namespace android {
namespace nanodisplay {

// A DisplayPort or HDMI connector reports a monitor connected.
inline bool monitorConnected() {
    DIR* d = opendir("/sys/class/drm");
    if (!d) return false;
    bool found = false;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.find("-DP-") == std::string::npos && name.find("-HDMI-") == std::string::npos) continue;
        char status[16] = {0};
        if (FILE* f = fopen(("/sys/class/drm/" + name + "/status").c_str(), "r")) {
            if (fgets(status, sizeof(status), f) == nullptr) status[0] = 0;
            fclose(f);
        }
        if (std::string(status).rfind("connected", 0) == 0) { found = true; break; }
    }
    closedir(d);
    return found;
}

// A device with two panels: Nano draws on a chosen port, or the dual-stack screens are on.
inline bool twoPanels() {
    return property_get_int32("persist.gammaos.nano.primary_display", 0) > 0 ||
           property_get_bool("persist.gammaos.dualstack.enabled", false);
}

// SurfaceFlinger calls every display but the first external when the composer doesn't say, a second
// panel included: which are monitors is isMonitor's rule.
inline std::vector<Display> sfDisplays(const std::vector<PhysicalDisplayId>& ids) {
    std::vector<Display> out;
    const bool connected = monitorConnected();
    const bool dual = twoPanels();
    for (const PhysicalDisplayId& id : ids) {
        Display d;
        d.id = id.value;
        d.port = static_cast<int>(id.getPort());
        ui::StaticDisplayInfo info;
        const bool known = SurfaceComposerClient::getStaticDisplayInfo(id.value, &info) == NO_ERROR;
        d.internal = !(known && isMonitor(info.connectionType != ui::DisplayConnectionType::Internal,
                                          connected, info.deviceProductInfo.has_value(), dual));
        if (const sp<IBinder> token = SurfaceComposerClient::getPhysicalDisplayToken(id)) {
            ui::DisplayState state;
            if (SurfaceComposerClient::getDisplayState(token, &state) == NO_ERROR) {
                d.hasLayerStack = true;
                d.layerStack = state.layerStack.id;
            }
        }
        out.push_back(d);
    }
    return out;
}

// A monitor is in use: SurfaceFlinger lists one (as the screen, beside the panel, or mirroring it),
// not merely a connector reading connected.
inline bool monitorListed() {
    for (const Display& d : sfDisplays(SurfaceComposerClient::getPhysicalDisplayIds()))
        if (!d.internal) return true;
    return false;
}

inline Choice sfChoose(const std::vector<PhysicalDisplayId>& ids) {
    char port[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.nano.primary_display", port, "0");
    char mode[PROPERTY_VALUE_MAX] = {};
    property_get("persist.gammaos.display.mode", mode, "docked");
    return choose(sfDisplays(ids), atoi(port), strcmp(mode, "dual") == 0);
}

}  // namespace nanodisplay
}  // namespace android
