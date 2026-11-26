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

// A stand-in for a [dropped-linux] file: what it defines is what gen.py may stub. Never compiled.
#include "fake.h"

namespace android {

int gDroppedValue = 42;
int FakeMenu::sCount = 0;

namespace {
int internalOnly(int x) { return x; }   // anonymous namespace: not stubbed
}  // namespace

static int fileLocal() { return 1; }    // static: not stubbed

FakeMenu::FakeMenu() {}
FakeMenu::~FakeMenu() {}

bool FakeMenu::drasticParkEnabled() const {
    if (internalOnly(1)) {
        return fileLocal() > 0;
    }
    return false;
}

void FakeMenu::nsTick(int dtMs, const std::string& why) {
    (void)dtMs;
    (void)why;
}

FakeMenu::Mode FakeMenu::mode() const { return Mode::On; }

const std::string& FakeMenu::label() const {
    static std::string s = "x";
    return s;
}

std::vector<int> FakeMenu::ids(uint32_t max) noexcept { return std::vector<int>(max); }

Runner::Runner() {}
bool Runner::init(const std::string& path, int flags) { return !path.empty() && flags == 0; }
float* Runner::matrix() { return nullptr; }

void nanoRgbFollowSample() {}

}  // namespace android
