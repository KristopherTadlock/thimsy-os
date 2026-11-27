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

// See Volumes.h.

#define LOG_TAG "ThimsyVolumes"

#include "Volumes.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include <log/log.h>
#include <nanocompat/nanocompat.h>
#include <thimsy/json.h>

#include "ControlKeys.h"

namespace nanolinux {

namespace {

using thimsy::json::Value;

bool readText(const std::string& path, std::string* out, std::string* err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = "open " + path + ": " + strerror(errno);
        return false;
    }
    out->clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            *err = "read " + path + ": " + strerror(errno);
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        out->append(buf, (size_t)n);
    }
    ::close(fd);
    return true;
}

bool isHex(char c) { return isxdigit((unsigned char)c) != 0; }

// The folder's st_dev differs from storageDir's: a drive is mounted on it (plan 7.3).
bool stDevDiffers(const std::string& storageDir, const std::string& path) {
    struct stat a, b;
    if (stat(path.c_str(), &a) != 0 || !S_ISDIR(a.st_mode)) return false;
    if (stat(storageDir.c_str(), &b) != 0) return false;
    return a.st_dev != b.st_dev;
}

bool writableDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(path.c_str(), W_OK) == 0;
}

std::string lastComponent(const std::string& path) {
    const size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

class LinuxVolumes : public nanoplat::Volumes {
 public:
    explicit LinuxVolumes(VolumesOptions options)
        : opt_(std::move(options)), dirty_(std::make_shared<std::atomic<bool>>(true)) {
        if (!opt_.isMount) {
            const std::string dir = opt_.storageDir;
            opt_.isMount = [dir](const std::string& path) { return stDevDiffers(dir, path); };
        }
        // The session bumps the key after each write of the file: read it again on the next call.
        std::shared_ptr<std::atomic<bool>> dirty = dirty_;
        observer_ = nanocompat::addPropertyObserver(
                opt_.serialKey, [dirty](std::string_view, std::string_view) { dirty->store(true); });
    }
    ~LinuxVolumes() override { nanocompat::removePropertyObserver(observer_); }

    // The writable drives, the microSD slot's first (as on Android), then the boot card's (its games
    // drive is the default library, decision I44), then by path.
    std::string cardRoot() override {
        std::vector<VolumeEntry> found;
        for (const VolumeEntry& v : snapshot())
            if (inUse(v) && !v.readOnly && !readOnlyForNano(v) && access(v.path.c_str(), W_OK) == 0)
                found.push_back(v);
        std::stable_sort(found.begin(), found.end(), [](const VolumeEntry& a, const VolumeEntry& b) {
            const bool sa = a.kind == "sd", sb = b.kind == "sd";
            if (sa != sb) return sa;
            if (a.bootDisk != b.bootDisk) return a.bootDisk;
            return a.path < b.path;
        });
        return found.empty() ? std::string() : found[0].path;
    }

    bool isSlot(const std::string& storagePath) override {
        const std::string id = lastComponent(storagePath);
        for (const VolumeEntry& v : snapshot())
            if (v.id == id) return inUse(v) && v.kind == "sd";
        return false;
    }

    bool present(const std::string& id) override {
        if (!looksLikeVolumeId(id)) return access((opt_.storageDir + "/" + id).c_str(), F_OK) == 0;
        for (const VolumeEntry& v : snapshot())
            if (v.id == id && inUse(v)) return true;
        return false;
    }

    // A drive being checked is in (it mounts next); an ejected or failed one is not, as on Android,
    // where vold removes /storage/<id> then.
    bool absent(const std::string& id) override {
        if (!looksLikeVolumeId(id)) {
            struct stat st;
            return stat((opt_.storageDir + "/" + id).c_str(), &st) != 0;
        }
        for (const VolumeEntry& v : snapshot())
            if (v.id == id && (v.state == "checking" || inUse(v))) return false;
        return true;
    }

    std::vector<std::string> writableFolders(const std::string& vol, const std::string& rel) override {
        std::vector<std::string> found;
        for (const VolumeEntry& v : snapshot()) {
            if (v.id == vol || !inUse(v) || readOnlyForNano(v)) continue;
            const std::string cand = v.path + rel;
            if (writableDir(cand)) found.push_back(cand);
        }
        return found;
    }

 private:
    bool inUse(const VolumeEntry& v) const {
        return v.state == "mounted" && !v.id.empty() && !v.path.empty() && opt_.isMount(v.path);
    }

    // The list, read again when the key changed since the last read (or that read failed).
    std::vector<VolumeEntry> snapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        if (dirty_->exchange(false)) {
            std::string text, err;
            std::vector<VolumeEntry> list;
            if (readText(opt_.file, &text, &err) && parseVolumesJson(text, &list, &err)) {
                lastError_.clear();
            } else {
                list.clear();
                dirty_->store(true);
                if (err != lastError_) ALOGW("volumes: %s; no drive is in until it can be read", err.c_str());
                lastError_ = err;
            }
            list_ = std::move(list);
        }
        return list_;
    }

    VolumesOptions opt_;
    std::shared_ptr<std::atomic<bool>> dirty_;
    int observer_ = 0;
    std::mutex mu_;
    std::vector<VolumeEntry> list_;
    std::string lastError_;
};

