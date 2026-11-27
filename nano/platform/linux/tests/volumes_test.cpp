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

// The Linux Volumes (Volumes.h): volumes.json as thimsy-session writes it (linux/docs/session.md;
// the files in data/ are the session's own output, see data/README), and the answers given from it
// on a scratch /storage, with the property store's serial key driving the re-reads.

#include "Volumes.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <cutils/properties.h>

#include "compat_test_util.h"

using nanolinux::VolumeEntry;

#ifndef PLAT_TEST_DATA_DIR
#error "platform.mk defines PLAT_TEST_DATA_DIR"
#endif

namespace {

void writeText(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    ASSERT_TRUE(f != nullptr) << path;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// One volumes.json entry as the session writes it (VolumeManager::toJson's member order).
std::string entry(const std::string& id, const std::string& state, const std::string& path, const std::string& kind,
                  bool bootDisk = false, bool readOnly = false) {
    return "    {\n      \"id\": \"" + id + "\",\n      \"state\": \"" + state + "\",\n      \"path\": \"" + path +
           "\",\n      \"media_rw\": \"/mnt/media_rw/" + id + "\",\n      \"dev\": \"/dev/mmcblk1p1\",\n" +
           "      \"disk\": \"mmcblk1\",\n      \"partition\": 1,\n      \"fs\": \"vfat\",\n      \"label\": \"L\",\n" +
           "      \"kind\": \"" + kind + "\",\n      \"boot_disk\": " + (bootDisk ? "true" : "false") +
           ",\n      \"read_only\": " + (readOnly ? "true" : "false") + ",\n      \"size_bytes\": 1024\n    }";
}

// The same entry with an ext4 drive's "ownership" member, which the session writes after size_bytes.
std::string withOwnership(std::string e, const std::string& ownership) {
    const std::string tail = "\"size_bytes\": 1024\n    }";
    const size_t at = e.find(tail);
    if (at != std::string::npos)
        e.replace(at, tail.size(), "\"size_bytes\": 1024,\n      \"ownership\": \"" + ownership + "\"\n    }");
    return e;
}

std::string doc(const std::vector<std::string>& entries) {
    std::string s = "{\n  \"version\": 1,\n  \"volumes\": [";
    for (size_t i = 0; i < entries.size(); ++i) s += (i ? ",\n" : "\n") + entries[i];
    return s + (entries.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

// A scratch /storage with drive folders, the session's file, and its own property store. Every
// folder listed in `mounts` counts as a mount (isMount), as a real drive's st_dev would make it.
class VolumesTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ASSERT_TRUE(nanocompat::init(compat_test::storeOptions(dir)));
        storage = dir / "storage";
        ASSERT_EQ(mkdir(storage.c_str(), 0755), 0);
        file = dir / "volumes.json";
    }

    std::unique_ptr<nanoplat::Volumes> make() {
        nanolinux::VolumesOptions o;
        o.file = file;
        o.storageDir = storage;
        o.isMount = [this](const std::string& p) { return mounts.count(p) != 0; };
        return nanolinux::makeVolumes(o);
    }

    std::string drive(const std::string& id) {
        const std::string p = storage + "/" + id;
        mkdir(p.c_str(), 0755);
        mounts.insert(p);
        return p;
    }

    void publish(const std::vector<std::string>& entries) {
        writeText(file, doc(entries));
        property_set("sys.thimsy.volumes.serial", std::to_string(++serial).c_str());
    }

    compat_test::ScratchDir dir;
    std::string storage, file;
    std::set<std::string> mounts;
    int serial = 0;
};

}  // namespace

TEST(VolumesJson, TheContractsExample) {
    // linux/docs/session.md's example, as written there.
    const char* text = R"({
  "version": 1,
  "volumes": [
    {
      "id": "9602-C755",
      "state": "mounted",
      "path": "/storage/9602-C755",
      "media_rw": "/mnt/media_rw/9602-C755",
      "dev": "/dev/mmcblk1p5",
      "disk": "mmcblk1",
      "partition": 5,
      "fs": "exfat",
      "label": "THIMSY",
      "kind": "sd",
      "boot_disk": true,
      "read_only": false,
      "size_bytes": 60129542144
    }
  ]
})";
    std::vector<VolumeEntry> v;
    std::string err;
    ASSERT_TRUE(nanolinux::parseVolumesJson(text, &v, &err)) << err;
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].id, "9602-C755");
    EXPECT_EQ(v[0].state, "mounted");
    EXPECT_EQ(v[0].path, "/storage/9602-C755");
    EXPECT_EQ(v[0].mediaRw, "/mnt/media_rw/9602-C755");
    EXPECT_EQ(v[0].fs, "exfat");
    EXPECT_EQ(v[0].label, "THIMSY");
    EXPECT_EQ(v[0].kind, "sd");
    EXPECT_TRUE(v[0].bootDisk);
    EXPECT_FALSE(v[0].readOnly);
}

