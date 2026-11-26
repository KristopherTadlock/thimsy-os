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

// A stand-in for [core]: it calls what the dropped file would have defined. selftest.sh links it
// with gen.py's stubs (and no dropped.cpp) and runs it: every call returns a zero value.
#include <cstdio>

#include "fake.h"

int main() {
    using namespace android;
    FakeMenu m;
    int bad = 0;
    if (m.drasticParkEnabled()) bad |= 1;
    m.nsTick(16, "frame");
    if (m.mode() != FakeMenu::Mode::Off) bad |= 2;
    if (!m.label().empty()) bad |= 4;
    if (!m.ids(4).empty()) bad |= 8;
    if (m.core(1) != 2) bad |= 16;
    if (FakeMenu::sCount != 0 || gDroppedValue != 0) bad |= 32;
    Runner r;
    if (r.init("/x") || r.matrix() != nullptr) bad |= 64;
    nanoRgbFollowSample();
    nanoRgbFollowSample();
    std::printf("stubgen selftest: %s (%d)\n", bad ? "FAILED" : "ok", bad);
    return bad;
}
