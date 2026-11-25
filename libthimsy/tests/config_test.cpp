// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/config.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "test_util.h"
#include "thimsy/atomic_file.h"
#include "thimsy/watch.h"

using thimsy::ConfigStore;
using thimsy_test::ScratchDir;

namespace {

class StoreTest : public ::testing::Test {
 protected:
    void SetUp() override {
        ASSERT_FALSE(dir.path().empty());
        roots.persistFile = dir / "config/thimsy/persist.props";
        roots.runDir = dir / "run/props";
        store = open();
        ASSERT_TRUE(store != nullptr);
    }
    void TearDown() override {
        thimsy::setAtomicWriteTestHook(nullptr);
        store.reset();
    }
    std::unique_ptr<ConfigStore> open() {
        std::string err;
        auto s = ConfigStore::open(roots, &err);
        EXPECT_TRUE(s != nullptr) << err;
        return s;
    }

    ScratchDir dir;
    ConfigStore::Roots roots;
    std::unique_ptr<ConfigStore> store;
};

void killSelf(const char*) { raise(SIGKILL); }

}  // namespace

TEST_F(StoreTest, GetAndSet) {
    EXPECT_EQ(store->get("sys.thimsy.a"), "");
    EXPECT_EQ(store->get("sys.thimsy.a", "dflt"), "dflt");
    EXPECT_TRUE(store->set("sys.thimsy.a", "1"));
    EXPECT_EQ(store->get("sys.thimsy.a", "dflt"), "1");
    EXPECT_TRUE(store->has("sys.thimsy.a"));
    EXPECT_EQ(thimsy_test::slurp(roots.runDir + "/sys.thimsy.a"), "1");
    EXPECT_TRUE(store->set("debug.x", "with spaces\nand a newline"));
    EXPECT_EQ(store->get("debug.x"), "with spaces\nand a newline");
}

TEST_F(StoreTest, EmptyValueIsUnset) {
    ASSERT_TRUE(store->set("sys.k", "v"));
    ASSERT_TRUE(store->set("persist.k", "v"));
    EXPECT_TRUE(store->set("sys.k", ""));
    EXPECT_TRUE(store->set("persist.k", ""));
    EXPECT_EQ(store->get("sys.k", "d"), "d");
    EXPECT_EQ(store->get("persist.k", "d"), "d");
    struct stat st;
    EXPECT_NE(stat((roots.runDir + "/sys.k").c_str(), &st), 0);
    EXPECT_EQ(thimsy_test::slurp(roots.persistFile).find("persist.k"), std::string::npos);
    EXPECT_TRUE(store->set("sys.never.set", ""));  // removing a missing key is fine
}

TEST_F(StoreTest, AndroidValueLimit) {
    const std::string ok(91, 'x'), tooLong(92, 'x');
    EXPECT_EQ(store->trySet("sys.limit", ok), ConfigStore::SetResult::Ok);
    EXPECT_EQ(store->trySet("sys.limit", tooLong), ConfigStore::SetResult::ValueTooLong);
    EXPECT_EQ(store->get("sys.limit"), ok);
    EXPECT_EQ(store->trySet("persist.limit", tooLong), ConfigStore::SetResult::ValueTooLong);
    EXPECT_EQ(store->trySet("persist.limit", ok), ConfigStore::SetResult::Ok);
    // ro.* is exempt, as on Android.
    const std::string longRo(300, 'r');
    EXPECT_EQ(store->trySet("ro.thimsy.long", longRo), ConfigStore::SetResult::Ok);
    EXPECT_EQ(store->get("ro.thimsy.long"), longRo);
    EXPECT_EQ(store->trySet("sys.nul", std::string("a\0b", 3)), ConfigStore::SetResult::InvalidValue);
}

TEST_F(StoreTest, KeyNames) {
    for (const char* bad : {"", ".a", "a.", "a..b", "a/b", "a b", "../x", "a\nb", "persist.é"}) {
        SCOPED_TRACE(bad);
        EXPECT_FALSE(thimsy::isValidKey(bad));
        EXPECT_EQ(store->trySet(bad, "1"), ConfigStore::SetResult::InvalidKey);
        EXPECT_EQ(store->get(bad, "d"), "d");
    }
    EXPECT_FALSE(thimsy::isValidKey(std::string(256, 'k')));
    EXPECT_TRUE(thimsy::isValidKey(std::string(255, 'k')));
    for (const char* good : {"a", "sys.a-b_c:d@e", "persist.gammaos.nano.x", "vendor.X9"}) {
        SCOPED_TRACE(good);
        EXPECT_TRUE(thimsy::isValidKey(good));
    }
}