TEST(VolumesJson, ErrorsAndUnknownMembers) {
    std::vector<VolumeEntry> v;
    std::string err;
    EXPECT_TRUE(nanolinux::parseVolumesJson(R"({"version": 1, "volumes": [], "later": {"x": 1}})", &v, &err)) << err;
    EXPECT_TRUE(v.empty());
    ASSERT_TRUE(nanolinux::parseVolumesJson(
            R"({"version": 1, "volumes": [{"id": "", "state": "no_id", "error": "the filesystem has no usable serial"}]})",
            &v, &err)) << err;
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].state, "no_id");
    const char* bad[] = {
        "", "[]", "{\"volumes\": []}", "{\"version\": 2, \"volumes\": []}", "{\"version\": \"1\", \"volumes\": []}",
        "{\"version\": 1}", "{\"version\": 1, \"volumes\": {}}", "{\"version\": 1, \"volumes\": [3]}",
        "{\"version\": 1, \"volumes\": [{\"id\": 3}]}", "{\"version\": 1, \"volumes\": [{\"read_only\": \"no\"}]}",
        "{\"version\": 1, \"volumes\": [],}",
    };
    for (const char* t : bad) {
        std::vector<VolumeEntry> keep(1);
        err.clear();
        EXPECT_FALSE(nanolinux::parseVolumesJson(t, &keep, &err)) << t;
        EXPECT_FALSE(err.empty()) << t;
        EXPECT_EQ(keep.size(), 1u) << t;   // left alone on failure
    }
}

// Every file in data/ is thimsy-session's own volumes.json (its E2E run, data/README): each parses,
// and each entry's fields are those of the contract.
TEST(VolumesJson, TheSessionsOwnFiles) {
    const std::string dataDir = PLAT_TEST_DATA_DIR;
    DIR* d = opendir(dataDir.c_str());
    ASSERT_TRUE(d != nullptr) << dataDir;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > 5 && n.compare(0, 8, "volumes-") == 0 && n.compare(n.size() - 5, 5, ".json") == 0)
            names.push_back(n);
    }
    closedir(d);
    ASSERT_FALSE(names.empty()) << dataDir;
    const std::set<std::string> states = {"checking", "mounted", "ejected", "check_failed",
                                          "mount_failed", "unsupported", "no_id", "duplicate"};
    const std::set<std::string> kinds = {"sd", "usb", "virtio", "other"};
    size_t entries = 0;
    for (const std::string& n : names) {
        std::vector<VolumeEntry> v;
        std::string err;
        ASSERT_TRUE(nanolinux::parseVolumesJson(compat_test::readFile(dataDir + "/" + n), &v, &err)) << n << ": " << err;
        for (const VolumeEntry& e : v) {
            ++entries;
            EXPECT_TRUE(states.count(e.state) == 1) << n << ": " << e.state;
            EXPECT_TRUE(kinds.count(e.kind) == 1) << n << ": " << e.kind;
            if (e.id.empty()) continue;
            EXPECT_TRUE(nanolinux::looksLikeVolumeId(e.id)) << n << ": " << e.id;
            EXPECT_EQ(e.path, "/storage/" + e.id) << n;
            EXPECT_EQ(e.mediaRw, "/mnt/media_rw/" + e.id) << n;
        }
    }
    EXPECT_GT(entries, 0u);
}