bool memberString(const Value& o, const char* key, const std::string& where, std::string* out, std::string* err) {
    const Value* v = o.find(key);
    if (!v) return true;
    if (!v->isString()) {
        *err = where + "." + key + ": not a string";
        return false;
    }
    *out = v->asString();
    return true;
}

bool memberBool(const Value& o, const char* key, const std::string& where, bool* out, std::string* err) {
    const Value* v = o.find(key);
    if (!v) return true;
    if (!v->isBool()) {
        *err = where + "." + key + ": not a boolean";
        return false;
    }
    *out = v->asBool();
    return true;
}

}  // namespace

bool looksLikeVolumeId(const std::string& id) {
    if (id.size() == 9) {
        for (size_t i = 0; i < 9; ++i)
            if (i == 4 ? id[i] != '-' : !isHex(id[i])) return false;
        return true;
    }
    if (id.size() == 36) {
        for (size_t i = 0; i < 36; ++i) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? id[i] != '-' : !isHex(id[i])) return false;
        }
        return true;
    }
    return false;
}

bool parseVolumesJson(std::string_view text, std::vector<VolumeEntry>* out, std::string* err) {
    std::string e;
    Value root;
    if (!thimsy::json::parse(text, &root, &e)) {
        if (err) *err = "volumes.json: " + e;
        return false;
    }
    std::vector<VolumeEntry> list;
    if (!root.isObject()) {
        e = "not an object";
    } else if (!root["version"].isInt()) {
        e = "version: missing or not an integer";
    } else if (root["version"].asInt() != 1) {
        e = "version " + std::to_string(root["version"].asInt()) + " is not 1";
    } else if (!root["volumes"].isArray()) {
        e = "volumes: missing or not an array";
    } else {
        const std::vector<Value>& vols = root["volumes"].elements();
        for (size_t i = 0; i < vols.size() && e.empty(); ++i) {
            const std::string where = "volumes[" + std::to_string(i) + "]";
            const Value& o = vols[i];
            VolumeEntry v;
            if (!o.isObject()) {
                e = where + ": not an object";
            } else if (memberString(o, "id", where, &v.id, &e) && memberString(o, "state", where, &v.state, &e) &&
                       memberString(o, "path", where, &v.path, &e) &&
                       memberString(o, "media_rw", where, &v.mediaRw, &e) &&
                       memberString(o, "fs", where, &v.fs, &e) && memberString(o, "label", where, &v.label, &e) &&
                       memberString(o, "kind", where, &v.kind, &e) &&
                       memberBool(o, "boot_disk", where, &v.bootDisk, &e) &&
                       memberBool(o, "read_only", where, &v.readOnly, &e) &&
                       memberString(o, "ownership", where, &v.ownership, &e)) {
                list.push_back(std::move(v));
            }
        }
    }
    if (!e.empty()) {
        if (err) *err = "volumes.json: " + e;
        return false;
    }
    *out = std::move(list);
    return true;
}

bool needsOwnershipDecision(const VolumeEntry& v) { return v.state == "mounted" && v.ownership == "needs_decision"; }

bool readOnlyForNano(const VolumeEntry& v) {
    return v.state == "mounted" &&
           (v.ownership == "needs_decision" || v.ownership == "declined" || v.ownership == "fs_error");
}

std::string takeOwnershipVerb(const std::string& id) {
    return looksLikeVolumeId(id) ? "take-ownership:" + id : std::string();
}

std::string keepReadOnlyVerb(const std::string& id) {
    return looksLikeVolumeId(id) ? "keep-read-only:" + id : std::string();
}

namespace {

// Sends a drive request built by one of the verb functions above; false for an id they refuse.
bool sendDriveRequest(const std::string& verb, const std::string& id,
                      const std::function<void(const std::string& verb)>& sessionRequest) {
    if (verb.empty()) {
        ALOGW("volumes: no request for '%s': not a volume id", id.c_str());
        return false;
    }
    if (sessionRequest) {
        sessionRequest(verb);
    } else {
        controlkeys::sessionRequest(verb);
    }
    return true;
}

}  // namespace

bool requestTakeOwnership(const std::string& id, const std::function<void(const std::string& verb)>& sessionRequest) {
    return sendDriveRequest(takeOwnershipVerb(id), id, sessionRequest);
}

bool requestKeepReadOnly(const std::string& id, const std::function<void(const std::string& verb)>& sessionRequest) {
    return sendDriveRequest(keepReadOnlyVerb(id), id, sessionRequest);
}

std::unique_ptr<nanoplat::Volumes> makeVolumes(VolumesOptions options) {
    return std::unique_ptr<nanoplat::Volumes>(new LinuxVolumes(std::move(options)));
}

}  // namespace nanolinux
