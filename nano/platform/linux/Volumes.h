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

// The Linux platform's Volumes (docs/linux/IMPLEMENTATION.md B.6, G7-volumes). thimsy-session owns
// the drives and lists them in /run/thimsy/volumes.json (linux/docs/session.md is the contract),
// bumping sys.thimsy.volumes.serial after each write. This reads that file, again only after the
// key changed, and answers NanoPlatform.h's questions from it, not from /storage's folders: on
// Linux /storage/<id> is a permanent folder on the data partition, there whether the drive is in
// or not, and /storage also holds the data partition's own folders.
//
// A drive counts as in only while its state is "mounted" and its path is a mount (plan 7.3: its
// st_dev differs from /storage's), so a list the session has not yet rewritten after a card was
// pulled is not believed. An id that is not shaped like a volume id (XXXX-XXXX or a UUID) is no
// drive the session can list, so present() and absent() ask the filesystem for it, as Android does.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "NanoPlatform.h"

namespace nanolinux {

// One entry of volumes.json's "volumes" (the fields Nano uses).
struct VolumeEntry {
    std::string id;
    std::string state;
    std::string path;
    std::string mediaRw;
    std::string fs;
    std::string label;
    std::string kind;
    bool bootDisk = false;
    bool readOnly = false;
};

// Parses volumes.json's text. false, with *err set, on a syntax error, a version other than 1 or
// a member of the wrong type; members it does not know are ignored.
bool parseVolumesJson(std::string_view text, std::vector<VolumeEntry>* out, std::string* err);

// "XXXX-XXXX" (hex, either case) or a dashed UUID: the ids the session gives drives.
bool looksLikeVolumeId(const std::string& id);

struct VolumesOptions {
    std::string file = "/run/thimsy/volumes.json";
    std::string serialKey = "sys.thimsy.volumes.serial";   // watched through the property store
    std::string storageDir = "/storage";
    // Whether `path`, a mounted drive's folder, really is a mount: by default its st_dev differs
    // from storageDir's. Tests replace it.
    std::function<bool(const std::string& path)> isMount;
};

std::unique_ptr<nanoplat::Volumes> makeVolumes(VolumesOptions options = VolumesOptions());

}  // namespace nanolinux