// The session's E2E run, file by file: the boot card's games drive being checked, then mounted, the
// second card ejected, then the games drive pulled. The folders are taken as mounts here (isMount);
// cardRoot() needs writable folders and is left to the scratch tests below.
TEST_F(VolumesTest, TheSessionsOwnFilesAnswered) {
    mounts = {"/storage/9602-C755", "/storage/1234-ABCD"};
    auto v = make();
    auto load = [&](const char* name) {
        writeText(file, compat_test::readFile(std::string(PLAT_TEST_DATA_DIR) + "/" + name));
        property_set("sys.thimsy.volumes.serial", std::to_string(++serial).c_str());
    };
    load("volumes-e2e-00-boot.json");
    EXPECT_TRUE(v->absent("1234-ABCD"));
    EXPECT_FALSE(v->present("1234-ABCD"));
    load("volumes-e2e-01-coldplug.json");
    EXPECT_TRUE(v->present("1234-ABCD"));
    EXPECT_TRUE(v->isSlot("/storage/1234-ABCD"));
    load("volumes-e2e-02-games-checking.json");
    EXPECT_FALSE(v->present("9602-C755"));
    EXPECT_FALSE(v->absent("9602-C755"));
    EXPECT_FALSE(v->isSlot("/storage/9602-C755"));
    load("volumes-e2e-03-games-mounted.json");
    EXPECT_TRUE(v->present("9602-C755"));
    EXPECT_TRUE(v->isSlot("/storage/9602-C755"));
    load("volumes-e2e-04-ejected.json");
    EXPECT_TRUE(v->present("9602-C755"));
    EXPECT_FALSE(v->present("1234-ABCD"));
    EXPECT_TRUE(v->absent("1234-ABCD"));
    load("volumes-e2e-05-pulled.json");
    EXPECT_FALSE(v->present("9602-C755"));
    EXPECT_TRUE(v->absent("9602-C755"));
}

TEST(VolumesJson, VolumeIds) {
    EXPECT_TRUE(nanolinux::looksLikeVolumeId("9602-C755"));
    EXPECT_TRUE(nanolinux::looksLikeVolumeId("1234-abcd"));
    EXPECT_TRUE(nanolinux::looksLikeVolumeId("0f3c2a9e-5b1d-4c6e-8a7f-112233445566"));
    EXPECT_FALSE(nanolinux::looksLikeVolumeId("emulated"));
    EXPECT_FALSE(nanolinux::looksLikeVolumeId("thimsy"));
    EXPECT_FALSE(nanolinux::looksLikeVolumeId("12345-678"));
    EXPECT_FALSE(nanolinux::looksLikeVolumeId("9602-C75G"));
    EXPECT_FALSE(nanolinux::looksLikeVolumeId(""));
}

TEST_F(VolumesTest, NothingWithoutTheFile) {
    auto v = make();
    EXPECT_EQ(v->cardRoot(), "");
    EXPECT_FALSE(v->present("9602-C755"));
    EXPECT_TRUE(v->absent("9602-C755"));
    EXPECT_TRUE(v->writableFolders("", "").empty());
    // The file appears without a serial bump (a failed read is tried again on the next call).
    const std::string p = drive("9602-C755");
    writeText(file, doc({entry("9602-C755", "mounted", p, "sd", true)}));
    EXPECT_EQ(v->cardRoot(), p);
}

TEST_F(VolumesTest, CardRootPrefersTheSlotThenTheBootCard) {
    const std::string usb = drive("0000-0001"), sd2 = drive("AAAA-0002"), boot = drive("BBBB-0003");
    auto v = make();
    publish({entry("0000-0001", "mounted", usb, "usb"), entry("AAAA-0002", "mounted", sd2, "sd"),
             entry("BBBB-0003", "mounted", boot, "sd", true)});
    EXPECT_EQ(v->cardRoot(), boot);
    publish({entry("0000-0001", "mounted", usb, "usb"), entry("AAAA-0002", "mounted", sd2, "sd")});
    EXPECT_EQ(v->cardRoot(), sd2);
    publish({entry("0000-0001", "mounted", usb, "usb")});
    EXPECT_EQ(v->cardRoot(), usb);
    EXPECT_TRUE(v->isSlot(sd2) == false);   // no longer listed
    EXPECT_FALSE(v->isSlot(usb));
}

TEST_F(VolumesTest, OnlyMountedWritableDrivesCount) {
    const std::string a = drive("AAAA-0001"), b = drive("BBBB-0002"), c = drive("CCCC-0003"), d = drive("DDDD-0004");
    mounts.erase(d);   // listed as mounted, but its folder is the data partition's (pulled, not yet rewritten)
    auto v = make();
    publish({entry("AAAA-0001", "checking", a, "sd"), entry("BBBB-0002", "mounted", b, "sd", false, true),
             entry("CCCC-0003", "ejected", c, "sd"), entry("DDDD-0004", "mounted", d, "sd")});
    EXPECT_EQ(v->cardRoot(), "");   // b is read-only
    EXPECT_FALSE(v->present("AAAA-0001"));
    EXPECT_TRUE(v->present("BBBB-0002"));
    EXPECT_FALSE(v->present("CCCC-0003"));
    EXPECT_FALSE(v->present("DDDD-0004"));
    EXPECT_FALSE(v->absent("AAAA-0001"));   // being checked: in
    EXPECT_FALSE(v->absent("BBBB-0002"));
    EXPECT_TRUE(v->absent("CCCC-0003"));    // ejected: out, as vold removes /storage/<id>
    EXPECT_TRUE(v->absent("DDDD-0004"));
    EXPECT_TRUE(v->absent("EEEE-0005"));    // never listed
    EXPECT_TRUE(v->isSlot(b));
    EXPECT_FALSE(v->isSlot(a));
}

