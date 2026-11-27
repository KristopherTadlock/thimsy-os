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
//
// ext2/3/4 drives (OA-29): Nano runs as `nano` (OA-27), and a drive made on a PC carries the PC
// user's uids. The session mounts it through an idmapped mount, and where it cannot, read-only with
// "ownership": "needs_decision"; such a drive is not writable (cardRoot, writableFolders) until the
// user agrees that Thimsy takes its top folder: requestTakeOwnership() asks the session for that.
// The shared prompt that asks the user, and the nanoplat call it will use, come with that prompt.
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
    std::string ownership;   // ext2/3/4 only: "nano", "idmapped" or "needs_decision"; else ""
};

// Parses volumes.json's text. false, with *err set, on a syntax error, a version other than 1 or
// a member of the wrong type; members it does not know are ignored.
bool parseVolumesJson(std::string_view text, std::vector<VolumeEntry>* out, std::string* err);

// "XXXX-XXXX" (hex, either case) or a dashed UUID: the ids the session gives drives.
bool looksLikeVolumeId(const std::string& id);

// A mounted drive the session left read-only until the user decides whether Thimsy may take its
// top folder (OA-29).
bool needsOwnershipDecision(const VolumeEntry& v);

// The thimsy-session request that takes drive `id`'s top folder for Nano, "take-ownership:<id>"
// (linux/docs/session.md); "" when `id` is not shaped like a volume id.
std::string takeOwnershipVerb(const std::string& id);

// The user agreed: asks thimsy-session to chown drive `id`'s top folder to `nano` and mount it
// writable. The request goes through `sessionRequest` (by default controlkeys::sessionRequest, the
// one sequence every request of Nano's shares); the answer comes in sys.thimsy.session.done, and the
// drive's new "ownership" in volumes.json. false, with nothing written, for an id that is not a
// volume id.
bool requestTakeOwnership(const std::string& id,
                          const std::function<void(const std::string& verb)>& sessionRequest = nullptr);

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
