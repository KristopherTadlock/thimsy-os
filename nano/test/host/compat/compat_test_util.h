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

// Helpers shared by the compat layer's tests: a scratch directory and a fresh property store.
#pragma once

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nanocompat/nanocompat.h>

namespace compat_test {

inline void removeTree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR* d = opendir(path.c_str())) {
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                removeTree(path + "/" + e->d_name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

// A fresh directory under $TMPDIR (or /tmp), removed with everything in it.
class ScratchDir {
 public:
    ScratchDir() {
        const char* t = getenv("TMPDIR");
        std::string tmpl = std::string(t && *t ? t : "/tmp") + "/nanocompat-test-XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data())) path_ = buf.data();
    }
    ~ScratchDir() {
        if (!path_.empty()) removeTree(path_);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    const std::string& path() const { return path_; }
    std::string operator/(const std::string& rel) const { return path_ + "/" + rel; }

 private:
    std::string path_;
};

inline nanocompat::InitOptions storeOptions(const ScratchDir& dir, bool watch = false) {
    nanocompat::InitOptions o;
    o.persistFile = dir / "persist.props";
    o.runDir = dir / "run";
    o.watch = watch;
    return o;
}

// A test with its own empty property store (the process-wide one is replaced for each test).
class StoreTest : public ::testing::Test {
 protected:
    void SetUp() override {
        std::string err;
        ASSERT_FALSE(dir_.path().empty());
        ASSERT_TRUE(nanocompat::init(storeOptions(dir_, watch_), &err)) << err;
    }
    bool watch_ = false;
    ScratchDir dir_;
};

inline std::string readFile(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
    }
    return out;
}

// Polls cond every millisecond for up to timeoutMs.
template <class Cond>
bool eventually(Cond cond, int timeoutMs = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return cond();
}

}  // namespace compat_test
