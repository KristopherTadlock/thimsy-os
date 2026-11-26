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

// A stand-in for NanoMenu.h and a dropped file's header, for gen.py's tests (selftest.sh compiles
// it; test_nano_stubs.py runs gen.py on canned nm output of the same symbols).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace android {

class FakeMenu {
 public:
    enum class Mode { Off, On };

    FakeMenu();
    ~FakeMenu();
    bool drasticParkEnabled() const;
    void nsTick(int dtMs, const std::string& why);
    Mode mode() const;
    const std::string& label() const;
    std::vector<int> ids(uint32_t max) noexcept;
    int core(int x) { return x + 1; }   // inline: never missing
    static int sCount;

 private:
    // A call inside an inline body is not a declaration.
    void helper() { nsTick(0, ""); }
};

class Runner {
 public:
    Runner();
    bool init(const std::string& path, int flags = 0);
    float* matrix();
};

void nanoRgbFollowSample();
extern int gDroppedValue;

}  // namespace android