TEST_F(VolumesTest, ReadsAgainOnlyWhenTheKeyChanges) {
    const std::string a = drive("AAAA-0001"), b = drive("BBBB-0002");
    auto v = make();
    publish({entry("AAAA-0001", "mounted", a, "sd")});
    EXPECT_TRUE(v->present("AAAA-0001"));
    writeText(file, doc({entry("BBBB-0002", "mounted", b, "sd")}));   // no bump: not read yet
    EXPECT_TRUE(v->present("AAAA-0001"));
    EXPECT_FALSE(v->present("BBBB-0002"));
    property_set("sys.thimsy.volumes.serial", std::to_string(++serial).c_str());
    EXPECT_FALSE(v->present("AAAA-0001"));
    EXPECT_TRUE(v->present("BBBB-0002"));
    // A broken write is not believed: nothing is in until a good one.
    writeText(file, "{\"version\": 1, \"volumes\": [");
    property_set("sys.thimsy.volumes.serial", std::to_string(++serial).c_str());
    EXPECT_FALSE(v->present("BBBB-0002"));
    writeText(file, doc({entry("BBBB-0002", "mounted", b, "sd")}));
    EXPECT_TRUE(v->present("BBBB-0002"));   // retried without a bump after the failure
}

TEST_F(VolumesTest, WritableFoldersOnTheOtherDrives) {
    const std::string a = drive("AAAA-0001"), b = drive("BBBB-0002"), c = drive("CCCC-0003");
    ASSERT_EQ(mkdir((a + "/ES-DE").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((b + "/ES-DE").c_str(), 0755), 0);
    writeText(c + "/ES-DE", "a file, not a folder");
    auto v = make();
    publish({entry("AAAA-0001", "mounted", a, "sd"), entry("BBBB-0002", "mounted", b, "usb"),
             entry("CCCC-0003", "mounted", c, "sd"), entry("DDDD-0004", "ejected", storage + "/DDDD-0004", "sd")});
    std::vector<std::string> want = {a + "/ES-DE", b + "/ES-DE"};
    EXPECT_EQ(v->writableFolders("", "/ES-DE"), want);
    want = {b + "/ES-DE"};
    EXPECT_EQ(v->writableFolders("AAAA-0001", "/ES-DE"), want);
    EXPECT_TRUE(v->writableFolders("AAAA-0001", "/nope").empty());
}

TEST_F(VolumesTest, OtherFoldersOfStorageAskTheFilesystem) {
    ASSERT_EQ(mkdir((storage + "/thimsy").c_str(), 0755), 0);
    auto v = make();
    publish({});
    EXPECT_TRUE(v->present("thimsy"));
    EXPECT_FALSE(v->absent("thimsy"));
    EXPECT_FALSE(v->present("nothing"));
    EXPECT_TRUE(v->absent("nothing"));
}

// Without the test's isMount: a listed drive whose folder is on /storage's own filesystem is not in
// (plan 7.3's st_dev check); one on another filesystem is.
TEST_F(VolumesTest, TheDefaultMountCheckComparesDevices) {
    const std::string same = storage + "/AAAA-0001";
    ASSERT_EQ(mkdir(same.c_str(), 0755), 0);
    nanolinux::VolumesOptions o;
    o.file = file;
    o.storageDir = storage;
    auto v = nanolinux::makeVolumes(o);
    struct stat proc, here;
    const bool other = stat("/proc", &proc) == 0 && stat(storage.c_str(), &here) == 0 && proc.st_dev != here.st_dev;
    publish({entry("AAAA-0001", "mounted", same, "sd"), entry("BBBB-0002", "mounted", "/proc", "sd")});
    EXPECT_FALSE(v->present("AAAA-0001"));
    EXPECT_TRUE(v->absent("AAAA-0001"));
    if (other) {
        EXPECT_TRUE(v->present("BBBB-0002"));
    }
}

TEST_F(VolumesTest, TheObserverGoesWithTheObject) {
    const std::string a = drive("AAAA-0001");
    {
        auto v = make();
        publish({entry("AAAA-0001", "mounted", a, "sd")});
        EXPECT_TRUE(v->present("AAAA-0001"));
    }
    publish({});   // the destroyed object's observer is gone: nothing to crash on
    auto v = make();
    EXPECT_FALSE(v->present("AAAA-0001"));
}

// --- ext2/3/4 drives and the take-ownership request (OA-29) -------------------------------------

TEST(VolumesJson, TheOwnershipMember) {
    std::vector<VolumeEntry> v;
    std::string err;
    ASSERT_TRUE(nanolinux::parseVolumesJson(
            doc({withOwnership(entry("0b1c2d3e-0000-4000-8000-00000000abcd", "mounted",
                                     "/storage/0b1c2d3e-0000-4000-8000-00000000abcd", "usb"),
                               "needs_decision"),
                 entry("1234-ABCD", "mounted", "/storage/1234-ABCD", "sd")}),
            &v, &err)) << err;
    ASSERT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0].ownership, "needs_decision");
    EXPECT_TRUE(nanolinux::needsOwnershipDecision(v[0]));
    EXPECT_EQ(v[1].ownership, "");
    EXPECT_FALSE(nanolinux::needsOwnershipDecision(v[1]));
    v[0].state = "ejected";
    EXPECT_FALSE(nanolinux::needsOwnershipDecision(v[0]));
    v[0].state = "mounted";
    v[0].ownership = "idmapped";
    EXPECT_FALSE(nanolinux::needsOwnershipDecision(v[0]));
    std::vector<VolumeEntry> keep(1);
    EXPECT_FALSE(nanolinux::parseVolumesJson(R"({"version": 1, "volumes": [{"ownership": 1}]})", &keep, &err));
}

