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

// The firmware's nanoplat::Capabilities (NanoPlatform.h; docs/linux/ARCHITECTURE-AUDIT.md step 3):
// Android does every one of them today, so each answer is true and gating a menu entry on has()
// changes nothing on Android. Whether a feature applies to the device (a second panel, a slide) is
// the device's question, asked where it was asked before.

#include <memory>

#include "Capabilities.h"
#include "NanoPlatform.h"

namespace nanofirmware {

namespace {

class FirmwareCapabilities : public nanoplat::Capabilities {
 public:
    bool has(nanoplat::Cap) const override { return true; }
};

}  // namespace

std::unique_ptr<nanoplat::Capabilities> createCapabilities() {
    return std::unique_ptr<nanoplat::Capabilities>(new FirmwareCapabilities);
}

}  // namespace nanofirmware
