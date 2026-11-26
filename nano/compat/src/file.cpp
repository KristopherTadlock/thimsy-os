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

// libbase's file helpers (android-base/file.h) on plain POSIX, for the Linux and host builds.

#include <android-base/file.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace android {
namespace base {

namespace {

int openRetry(const char* path, int flags, mode_t mode = 0) {
    int fd;
    do {
        fd = ::open(path, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

}  // namespace

bool ReadFdToString(borrowed_fd fd, std::string* content) {
    content->clear();
    // Reserve the file's size when it is a regular file, as libbase does.
    struct stat sb;
    if (fstat(fd.get(), &sb) != -1 && sb.st_size > 0 && S_ISREG(sb.st_mode)) {
        content->reserve(static_cast<size_t>(sb.st_size));
    }
    char buf[BUFSIZ];
    for (;;) {
        const ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return true;
        content->append(buf, static_cast<size_t>(n));
    }
}

bool ReadFileToString(const std::string& path, std::string* content, bool follow_symlinks) {
    const int flags = O_RDONLY | O_CLOEXEC | (follow_symlinks ? 0 : O_NOFOLLOW);
    unique_fd fd(openRetry(path.c_str(), flags));
    if (fd == -1) return false;
    return ReadFdToString(fd, content);
}

bool WriteStringToFd(std::string_view content, borrowed_fd fd) {
    return WriteFully(fd, content.data(), content.size());
}

bool WriteStringToFile(const std::string& content, const std::string& path, bool follow_symlinks) {
    const int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (follow_symlinks ? 0 : O_NOFOLLOW);
    unique_fd fd(openRetry(path.c_str(), flags, 0666));
    if (fd == -1) return false;
    return WriteStringToFd(content, fd);
}

bool WriteStringToFile(const std::string& content, const std::string& path, mode_t mode, uid_t owner,
                       gid_t group, bool follow_symlinks) {
    const int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (follow_symlinks ? 0 : O_NOFOLLOW);
    unique_fd fd(openRetry(path.c_str(), flags, mode));
    if (fd == -1) return false;
    // fchmod too, since open's mode is filtered by the umask and ignored for an existing file.
    if (fchmod(fd.get(), mode) == -1 || fchown(fd.get(), owner, group) == -1 ||
        !WriteStringToFd(content, fd)) {
        const int saved = errno;
        unlink(path.c_str());
        errno = saved;
        return false;
    }
    return true;
}

bool ReadFully(borrowed_fd fd, void* data, size_t byte_count) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t remaining = byte_count;
    while (remaining > 0) {
        const ssize_t n = ::read(fd.get(), p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = ENODATA;
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool ReadFullyAtOffset(borrowed_fd fd, void* data, size_t byte_count, off64_t offset) {
    uint8_t* p = static_cast<uint8_t*>(data);
    while (byte_count > 0) {
        const ssize_t n = ::pread64(fd.get(), p, byte_count, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            errno = ENODATA;
            return false;
        }
        p += n;
        byte_count -= static_cast<size_t>(n);
        offset += n;
    }
    return true;
}

bool WriteFully(borrowed_fd fd, const void* data, size_t byte_count) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t remaining = byte_count;
    while (remaining > 0) {
        const ssize_t n = ::write(fd.get(), p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool RemoveFileIfExists(const std::string& path, std::string* err) {
    struct stat st;
    if (lstat(path.c_str(), &st) == -1) {
        if (errno == ENOENT || errno == ENOTDIR) return true;
        if (err) *err = strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
        if (err) *err = "is not a regular file or symbolic link";
        return false;
    }
    if (unlink(path.c_str()) == -1) {
        if (err) *err = strerror(errno);
        return false;
    }
    return true;
}

bool Readlink(const std::string& path, std::string* result) {
    result->clear();
    std::vector<char> buf(4096);
    for (;;) {
        const ssize_t n = readlink(path.c_str(), buf.data(), buf.size());
        if (n == -1) return false;
        if (static_cast<size_t>(n) < buf.size()) {
            result->assign(buf.data(), static_cast<size_t>(n));
            return true;
        }
        buf.resize(buf.size() * 2);
    }
}

bool Realpath(const std::string& path, std::string* result) {
    result->clear();
    char* resolved = realpath(path.c_str(), nullptr);
    if (resolved == nullptr) return false;
    *result = resolved;
    free(resolved);
    return true;
}

std::string GetExecutablePath() {
    std::string path;
    Readlink("/proc/self/exe", &path);
    return path;
}

std::string GetExecutableDirectory() { return Dirname(GetExecutablePath()); }

std::string Basename(std::string_view path) {
    // POSIX basename: trailing slashes are ignored; "" gives "."; "/" gives "/".
    if (path.empty()) return ".";
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    if (end == 1 && path[0] == '/') return "/";
    const size_t slash = path.rfind('/', end - 1);
    const size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    return std::string(path.substr(start, end - start));
}

std::string Dirname(std::string_view path) {
    // POSIX dirname: "" and "a" give "."; "/" and "/a" give "/"; trailing slashes are ignored.
    if (path.empty()) return ".";
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    const size_t slash = path.rfind('/', end - 1);
    if (slash == std::string_view::npos) return ".";
    size_t dirEnd = slash;
    while (dirEnd > 1 && path[dirEnd - 1] == '/') --dirEnd;
    if (dirEnd == 0) return "/";
    return std::string(path.substr(0, dirEnd));
}

}  // namespace base
}  // namespace android
