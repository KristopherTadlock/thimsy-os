// A scratch directory for one test, removed with everything in it when the test ends.
#pragma once

#include <ftw.h>
#include <stdio.h>

#include <string>

#include "nano_test_util.h"

namespace nanotest {

class TempDir {
public:
    explicit TempDir(const char* tag) : mPath(makeTempDir(tag)) {}
    ~TempDir() {
        if (mPath.empty()) return;
        nftw(mPath.c_str(), [](const char* p, const struct stat*, int, struct FTW*) { return remove(p); },
             16, FTW_DEPTH | FTW_PHYS);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const { return mPath; }
    std::string operator/(const std::string& name) const { return mPath + "/" + name; }

private:
    std::string mPath;
};

}  // namespace nanotest
