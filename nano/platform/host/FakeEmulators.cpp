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

// The host's scripted Emulators: see FakeEmulators.h.

#include "FakeEmulators.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nanohost {

namespace {

bool listed(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

class FakeEmulators : public nanoplat::Emulators {
 public:
    explicit FakeEmulators(FakeEmulatorsConfig config) : config_(std::move(config)) {}

    bool coreInstalled(const std::string& coreSo) override {
        if (coreSo.empty()) return true;
        return listed(config_.cores, coreSo.substr(coreSo.find_last_of('/') + 1));
    }

    bool appInstalled(const std::string& package) override {
        return package.empty() || listed(config_.apps, package);
    }

    std::vector<nanoplat::SystemEmulator> imageDefaults() override { return config_.defaults; }

 private:
    const FakeEmulatorsConfig config_;
};

}  // namespace

std::unique_ptr<nanoplat::Emulators> makeFakeEmulators(FakeEmulatorsConfig config) {
    return std::unique_ptr<nanoplat::Emulators>(new FakeEmulators(std::move(config)));
}

}  // namespace nanohost
