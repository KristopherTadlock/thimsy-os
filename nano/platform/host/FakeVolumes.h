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

// The host platform's Volumes (docs/linux/IMPLEMENTATION.md B.6, D.3, G7-volumes): a scripted list of
// drives, the host config's "volumes" (HostConfig.h), standing in for the Linux image's
// /run/thimsy/volumes.json. It answers as platform/linux/Volumes.cpp does: a drive is in while its
// state is "mounted"; one being "checking" is not absent; cardRoot() is the first writable drive,
// the microSD slot's first, then the boot card's, then by path. Whether a drive is in comes only
// from the list. writableFolders() also asks the jail's filesystem whether <path><rel> is a folder
// Nano can write to, as the ES-DE store's card-follow needs a real folder there.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "NanoPlatform.h"

namespace nanohost {

struct FakeVolume {
    std::string id;
    std::string path;               // "" for /storage/<id>
    std::string kind = "sd";        // volumes.json's kinds: sd, usb, virtio, other
    std::string state = "mounted";  // volumes.json's states
    bool bootDisk = false;
    bool readOnly = false;
};

std::unique_ptr<nanoplat::Volumes> makeFakeVolumes(std::vector<FakeVolume> volumes);

}  // namespace nanohost