TEST_F(StoreTest, ReadOnlyKeysAreSetOnceAndRefusedAfterBoot) {
    EXPECT_FALSE(store->readOnlySealed());
    EXPECT_TRUE(store->set("ro.thimsy.a", "1"));
    EXPECT_EQ(store->trySet("ro.thimsy.a", "2"), ConfigStore::SetResult::ReadOnly);
    EXPECT_EQ(store->get("ro.thimsy.a"), "1");
    EXPECT_TRUE(store->set("ro.thimsy.b", "2"));
    EXPECT_TRUE(store->sealReadOnly());
    EXPECT_TRUE(store->readOnlySealed());
    EXPECT_EQ(store->trySet("ro.thimsy.c", "3"), ConfigStore::SetResult::ReadOnly);
    EXPECT_EQ(store->trySet("ro.thimsy.a", ""), ConfigStore::SetResult::ReadOnly);
    EXPECT_EQ(store->get("ro.thimsy.c", "none"), "none");
    // Another process (here: another store) sees the seal and the values.
    auto other = open();
    ASSERT_TRUE(other != nullptr);
    EXPECT_TRUE(other->readOnlySealed());
    EXPECT_EQ(other->trySet("ro.thimsy.d", "4"), ConfigStore::SetResult::ReadOnly);
    EXPECT_EQ(other->get("ro.thimsy.b"), "2");
    EXPECT_EQ(other->list("ro.").size(), 2u);
}

