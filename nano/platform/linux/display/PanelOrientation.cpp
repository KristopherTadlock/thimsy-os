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

// readPanelOrientation (Display.h): the first source of the rotation contract, a DRM connector's
// immutable "panel orientation" property, which the kernel sets from the panel's device-tree
// `rotation` (or a quirk table). Only the kernel's uapi headers (drm.h, drm_mode.h) are needed.

#include "Display.h"

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include <drm.h>
#include <drm_mode.h>

namespace nanolinux {
namespace display {

std::string readPanelOrientation(int drmFd, uint32_t connectorId, IoctlFn ioctlFn) {
    if (drmFd < 0 || connectorId == 0 || ioctlFn == nullptr) return "";

    // The connector's properties: their count, then their ids and values.
    struct drm_mode_obj_get_properties op;
    memset(&op, 0, sizeof(op));
    op.obj_id = connectorId;
    op.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    if (ioctlFn(drmFd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) != 0 || op.count_props == 0) return "";
    const uint32_t count = op.count_props;
    std::vector<uint32_t> ids(count);
    std::vector<uint64_t> values(count);
    op.props_ptr = (uint64_t)(uintptr_t)ids.data();
    op.prop_values_ptr = (uint64_t)(uintptr_t)values.data();
    if (ioctlFn(drmFd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) != 0 || op.count_props > count) return "";

    for (uint32_t i = 0; i < op.count_props; i++) {
        struct drm_mode_get_property gp;
        memset(&gp, 0, sizeof(gp));
        gp.prop_id = ids[i];
        if (ioctlFn(drmFd, DRM_IOCTL_MODE_GETPROPERTY, &gp) != 0) continue;
        gp.name[DRM_PROP_NAME_LEN - 1] = '\0';
        if (strcmp(gp.name, "panel orientation") != 0) continue;
        if (!(gp.flags & DRM_MODE_PROP_ENUM) || gp.count_enum_blobs == 0) return "";

        // Its enum entries: value and name. The values array is not asked for (count_values 0).
        const uint32_t enumCount = gp.count_enum_blobs;
        std::vector<struct drm_mode_property_enum> enums(enumCount);
        gp.count_values = 0;
        gp.values_ptr = 0;
        gp.enum_blob_ptr = (uint64_t)(uintptr_t)enums.data();
        if (ioctlFn(drmFd, DRM_IOCTL_MODE_GETPROPERTY, &gp) != 0 || gp.count_enum_blobs > enumCount) return "";
        for (uint32_t e = 0; e < gp.count_enum_blobs; e++) {
            if (enums[e].value != values[i]) continue;
            enums[e].name[DRM_PROP_NAME_LEN - 1] = '\0';
            return enums[e].name;
        }
        return "";
    }
    return "";
}

}  // namespace display
}  // namespace nanolinux
