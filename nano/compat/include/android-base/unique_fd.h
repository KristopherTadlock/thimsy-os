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

// Thimsy's <android-base/unique_fd.h> for the Linux and host builds of Nano: libbase's owning file
// descriptor (closes on destruction and reset) and borrowed_fd.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <utility>

namespace android {
namespace base {

struct DefaultCloser {
    static void Close(int fd) {
        // Keep errno: a destructor may run between a failing call and the caller reading errno.
        const int saved = errno;
        ::close(fd);
        errno = saved;
    }
};

template <typename Closer>
class unique_fd_impl final {
 public:
    unique_fd_impl() {}
    explicit unique_fd_impl(int fd) { reset(fd); }
    ~unique_fd_impl() { reset(); }

    unique_fd_impl(const unique_fd_impl&) = delete;
    unique_fd_impl& operator=(const unique_fd_impl&) = delete;
    unique_fd_impl(unique_fd_impl&& other) noexcept { reset(other.release()); }
    unique_fd_impl& operator=(unique_fd_impl&& s) noexcept {
        int fd = s.fd_;
        s.fd_ = -1;
        reset(fd);
        return *this;
    }

    void reset(int new_value = -1) {
        if (fd_ != -1 && fd_ != new_value) Closer::Close(fd_);
        fd_ = new_value;
    }

    int get() const { return fd_; }
    // Implicit, as libbase has it unless ANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION.
#if !defined(ANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION)
    operator int() const { return get(); }  // NOLINT(google-explicit-constructor)
#endif
    bool operator>=(int rhs) const { return get() >= rhs; }
    bool operator<(int rhs) const { return get() < rhs; }
    bool operator==(int rhs) const { return get() == rhs; }
    bool operator!=(int rhs) const { return get() != rhs; }
    bool operator!() const = delete;
    bool ok() const { return get() >= 0; }

    // Gives up ownership without closing.
    int release() __attribute__((warn_unused_result)) {
        int ret = fd_;
        fd_ = -1;
        return ret;
    }

 private:
    int fd_ = -1;
};

using unique_fd = unique_fd_impl<DefaultCloser>;

// A non-owning file descriptor, accepted wherever libbase takes one.
class borrowed_fd {
 public:
    borrowed_fd(int fd) : fd_(fd) {}  // NOLINT(google-explicit-constructor)
    template <typename T>
    borrowed_fd(const unique_fd_impl<T>& ufd) : fd_(ufd.get()) {}  // NOLINT
    int get() const { return fd_; }
    bool operator>=(int rhs) const { return get() >= rhs; }
    bool operator<(int rhs) const { return get() < rhs; }
    bool operator==(int rhs) const { return get() == rhs; }
    bool operator!=(int rhs) const { return get() != rhs; }

 private:
    int fd_ = -1;
};

// A pipe whose two ends are owned. flags: O_CLOEXEC by default (pipe2).
template <typename Closer>
inline bool Pipe(unique_fd_impl<Closer>* read, unique_fd_impl<Closer>* write, int flags = O_CLOEXEC) {
    int pipefd[2];
    if (pipe2(pipefd, flags) != 0) return false;
    read->reset(pipefd[0]);
    write->reset(pipefd[1]);
    return true;
}

}  // namespace base
}  // namespace android
