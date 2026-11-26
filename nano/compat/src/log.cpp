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

// liblog's write side (android/log.h) for the Linux and host builds: one line per message line,
// "<uptime ms> <V|D|I|W|E|F> <tag>: <text>", to stderr (or a test's sink) and optionally a ring
// file of two halves. IMPLEMENTATION.md B.2.

#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nanocompat/nanocompat.h>

namespace nanocompat {
namespace {

struct LogState {
    std::atomic<int> minPriority{ANDROID_LOG_VERBOSE};
    std::mutex mu;  // the sink and the file
    std::shared_ptr<LogSink> sink;
    std::string filePath;
    size_t fileMax = 0;
    int fd = -1;
    off_t fileSize = 0;
};

// Never destroyed: threads may log while the process exits.
LogState& logState() {
    static LogState* s = new LogState();
    return *s;
}

char priorityLetter(int prio) {
    switch (prio) {
        case ANDROID_LOG_VERBOSE:
            return 'V';
        case ANDROID_LOG_DEBUG:
            return 'D';
        case ANDROID_LOG_INFO:
            return 'I';
        case ANDROID_LOG_WARN:
            return 'W';
        case ANDROID_LOG_ERROR:
            return 'E';
        case ANDROID_LOG_FATAL:
            return 'F';
        case ANDROID_LOG_SILENT:
            return 'S';
        default:
            return 'D';  // DEFAULT and UNKNOWN log as debug, as logd shows them
    }
}

void writeAll(int fd, const char* data, size_t len) {
    while (len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

// Caller holds mu. Opens the ring file's current half on first use.
void appendToFileLocked(LogState& s, const std::string& text) {
    if (s.filePath.empty()) return;
    if (s.fd < 0) {
        s.fd = ::open(s.filePath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (s.fd < 0) return;
        struct stat st;
        s.fileSize = fstat(s.fd, &st) == 0 ? st.st_size : 0;
    }
    if (s.fileMax > 0 && static_cast<size_t>(s.fileSize) + text.size() > s.fileMax / 2) {
        // This half is full: it becomes the older half, and a new one starts.
        ::close(s.fd);
        const std::string older = s.filePath + ".1";
        ::rename(s.filePath.c_str(), older.c_str());
        s.fd = ::open(s.filePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
        s.fileSize = 0;
        if (s.fd < 0) return;
    }
    writeAll(s.fd, text.data(), text.size());
    s.fileSize += static_cast<off_t>(text.size());
}

}  // namespace

void setLogMinPriority(int priority) { logState().minPriority.store(priority, std::memory_order_relaxed); }

void setLogFile(const std::string& path, size_t maxBytes) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lock(s.mu);
    if (s.fd >= 0) ::close(s.fd);
    s.fd = -1;
    s.fileSize = 0;
    s.filePath = path;
    s.fileMax = maxBytes;
}

void setLogSink(LogSink sink) {
    LogState& s = logState();
    std::lock_guard<std::mutex> lock(s.mu);
    s.sink = sink ? std::make_shared<LogSink>(std::move(sink)) : nullptr;
}

// Writes one message: every line of it gets the prefix; a trailing newline adds no empty line.
void logMessage(int prio, const char* tag, const char* text) {
    LogState& s = logState();
    if (prio < s.minPriority.load(std::memory_order_relaxed)) return;
    if (tag == nullptr) tag = "";
    if (text == nullptr) text = "";

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char prefix[96];
    const long long ms = static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    snprintf(prefix, sizeof prefix, "%lld %c ", ms, priorityLetter(prio));

    std::string out;
    std::string_view rest(text);
    if (!rest.empty() && rest.back() == '\n') rest.remove_suffix(1);
    for (;;) {
        const size_t nl = rest.find('\n');
        out += prefix;
        out += tag;
        out += ": ";
        out.append(rest.data(), nl == std::string_view::npos ? rest.size() : nl);
        out += '\n';
        if (nl == std::string_view::npos) break;
        rest.remove_prefix(nl + 1);
    }

    std::shared_ptr<LogSink> sink;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        sink = s.sink;
        appendToFileLocked(s, out);
        if (!sink) writeAll(STDERR_FILENO, out.data(), out.size());  // one write per message
    }
    if (sink) (*sink)(prio, tag, text);
}

}  // namespace nanocompat

namespace {

int vprintMessage(int prio, const char* tag, const char* fmt, va_list ap) {
    char stackBuf[1024];
    va_list copy;
    va_copy(copy, ap);
    const int n = vsnprintf(stackBuf, sizeof stackBuf, fmt ? fmt : "", copy);
    va_end(copy);
    if (n < 0) return -1;
    if (static_cast<size_t>(n) < sizeof stackBuf) {
        nanocompat::logMessage(prio, tag, stackBuf);
        return n;
    }
    std::string big(static_cast<size_t>(n) + 1, '\0');
    vsnprintf(&big[0], big.size(), fmt ? fmt : "", ap);
    big.resize(static_cast<size_t>(n));
    nanocompat::logMessage(prio, tag, big.c_str());
    return n;
}

}  // namespace

extern "C" int __android_log_write(int prio, const char* tag, const char* text) {
    nanocompat::logMessage(prio, tag, text);
    return 1;
}

extern "C" int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    return vprintMessage(prio, tag, fmt, ap);
}

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vprintMessage(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

extern "C" int __android_log_buf_write(int /*bufID*/, int prio, const char* tag, const char* text) {
    nanocompat::logMessage(prio, tag, text);
    return 1;
}

extern "C" int __android_log_buf_print(int /*bufID*/, int prio, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vprintMessage(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

extern "C" int __android_log_is_loggable(int prio, const char* /*tag*/, int /*default_prio*/) {
    return prio >= nanocompat::logState().minPriority.load(std::memory_order_relaxed) ? 1 : 0;
}

namespace {

[[noreturn]] void fatal(const char* cond, const char* tag, const char* fmt, va_list ap) {
    std::string msg;
    if (cond != nullptr) {
        msg = "Assertion failed: ";
        msg += cond;
    }
    if (fmt != nullptr && *fmt != '\0') {
        char buf[1024];
        vsnprintf(buf, sizeof buf, fmt, ap);
        if (!msg.empty()) msg += ": ";
        msg += buf;
    }
    if (msg.empty()) msg = "fatal error";
    nanocompat::logMessage(ANDROID_LOG_FATAL, tag, msg.c_str());
    abort();
}

}  // namespace

extern "C" void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fatal(cond, tag, fmt, ap);
}

extern "C" void nanocompat_log_fatal(const char* cond, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fatal(cond, tag, fmt, ap);
}
