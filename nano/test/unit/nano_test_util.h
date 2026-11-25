// Shared helpers for the Nano host tests.
#pragma once

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

namespace nanotest {

// The checked-in fixtures. Soong installs `data` next to the test binary, keeping the path relative
// to the module directory (<bin dir>/test/fixtures); NANO_TEST_FIXTURES overrides it for a binary
// run from somewhere else.
inline std::string fixtureDir() {
    if (const char* env = getenv("NANO_TEST_FIXTURES")) return env;
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return "test/fixtures";
    exe[n] = '\0';
    std::string dir(exe);
    dir.resize(dir.find_last_of('/'));
    return dir + "/test/fixtures";
}

inline std::string fixture(const std::string& rel) { return fixtureDir() + "/" + rel; }

// A fresh, empty directory under $TMPDIR (or /tmp), removed by the caller if it cares.
inline std::string makeTempDir(const char* tag) {
    const char* base = getenv("TMPDIR");
    std::string t = std::string(base && *base ? base : "/tmp") + "/nano_" + tag + "_XXXXXX";
    if (!mkdtemp(&t[0])) return std::string();
    return t;
}

}  // namespace nanotest
