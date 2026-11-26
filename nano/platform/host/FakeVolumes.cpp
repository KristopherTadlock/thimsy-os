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

// See FakeVolumes.h.

#include "FakeVolumes.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace nanohost {

namespace {

class FakeVolumes : public nanoplat::Volumes {
 public:
    explicit FakeVolumes(std::vector<FakeVolume> list) : list_(std::move(list)) {
        for (FakeVolume& v : list_)
            if (v.path.empty()) v.path = "/storage/" + v.id;
    }

    std::string cardRoot() override {
        std::vector<const FakeVolume*> found;
        for (const FakeVolume& v : list_)
            if (in(v) && !v.readOnly) found.push_back(&v);
        std::stable_sort(found.begin(), found.end(), [](const FakeVolume* a, const FakeVolume* b) {
            const bool sa = a->kind == "sd", sb = b->kind == "sd";
            if (sa != sb) return sa;
            if (a->bootDisk != b->bootDisk) return a->bootDisk;
            return a->path < b->path;
        });
        return found.empty() ? std::string() : found[0]->path;
    }

    bool isSlot(const std::string& storagePath) override {
        const size_t sl = storagePath.find_last_of('/');
        const std::string id = sl == std::string::npos ? storagePath : storagePath.substr(sl + 1);
        const FakeVolume* v = find(id);
        return v && in(*v) && v->kind == "sd";
    }

    bool present(const std::string& id) override {
        const FakeVolume* v = find(id);
        return v && in(*v);
    }

    bool absent(const std::string& id) override {
        const FakeVolume* v = find(id);
        return !v || !(in(*v) || v->state == "checking");
    }

    std::vector<std::string> writableFolders(const std::string& vol, const std::string& rel) override {
        std::vector<std::string> found;
        for (const FakeVolume& v : list_) {
            if (v.id == vol || !in(v) || v.readOnly) continue;
            const std::string cand = v.path + rel;
            struct stat st;
            if (stat(cand.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(cand.c_str(), W_OK) == 0)
                found.push_back(cand);
        }
        return found;
    }

 private:
    static bool in(const FakeVolume& v) { return v.state == "mounted"; }

    const FakeVolume* find(const std::string& id) const {
        if (id.empty()) return nullptr;
        for (const FakeVolume& v : list_)
            if (v.id == id) return &v;
        return nullptr;
    }

    std::vector<FakeVolume> list_;   // fixed after construction: any thread may read it
};

}  // namespace

std::unique_ptr<nanoplat::Volumes> makeFakeVolumes(std::vector<FakeVolume> volumes) {
    return std::unique_ptr<nanoplat::Volumes>(new FakeVolumes(std::move(volumes)));
}

}  // namespace nanohost