TEST(VolumesJson, TheTakeOwnershipRequest) {
    EXPECT_EQ(nanolinux::takeOwnershipVerb("0b1c2d3e-0000-4000-8000-00000000abcd"),
              "take-ownership:0b1c2d3e-0000-4000-8000-00000000abcd");
    EXPECT_EQ(nanolinux::takeOwnershipVerb("1234-ABCD"), "take-ownership:1234-ABCD");
    EXPECT_EQ(nanolinux::takeOwnershipVerb("../etc"), "");
    EXPECT_EQ(nanolinux::takeOwnershipVerb(""), "");
    std::vector<std::string> sent;
    auto send = [&sent](const std::string& verb) { sent.push_back(verb); };
    EXPECT_TRUE(nanolinux::requestTakeOwnership("0b1c2d3e-0000-4000-8000-00000000abcd", send));
    EXPECT_FALSE(nanolinux::requestTakeOwnership("thimsy", send));
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0], "take-ownership:0b1c2d3e-0000-4000-8000-00000000abcd");
}

TEST_F(VolumesTest, TheDefaultRequestUsesNanosSequence) {
    property_set("sys.thimsy.session.request", "41:sleep");
    EXPECT_TRUE(nanolinux::requestTakeOwnership("1234-ABCD"));
    char value[PROPERTY_VALUE_MAX] = {};
    property_get("sys.thimsy.session.request", value, "");
    EXPECT_STREQ(value, "42:take-ownership:1234-ABCD");
}

TEST_F(VolumesTest, ADriveAwaitingTheDecisionIsNotWritable) {
    const std::string ext = drive("0b1c2d3e-0000-4000-8000-00000000abcd"), fat = drive("AAAA-0001");
    ASSERT_EQ(mkdir((ext + "/ES-DE").c_str(), 0755), 0);
    ASSERT_EQ(mkdir((fat + "/ES-DE").c_str(), 0755), 0);
    auto v = make();
    publish({withOwnership(entry("0b1c2d3e-0000-4000-8000-00000000abcd", "mounted", ext, "sd"), "needs_decision"),
             entry("AAAA-0001", "mounted", fat, "usb")});
    EXPECT_EQ(v->cardRoot(), fat);   // the slot's drive is read-only for now
    EXPECT_TRUE(v->present("0b1c2d3e-0000-4000-8000-00000000abcd"));   // in, readable
    std::vector<std::string> want = {fat + "/ES-DE"};
    EXPECT_EQ(v->writableFolders("", "/ES-DE"), want);
    // Taken: the session lists it as nano's.
    publish({withOwnership(entry("0b1c2d3e-0000-4000-8000-00000000abcd", "mounted", ext, "sd"), "nano"),
             entry("AAAA-0001", "mounted", fat, "usb")});
    EXPECT_EQ(v->cardRoot(), ext);
    want = {ext + "/ES-DE", fat + "/ES-DE"};
    std::vector<std::string> got = v->writableFolders("", "/ES-DE");
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    EXPECT_EQ(got, want);
}