TEST_F(StoreTest, PersistRoundTripAndMode) {
    const mode_t old = umask(0);  // the file must be 0600 whatever the umask
    const std::string tricky = "line1\nline2\\x=y\r";
    EXPECT_TRUE(store->set("persist.thimsy.b", "2"));
    EXPECT_TRUE(store->set("persist.thimsy.a", tricky));
    EXPECT_TRUE(store->set("persist.thimsy.c", "3"));
    umask(old);
    struct stat st;
    ASSERT_EQ(stat(roots.persistFile.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
    const std::string text = thimsy_test::slurp(roots.persistFile);
    const size_t a = text.find("persist.thimsy.a=line1\\nline2\\\\x=y\\r\n");
    const size_t b = text.find("persist.thimsy.b=2\n");
    const size_t c = text.find("persist.thimsy.c=3\n");
    EXPECT_NE(a, std::string::npos) << text;
    EXPECT_LT(a, b);  // sorted
    EXPECT_LT(b, c);
    store.reset();
    auto again = open();
    ASSERT_TRUE(again != nullptr);
    EXPECT_EQ(again->get("persist.thimsy.a"), tricky);
    EXPECT_EQ(again->get("persist.thimsy.b"), "2");
    EXPECT_EQ(again->list("persist.").size(), 3u);
}

TEST_F(StoreTest, PersistFileToleratesHandEdits) {
    thimsy_test::spit(roots.persistFile,
                      "# comment\n\npersist.ok=1\nno equals sign\nsys.not.persist=2\n..bad=3\npersist.empty=\n");
    auto s = open();
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->get("persist.ok"), "1");
    EXPECT_EQ(s->list().size(), 1u);
}

TEST_F(StoreTest, WriterKilledBeforeRenameLeavesTheOldFile) {
    ASSERT_TRUE(store->set("persist.crash", "old"));
    ASSERT_TRUE(store->set("sys.crash", "old"));
    for (const char* key : {"persist.crash", "sys.crash"}) {
        SCOPED_TRACE(key);
        const int status = thimsy_test::runInChild([&] {
            thimsy::setAtomicWriteTestHook(killSelf);
            auto s = ConfigStore::open(roots);
            if (!s) return 3;
            s->set(key, "new");
            return 4;  // not reached: the hook kills the child
        });
        ASSERT_TRUE(WIFSIGNALED(status));
        EXPECT_EQ(WTERMSIG(status), SIGKILL);
        auto fresh = open();
        ASSERT_TRUE(fresh != nullptr);
        EXPECT_EQ(fresh->get(key), "old");
    }
    // The killed persist writer left its temp file; the next writer clears it under the lock.
    const std::string configDir = dir / "config/thimsy";
    auto stray = [&] {
        int n = 0;
        for (const auto& name : thimsy_test::listDir(configDir)) n += name.rfind(".persist.props.tmp.", 0) == 0;
        return n;
    };
    EXPECT_EQ(stray(), 1);
    EXPECT_TRUE(store->set("persist.crash", "newer"));
    EXPECT_EQ(stray(), 0);
    EXPECT_EQ(store->get("persist.crash"), "newer");
}

TEST_F(StoreTest, TwoProcessesWritingDifferentKeys) {
    constexpr int kKeys = 40;
    auto writer = [&](const char* tag) {
        return [this, tag] {
            auto s = ConfigStore::open(roots);
            if (!s) return 3;
            for (int i = 0; i < kKeys; ++i) {
                const std::string n = std::to_string(i);
                if (!s->set(std::string("persist.") + tag + "." + n, n)) return 4;
                if (!s->set(std::string("sys.") + tag + "." + n, n)) return 5;
            }
            return 0;
        };
    };
    fflush(stdout);
    fflush(stderr);
    const pid_t a = fork();
    if (a == 0) _exit(writer("a")());
    const pid_t b = fork();
    if (b == 0) _exit(writer("b")());
    int sa = -1, sb = -1;
    waitpid(a, &sa, 0);
    waitpid(b, &sb, 0);
    EXPECT_TRUE(thimsy_test::exitedWith(sa, 0)) << sa;
    EXPECT_TRUE(thimsy_test::exitedWith(sb, 0)) << sb;
    auto s = open();
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(s->list("persist.a.").size(), static_cast<size_t>(kKeys));
    EXPECT_EQ(s->list("persist.b.").size(), static_cast<size_t>(kKeys));
    EXPECT_EQ(s->list("sys.").size(), static_cast<size_t>(2 * kKeys));
    EXPECT_EQ(s->get("persist.b.39"), "39");
}

TEST_F(StoreTest, SerialsAndWaitChange) {
    EXPECT_EQ(store->serial("sys.s"), 0u);
    ASSERT_TRUE(store->set("sys.s", "1"));
    const uint32_t s1 = store->serial("sys.s");
    EXPECT_GT(s1, 0u);
    ASSERT_TRUE(store->set("sys.s", "1"));  // same value: no change
    EXPECT_EQ(store->serial("sys.s"), s1);
    ASSERT_TRUE(store->set("sys.s", "2"));
    const uint32_t s2 = store->serial("sys.s");
    EXPECT_NE(s2, s1);
    EXPECT_FALSE(store->waitChange("sys.s", s2, 30));
    EXPECT_TRUE(store->waitChange("sys.s", s1, 0));  // already changed
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        store->set("sys.s", "3");
    });
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(store->waitChange("sys.s", s2, 5000));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
    t.join();
    EXPECT_EQ(store->get("sys.s"), "3");
}

TEST_F(StoreTest, ReadThroughWithoutWatching) {
    auto other = open();
    ASSERT_TRUE(other != nullptr);
    const uint32_t before = other->serial("sys.rt");
    ASSERT_TRUE(store->set("sys.rt", "a"));
    ASSERT_TRUE(store->set("persist.rt", "b"));
    EXPECT_EQ(other->get("sys.rt"), "a");
    EXPECT_EQ(other->get("persist.rt"), "b");
    EXPECT_NE(other->serial("sys.rt"), before);
    // waitChange polls the file without the watcher.
    const uint32_t s = other->serial("sys.rt");
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        store->set("sys.rt", "c");
    });
    EXPECT_TRUE(other->waitChange("sys.rt", s, 5000));
    t.join();
    EXPECT_EQ(other->get("sys.rt"), "c");
}

