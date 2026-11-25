// Shared helpers for the Nano fuzz targets: the parsers under test read files, so each input is
// written to a per-process scratch file first.
#pragma once

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

namespace nanofuzz {

inline std::string scratchDir() {
    static const std::string dir = [] {
        const char* base = getenv("TMPDIR");
        std::string t = std::string(base && *base ? base : "/tmp") + "/nano_fuzz_XXXXXX";
        return mkdtemp(&t[0]) ? t : std::string("/tmp");
    }();
    return dir;
}

inline bool writeFile(const std::string& path, const uint8_t* data, size_t size) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < size) {
        const ssize_t n = write(fd, data + done, size - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd);
    return done == size;
}

}  // namespace nanofuzz
