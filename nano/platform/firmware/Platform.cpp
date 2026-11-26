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

// The firmware platform's createPlatform() (docs/linux/IMPLEMENTATION.md B.6, B.7 P4): what
// gammaos-nano's main.cpp installs. Each service is today's Android code, moved into
// platform/firmware/<Area>.cpp by the G7 step of its area, which wires it in below.

#include <memory>

#include "NanoPlatform.h"

std::unique_ptr<nanoplat::Platform> nanoplat::createPlatform(const PlatformArgs& /*args*/) {
    std::unique_ptr<Platform> platform(new Platform);
    // The services, one line each (G7 steps).
    return platform;
}