TEST_F(StoreTest, ObserversRunAfterTheStoreAndNeverVeto) {
    std::vector<std::string> seen;
    std::mutex mu;
    store->addObserver("sys.obs.", [&](std::string_view k, std::string_view v) {
        // Called after the value is stored: the store already returns it.
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back(std::string(k) + "=" + std::string(v) + (store->get(k) == v ? " stored" : " NOT stored"));
    });
    store->addObserver("sys.obs.a", [&](std::string_view, std::string_view) { throw std::runtime_error("veto?"); });
    int exact = 0, all = 0, star = 0;
    store->addObserver("sys.obs.a", [&](std::string_view, std::string_view) { ++exact; });
    const int allId = store->addObserver("", [&](std::string_view, std::string_view) { ++all; });
    store->addObserver("persist.obs*", [&](std::string_view, std::string_view) { ++star; });

    EXPECT_TRUE(store->set("sys.obs.a", "1"));  // the throwing observer can't refuse it
    EXPECT_EQ(store->get("sys.obs.a"), "1");
    EXPECT_TRUE(store->set("sys.obs.b", "2"));
    EXPECT_TRUE(store->set("sys.obs.b", "2"));  // unchanged: no call
    EXPECT_TRUE(store->set("sys.obs.b", ""));   // removed: called with ""
    EXPECT_TRUE(store->set("persist.obsx", "p"));
    EXPECT_TRUE(store->set("sys.other", "o"));
    store->removeObserver(allId);
    EXPECT_TRUE(store->set("sys.other", "o2"));

    std::lock_guard<std::mutex> lock(mu);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], "sys.obs.a=1 stored");
    EXPECT_EQ(seen[1], "sys.obs.b=2 stored");
    EXPECT_EQ(seen[2], "sys.obs.b= stored");
    EXPECT_EQ(exact, 1);
    EXPECT_EQ(all, 5);
    EXPECT_EQ(star, 1);
}

TEST_F(StoreTest, WatcherSeamDeliversOtherWritersChanges) {
    auto fake = std::make_unique<thimsy::FakeWatcher>();
    thimsy::FakeWatcher* w = fake.get();
    std::mutex mu;
    std::vector<std::string> seen;
    store->addObserver("", [&](std::string_view k, std::string_view v) {
        std::lock_guard<std::mutex> lock(mu);
        seen.push_back(std::string(k) + "=" + std::string(v));
    });
    ASSERT_TRUE(store->startWatching(std::move(fake)));
    EXPECT_TRUE(store->watching());
    EXPECT_EQ(w->dirs().size(), 3u);

    // Another process's write: the file changes on disk, then the watcher reports it.
    uint32_t s = store->serial("sys.w");
    thimsy_test::spit(roots.runDir + "/sys.w", "x");
    EXPECT_EQ(store->get("sys.w"), "");  // the cache answers while watching
    w->push({roots.runDir, "sys.w", thimsy::Watcher::kWritten});
    EXPECT_TRUE(store->waitChange("sys.w", s, 5000));
    EXPECT_EQ(store->get("sys.w"), "x");

    s = store->serial("sys.w");
    unlink((roots.runDir + "/sys.w").c_str());
    w->push({roots.runDir, "sys.w", thimsy::Watcher::kRemoved});
    EXPECT_TRUE(store->waitChange("sys.w", s, 5000));
    EXPECT_EQ(store->get("sys.w", "gone"), "gone");

    s = store->serial("persist.w");
    auto other = open();
    ASSERT_TRUE(other->set("persist.w", "p"));
    w->push({roots.persistFile.substr(0, roots.persistFile.rfind('/')), "persist.props", thimsy::Watcher::kWritten});
    EXPECT_TRUE(store->waitChange("persist.w", s, 5000));
    EXPECT_EQ(store->get("persist.w"), "p");

    // Temp names and unknown dirs are ignored; an overflow rescans everything.
    w->push({roots.runDir, ".sys.w.tmp.1.2", thimsy::Watcher::kWritten});
    w->push({"/elsewhere", "sys.z", thimsy::Watcher::kWritten});
    thimsy_test::spit(roots.runDir + "/sys.z", "z");
    s = store->serial("sys.z");
    w->push({"", "", thimsy::Watcher::kOverflow});
    EXPECT_TRUE(store->waitChange("sys.z", s, 5000));
    store->stopWatching();
    EXPECT_FALSE(store->watching());

    std::lock_guard<std::mutex> lock(mu);
    const std::vector<std::string> want = {"sys.w=x", "sys.w=", "persist.w=p", "sys.z=z"};
    EXPECT_EQ(seen, want);
}

