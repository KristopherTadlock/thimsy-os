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

// The Linux nanoplat::Capabilities (NanoPlatform.h; docs/linux/ARCHITECTURE-AUDIT.md step 3, R3).
// A capability is true only once Linux implements it natively; today none of the listed ones is, so
// every answer is false and the menus hide them (Capabilities.cpp says why for each). Never an
// Android action attempted on Linux: the hidden entries are the ones that would have been refused
// by the shell gate or stored with no effect.
#pragma once

#include <memory>

#include "NanoPlatform.h"

namespace nanolinux {
namespace caps {

std::unique_ptr<nanoplat::Capabilities> create();

}  // namespace caps
}  // namespace nanolinux
