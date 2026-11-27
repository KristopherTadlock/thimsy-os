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

// The host's nanoplat::Capabilities (NanoPlatform.h; W6-caps): every capability is true, as the
// firmware's, unless the host config's "caps" object answers it ({"FactoryReset": false, ...},
// HostConfig.h), so a scenario can show the menus as a platform without a feature shows them. Names
// are the Cap enumerators'. Any thread.
#pragma once

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

// The config's answers, in its order (a later answer for the same capability wins).
using CapAnswers = std::vector<std::pair<nanoplat::Cap, bool>>;

std::unique_ptr<nanoplat::Capabilities> createFakeCapabilities(const CapAnswers& answers);

// The enumerator's name ("FactoryReset"), or "" for Count and values outside the enum.
const char* capName(nanoplat::Cap cap);
// The capability named `name`; false when no capability has that name.
bool capFromName(std::string_view name, nanoplat::Cap* cap);

}  // namespace nanohost