TEST_F(StoreTest, InotifySeesAnotherProcess) {
    // The child is forked before the watcher thread starts, then released through a pipe.
    int go[2];
    ASSERT_EQ(pipe(go), 0);
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(go[1]);
        char c;
        if (read(go[0], &c, 1) != 1) _exit(2);
        auto s = ConfigStore::open(roots);
        if (!s) _exit(3);
        _exit(s->set("sys.thimsy.nano.first_frame", "1") && s->set("persist.gammaos.x", "y") ? 0 : 4);
    }
    close(go[0]);
    std::atomic<int> calls{0};
    store->addObserver("sys.thimsy.nano.", [&](std::string_view, std::string_view) { ++calls; });
    const uint32_t s1 = store->serial("sys.thimsy.nano.first_frame");
    const uint32_t s2 = store->serial("persist.gammaos.x");
    ASSERT_TRUE(store->startWatching());
    ASSERT_EQ(write(go[1], "g", 1), 1);
    close(go[1]);
    int status = -1;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(thimsy_test::exitedWith(status, 0)) << status;
    EXPECT_TRUE(store->waitChange("sys.thimsy.nano.first_frame", s1, 5000));
    EXPECT_TRUE(store->waitChange("persist.gammaos.x", s2, 5000));
    EXPECT_EQ(store->get("sys.thimsy.nano.first_frame"), "1");
    EXPECT_EQ(store->get("persist.gammaos.x"), "y");
    // Waiters are woken before observers run, so give the observer a moment.
    for (int i = 0; i < 200 && calls.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(calls.load(), 1);
    store->stopWatching();
}

TEST_F(StoreTest, ListIsSortedAndFiltered) {
    store->set("sys.b", "2");
    store->set("sys.a", "1");
    store->set("persist.c", "3");
    store->set("ro.d", "4");
    const auto all = store->list();
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all.begin()->first, "persist.c");
    EXPECT_EQ(store->list("sys.").size(), 2u);
    EXPECT_EQ(store->list("nothing.").size(), 0u);
}

TEST(Secrets, KeysThatLookSecretAreRedacted) {
    for (const char* k : {"persist.gammaos.scraper.ssdevid", "persist.gammaos.scraper.ssdevpw",
                          "persist.gammaos.scraper.sspassword", "persist.thimsy.share.smb_pw",
                          "persist.thimsy.wifi.psk", "persist.x.api_TOKEN", "sys.x.Secret",
                          "persist.x.apikey", "persist.x.credentials"}) {
        SCOPED_TRACE(k);
        EXPECT_TRUE(thimsy::isSecretKey(k));
        EXPECT_EQ(thimsy::redactValue(k, "hunter2"), "<redacted>");
        EXPECT_EQ(thimsy::redactValue(k, ""), "");
    }
    for (const char* k : {"persist.gammaos.volume", "sys.thimsy.nano.first_frame", "ro.product.model",
                          "persist.gammaos.scraper.user_lang"}) {
        SCOPED_TRACE(k);
        EXPECT_FALSE(thimsy::isSecretKey(k));
        EXPECT_EQ(thimsy::redactValue(k, "v"), "v");
    }
}

TEST(Escaping, RoundTrips) {
    for (const char* v : {"", "plain", "a\nb", "back\\slash", "\\n literal", "cr\r", "trailing\\"}) {
        SCOPED_TRACE(v);
        const std::string e = thimsy::escapeValue(v);
        EXPECT_EQ(e.find('\n'), std::string::npos);
        EXPECT_EQ(thimsy::unescapeValue(e), v);
    }
}

TEST(StoreOpen, RefusesEmptyRoots) {
    std::string err;
    EXPECT_TRUE(ConfigStore::open(ConfigStore::Roots{}, &err) == nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(StoreOpen, DefaultRootsFollowTheEnvironment) {
    unsetenv("THIMSY_PERSIST_FILE");
    unsetenv("THIMSY_RUN_DIR");
    EXPECT_EQ(ConfigStore::defaultRoots().persistFile, "/storage/.config/thimsy/persist.props");
    EXPECT_EQ(ConfigStore::defaultRoots().runDir, "/run/thimsy/props");
    setenv("THIMSY_RUN_DIR", "/tmp/x", 1);
    EXPECT_EQ(ConfigStore::defaultRoots().runDir, "/tmp/x");
    unsetenv("THIMSY_RUN_DIR");
}
