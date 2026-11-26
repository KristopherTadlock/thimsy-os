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

// The Linux platform's createPlatform() (LinuxPlatform.cpp), installed the way main_linux.cpp
// installs it, with NanoPlatform.cpp's install() and get().

#include <memory>

#include <gtest/gtest.h>

#include "NanoPlatform.h"

namespace {

TEST(LinuxPlatform, CreatesAndInstalls) {
    char arg0[] = "thimsy-nano";
    char* argv[] = {arg0, nullptr};
    nanoplat::PlatformArgs args;
    args.argc = 1;
    args.argv = argv;
    std::unique_ptr<nanoplat::Platform> platform = nanoplat::createPlatform(args);
    ASSERT_TRUE(platform != nullptr);
    EXPECT_TRUE(platform->volumes != nullptr);   // G7-volumes
    nanoplat::Platform* raw = platform.get();
    nanoplat::install(std::move(platform));
    EXPECT_TRUE(&nanoplat::get() == raw);
}

TEST(LinuxPlatform, EachCallMakesItsOwn) {
    nanoplat::PlatformArgs args;
    std::unique_ptr<nanoplat::Platform> a = nanoplat::createPlatform(args);
    std::unique_ptr<nanoplat::Platform> b = nanoplat::createPlatform(args);
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    EXPECT_TRUE(a.get() != b.get());
}

}  // namespace
