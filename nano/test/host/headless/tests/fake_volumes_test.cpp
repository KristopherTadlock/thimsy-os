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

// FakeVolumes (platform/host/FakeVolumes.h): the host config's scripted drives, parsed by
// HostConfig and answered as the Linux platform answers from volumes.json.

#include <sys/stat.h>

#include <gtest/gtest.h>

#include "headless_test_util.h"
#include "platform/host/FakeVolumes.h"
#include "platform/host/HostConfig.h"

using namespace nanohost;
using headless_test::TmpDir;

namespace {

FakeVolume vol(const std::string& id, const std::string& kind, const std::string& state = "mounted",
               const std::string& path = "") {
    FakeVolume v;
    v.id = id;
    v.kind = kind;
    v.state = state;
    v.path = path;
    return v;
}

TEST(FakeVolumesConfig, ParsesTheList) {
    HostConfig c;
    std::string err;
    ASSERT_TRUE(parseHostConfig("{}", &c, &err)) << err;
    EXPECT_TRUE(c.volumes.empty());
    ASSERT_TRUE(parseHostConfig(R"({"volumes": [{"id": "9602-C755", "boot_disk": true},
        {"id": "1234-ABCD", "path": "/x/1234-ABCD", "kind": "usb", "state": "ejected", "read_only": true}]})",
                                &c, &err)) << err;
    ASSERT_EQ(c.volumes.size(), 2u);
    EXPECT_EQ(c.volumes[0].id, "9602-C755");
    EXPECT_EQ(c.volumes[0].path, "");
    EXPECT_EQ(c.volumes[0].kind, "sd");
    EXPECT_EQ(c.volumes[0].state, "mounted");
    EXPECT_TRUE(c.volumes[0].bootDisk);
    EXPECT_EQ(c.volumes[1].path, "/x/1234-ABCD");
    EXPECT_EQ(c.volumes[1].kind, "usb");
    EXPECT_EQ(c.volumes[1].state, "ejected");
    EXPECT_TRUE(c.volumes[1].readOnly);
    const char* bad[] = {"{\"volumes\": {}}", "{\"volumes\": [3]}", "{\"volumes\": [{\"kind\": \"sd\"}]}",
                         "{\"volumes\": [{\"id\": \"A\", \"size\": 1}]}", "{\"volumes\": [{\"id\": \"A\", \"read_only\": 1}]}"};
    for (const char* t : bad) {
        HostConfig b;
        EXPECT_FALSE(parseHostConfig(t, &b, &err)) << t;
    }
}

TEST(FakeVolumes, TheScriptedListDecides) {
    auto v = makeFakeVolumes({vol("0000-0001", "usb"), vol("AAAA-0002", "sd"), vol("BBBB-0003", "sd", "checking"),
                              vol("CCCC-0004", "sd", "ejected")});
    EXPECT_EQ(v->cardRoot(), "/storage/AAAA-0002");
    EXPECT_TRUE(v->isSlot("/storage/AAAA-0002"));
    EXPECT_FALSE(v->isSlot("/storage/0000-0001"));
    EXPECT_FALSE(v->isSlot("/storage/BBBB-0003"));
    EXPECT_TRUE(v->present("0000-0001"));
    EXPECT_FALSE(v->present("BBBB-0003"));
    EXPECT_FALSE(v->present("CCCC-0004"));
    EXPECT_FALSE(v->present("emulated"));
    EXPECT_FALSE(v->absent("AAAA-0002"));
    EXPECT_FALSE(v->absent("BBBB-0003"));   // being checked: in
    EXPECT_TRUE(v->absent("CCCC-0004"));
    EXPECT_TRUE(v->absent("DDDD-0005"));
    auto none = makeFakeVolumes({});
    EXPECT_EQ(none->cardRoot(), "");
}

TEST(FakeVolumes, CardRootOrder) {
    FakeVolume boot = vol("BBBB-0002", "sd");
    boot.bootDisk = true;
    FakeVolume ro = vol("0000-0000", "sd");
    ro.readOnly = true;
    auto v = makeFakeVolumes({vol("0000-0001", "usb"), vol("AAAA-0001", "sd"), boot, ro});
    EXPECT_EQ(v->cardRoot(), "/storage/BBBB-0002");
    auto w = makeFakeVolumes({vol("0000-0001", "usb"), ro});
    EXPECT_EQ(w->cardRoot(), "/storage/0000-0001");
}

TEST(FakeVolumes, WritableFoldersLookAtTheFolders) {
    TmpDir dir;
    ASSERT_EQ(mkdir((dir / "a").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((dir / "a/ES-DE").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((dir / "b").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((dir / "c").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((dir / "c/ES-DE").c_str(), 0755), 0);
    auto v = makeFakeVolumes({vol("AAAA-0001", "sd", "mounted", dir / "a"), vol("BBBB-0002", "sd", "mounted", dir / "b"),
                              vol("CCCC-0003", "usb", "ejected", dir / "c")});
    std::vector<std::string> want = {dir / "a/ES-DE"};
    EXPECT_EQ(v->writableFolders("", "/ES-DE"), want);
    EXPECT_TRUE(v->writableFolders("AAAA-0001", "/ES-DE").empty());
}

}  // namespace
