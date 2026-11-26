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

// The process-wide store: the lazy default from THIMSY_PROP_ROOT, nanocompat::init (and replacing
// the store), and a store that can't be opened. This program sets THIMSY_PROP_ROOT in its own main,
// before any property call, and its tests run in order.

// This program has its own main, which sets THIMSY_PROP_ROOT first.
#define THIMSY_GTEST_NO_MAIN 1

#include <cutils/properties.h>
#include <sys/system_properties.h>

#include <stdlib.h>
#include <sys/stat.h>

#include <string>

#include <thimsy/config.h>

#include "compat_test_util.h"

namespace {

compat_test::ScratchDir* gRoot = nullptr;

std::string get(const char* key) {
    char buf[PROPERTY_VALUE_MAX];
    property_get(key, buf, "");
    return buf;
}

}  // namespace

TEST(Init, A_LazyDefaultComesFromThimsyPropRoot) {
    // No init() yet: the first property call opens <root>/persist.props and <root>/run.
    ASSERT_EQ(property_set("sys.test.lazy", "1"), 0);
    EXPECT_EQ(get("sys.test.lazy"), "1");
    struct stat st;
    EXPECT_EQ(stat((*gRoot / "run/sys.test.lazy").c_str(), &st), 0);
    ASSERT_EQ(property_set("persist.test.lazy", "p"), 0);
    EXPECT_EQ(stat((*gRoot / "persist.props").c_str(), &st), 0);
    thimsy::ConfigStore* st0 = nanocompat::store();
    ASSERT_TRUE(st0 != nullptr);
    EXPECT_EQ(st0->roots().runDir, *gRoot / "run");
    EXPECT_FALSE(st0->watching());  // the default store does not watch
}

TEST(Init, B_InitReplacesTheStore) {
    compat_test::ScratchDir other;
    std::string err;
    ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(other), &err)) << err;
    EXPECT_EQ(get("sys.test.lazy"), "");  // the new store's keys only
    ASSERT_EQ(property_set("sys.test.b", "2"), 0);
    struct stat st;
    EXPECT_EQ(stat((other / "run/sys.test.b").c_str(), &st), 0);
}

TEST(Init, C_AFailedInitKeepsThePreviousStore) {
    compat_test::ScratchDir dir;
    ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(dir)));
    ASSERT_EQ(property_set("sys.test.keep", "yes"), 0);
    nanocompat::InitOptions bad;  // empty roots
    std::string err;
    EXPECT_FALSE(nanocompat::init(bad, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(get("sys.test.keep"), "yes");
}

TEST(Init, D_WatchingIsStarted) {
    compat_test::ScratchDir dir;
    ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(dir, true)));
    ASSERT_TRUE(nanocompat::store() != nullptr);
    EXPECT_TRUE(nanocompat::store()->watching());
    // Another writer's change reaches the cache without a read-through.
    auto other = thimsy::ConfigStore::open({dir / "persist.props", dir / "run"});
    ASSERT_TRUE(other != nullptr);
    ASSERT_TRUE(other->set("persist.test.watched", "w"));
    EXPECT_TRUE(compat_test::eventually([] { return get("persist.test.watched") == "w"; }));
}

int main(int argc, char** argv) {
    compat_test::ScratchDir root;
    gRoot = &root;
    setenv("THIMSY_PROP_ROOT", root.path().c_str(), 1);
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    gRoot = nullptr;
    return rc;
}
