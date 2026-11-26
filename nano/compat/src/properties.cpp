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

// The property shims (cutils/properties.h, android-base/properties.h, sys/system_properties.h) on
// one process-wide thimsy::ConfigStore, plus the store's set-up and the property observers of
// nanocompat/nanocompat.h. IMPLEMENTATION.md B.2 and B.3.

#include <android-base/properties.h>
#include <cutils/properties.h>
#include <sys/system_properties.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <android/log.h>
#include <nanocompat/nanocompat.h>
#include <thimsy/config.h>

// A handle to one key, made once and kept for the life of the process.
struct prop_info {
    explicit prop_info(std::string n) : name(std::move(n)) {}
    const std::string name;
    // Sets of this key by this process. Added to the store's serial, so a set of the value the key
    // already has (which the store does not count as a change) still moves the serial, as on Android.
    std::atomic<uint32_t> ownSets{0};
};

namespace nanocompat {
namespace {

constexpr const char* kTag = "nanocompat";

struct Observer {
    int id;
    std::string pattern;
    bool prefix;
    std::shared_ptr<PropertyObserver> fn;
};

struct State {
    // Opening and replacing the store.
    std::mutex storeMu;
    std::atomic<thimsy::ConfigStore*> current{nullptr};
    std::vector<std::unique_ptr<thimsy::ConfigStore>> stores;  // kept until exit: see init()
    bool triedDefault = false;

    std::mutex infoMu;
    std::map<std::string, std::unique_ptr<prop_info>, std::less<>> infos;

    // Every set or change this process sees bumps the global serial and the generation, and wakes
    // the waiters, which then re-read the serial they wait on.
    std::atomic<uint32_t> globalSerial{1};
    std::mutex waitMu;
    std::condition_variable waitCv;
    uint64_t generation = 0;

    std::mutex obsMu;
    std::vector<Observer> observers;
    int nextObserverId = 1;
};

// Never destroyed: Nano's detached threads may still wait on a property while the process exits.
State& state() {
    static State* s = new State();
    return *s;
}

// The key this thread is setting while it is inside the store's set(), so the store's observer can
// tell this process's own write (announced by setProperty itself, even when the value is the same)
// from changes the store finds on the way (another process's persist.* write, say).
thread_local const char* tlOwnSetKey = nullptr;

bool patternMatches(const Observer& o, std::string_view key) {
    if (o.prefix) return key.size() >= o.pattern.size() && key.compare(0, o.pattern.size(), o.pattern) == 0;
    return o.pattern.empty() || key == o.pattern;
}

// Wakes waiters and tells the observers of one set or change.
void announce(std::string_view key, std::string_view value) {
    State& s = state();
    uint32_t next = s.globalSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (next == 0) s.globalSerial.fetch_add(1, std::memory_order_acq_rel);  // 0 means "never seen"
    {
        std::lock_guard<std::mutex> lock(s.waitMu);
        ++s.generation;
    }
    s.waitCv.notify_all();

    std::vector<std::shared_ptr<PropertyObserver>> fns;
    {
        std::lock_guard<std::mutex> lock(s.obsMu);
        for (const Observer& o : s.observers) {
            if (patternMatches(o, key)) fns.push_back(o.fn);
        }
    }
    for (const auto& fn : fns) {
        try {
            (*fn)(key, value);
        } catch (...) {
            // An observer can't veto or break a write.
        }
    }
}

// Opens a store and makes it current. Caller holds storeMu.
thimsy::ConfigStore* openLocked(State& s, const InitOptions& options, std::string* err) {
    thimsy::ConfigStore::Roots roots;
    roots.persistFile = options.persistFile;
    roots.runDir = options.runDir;
    std::unique_ptr<thimsy::ConfigStore> st = thimsy::ConfigStore::open(roots, err);
    if (!st) return nullptr;
    thimsy::ConfigStore* raw = st.get();
    raw->addObserver("", [raw](std::string_view key, std::string_view value) {
        if (tlOwnSetKey != nullptr && key == tlOwnSetKey) return;  // setProperty announces it
        if (state().current.load(std::memory_order_acquire) != raw) return;  // a replaced store
        announce(key, value);
    });
    if (options.watch && !raw->startWatching()) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "property store: can't watch %s for other processes' writes",
                            options.runDir.c_str());
    }
    s.stores.push_back(std::move(st));
    s.current.store(raw, std::memory_order_release);
    return raw;
}

bool validKey(const char* key) { return key != nullptr && thimsy::isValidKey(key); }

prop_info* infoFor(std::string_view name) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.infoMu);
    auto it = s.infos.find(name);
    if (it != s.infos.end()) return it->second.get();
    auto made = std::make_unique<prop_info>(std::string(name));
    prop_info* raw = made.get();
    s.infos.emplace(std::string(name), std::move(made));
    return raw;
}

prop_info* existingInfo(std::string_view name) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.infoMu);
    auto it = s.infos.find(name);
    return it == s.infos.end() ? nullptr : it->second.get();
}

std::string getValue(const char* key) {
    if (!validKey(key)) return std::string();
    thimsy::ConfigStore* st = store();
    return st ? st->get(key) : std::string();
}

uint32_t serialOf(const prop_info* pi) {
    thimsy::ConfigStore* st = store();
    const uint32_t base = st ? st->serial(pi->name) : 0;
    uint32_t serial = base + pi->ownSets.load(std::memory_order_acquire) + 1;
    return serial == 0 ? 1 : serial;
}

using Clock = std::chrono::steady_clock;

// Waits until read() returns something other than old (true, *out set) or the deadline passes.
// poll is how often a value only visible by reading (another process's write, while the store is
// not watching) is re-read.
template <class Read>
bool waitUntilChanged(Read read, uint32_t old, uint32_t* out, bool hasDeadline,
                      Clock::time_point deadline, std::chrono::milliseconds poll) {
    State& s = state();
    for (;;) {
        uint64_t gen;
        {
            std::lock_guard<std::mutex> lock(s.waitMu);
            gen = s.generation;
        }
        const uint32_t cur = read();
        if (cur != old) {
            if (out) *out = cur;
            return true;
        }
        auto step = poll;
        if (hasDeadline) {
            const auto now = Clock::now();
            if (now >= deadline) return false;
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1);
            step = std::min(step, left);
        }
        std::unique_lock<std::mutex> lock(s.waitMu);
        s.waitCv.wait_for(lock, step, [&] { return s.generation != gen; });
    }
}

std::chrono::milliseconds pollInterval() {
    thimsy::ConfigStore* st = store();
    return std::chrono::milliseconds(st && st->watching() ? 1000 : 20);
}

int setProperty(const char* key, const char* value) {
    if (key == nullptr) return -1;
    if (value == nullptr) value = "";
    thimsy::ConfigStore* st = store();
    if (st == nullptr) return -1;
    const char* const outer = tlOwnSetKey;
    tlOwnSetKey = key;
    const thimsy::ConfigStore::SetResult r = st->trySet(key, value);
    tlOwnSetKey = outer;
    if (r != thimsy::ConfigStore::SetResult::Ok) {
        if (r == thimsy::ConfigStore::SetResult::IoError) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "property_set %s: %s", key, thimsy::toString(r));
        }
        return -1;
    }
    infoFor(key)->ownSets.fetch_add(1, std::memory_order_acq_rel);
    announce(key, value);
    return 0;
}

intmax_t getImax(const char* key, intmax_t lower, intmax_t upper, intmax_t def) {
    if (key == nullptr) return def;
    char buf[PROPERTY_VALUE_MAX] = {};
    const int len = property_get(key, buf, "");
    if (len <= 0) return def;
    const int savedErrno = errno;
    errno = 0;
    char* end = nullptr;
    intmax_t result = strtoimax(buf, &end, 0);
    if ((result == INTMAX_MIN || result == INTMAX_MAX) && errno == ERANGE) {
        result = def;  // overflow
    } else if (result < lower || result > upper) {
        result = def;  // outside the type
    } else if (end == buf) {
        result = def;  // no digits
    }
    errno = savedErrno;
    return result;
}

}  // namespace

bool init(const InitOptions& options, std::string* err) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.storeMu);
    // The replaced store stays allocated (in stores) until exit, so a thread that loaded it just
    // before the switch is safe; its observer stops announcing once it is no longer current.
    return openLocked(s, options, err) != nullptr;
}

thimsy::ConfigStore* store() {
    State& s = state();
    if (thimsy::ConfigStore* p = s.current.load(std::memory_order_acquire)) return p;
    std::lock_guard<std::mutex> lock(s.storeMu);
    if (thimsy::ConfigStore* p = s.current.load(std::memory_order_acquire)) return p;
    if (s.triedDefault) return nullptr;
    s.triedDefault = true;
    InitOptions o;
    o.watch = false;
    const char* root = getenv("THIMSY_PROP_ROOT");
    if (root != nullptr && *root != '\0') {
        o.persistFile = std::string(root) + "/persist.props";
        o.runDir = std::string(root) + "/run";
    } else {
        const thimsy::ConfigStore::Roots r = thimsy::ConfigStore::defaultRoots();
        o.persistFile = r.persistFile;
        o.runDir = r.runDir;
    }
    std::string err;
    if (openLocked(s, o, &err) == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "no property store (%s, %s): %s; every property reads as its default",
                            o.persistFile.c_str(), o.runDir.c_str(), err.c_str());
    }
    return s.current.load(std::memory_order_acquire);
}

int addPropertyObserver(std::string_view keyOrPrefix, PropertyObserver observer) {
    State& s = state();
    Observer o;
    o.pattern = std::string(keyOrPrefix);
    o.prefix = false;
    if (!o.pattern.empty() && o.pattern.back() == '*') {
        o.pattern.pop_back();
        o.prefix = true;
    } else if (!o.pattern.empty() && o.pattern.back() == '.') {
        o.prefix = true;
    }
    o.fn = std::make_shared<PropertyObserver>(std::move(observer));
    std::lock_guard<std::mutex> lock(s.obsMu);
    o.id = s.nextObserverId++;
    s.observers.push_back(std::move(o));
    return s.observers.back().id;
}

void removePropertyObserver(int id) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.obsMu);
    s.observers.erase(std::remove_if(s.observers.begin(), s.observers.end(),
                                     [id](const Observer& o) { return o.id == id; }),
                      s.observers.end());
}

}  // namespace nanocompat

using nanocompat::Clock;

// ---- sys/system_properties.h ----

extern "C" int __system_property_set(const char* name, const char* value) {
    return nanocompat::setProperty(name, value);
}

extern "C" const prop_info* __system_property_find(const char* name) {
    if (!nanocompat::validKey(name)) return nullptr;
    if (prop_info* pi = nanocompat::existingInfo(name)) return pi;
    if (nanocompat::getValue(name).empty()) return nullptr;
    return nanocompat::infoFor(name);
}

extern "C" int __system_property_get(const char* name, char* value) {
    if (value == nullptr) return 0;
    value[0] = '\0';
    const std::string v = nanocompat::getValue(name);
    const size_t n = std::min(v.size(), static_cast<size_t>(PROP_VALUE_MAX - 1));
    memcpy(value, v.data(), n);
    value[n] = '\0';
    return static_cast<int>(n);
}

extern "C" void __system_property_read_callback(const prop_info* pi,
                                                void (*callback)(void* cookie, const char* name,
                                                                 const char* value, uint32_t serial),
                                                void* cookie) {
    if (pi == nullptr || callback == nullptr) return;
    const uint32_t serial = nanocompat::serialOf(pi);
    const std::string v = nanocompat::getValue(pi->name.c_str());
    callback(cookie, pi->name.c_str(), v.c_str(), serial);
}

extern "C" int __system_property_foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie) {
    if (propfn == nullptr) return -1;
    thimsy::ConfigStore* st = nanocompat::store();
    if (st == nullptr) return 0;
    for (const auto& kv : st->list()) propfn(nanocompat::infoFor(kv.first), cookie);
    return 0;
}

extern "C" uint32_t __system_property_serial(const prop_info* pi) {
    return pi == nullptr ? 0 : nanocompat::serialOf(pi);
}

extern "C" uint32_t __system_property_area_serial(void) {
    return nanocompat::state().globalSerial.load(std::memory_order_acquire);
}

extern "C" bool __system_property_wait(const prop_info* pi, uint32_t old_serial, uint32_t* new_serial_ptr,
                                       const struct timespec* relative_timeout) {
    const bool hasDeadline = relative_timeout != nullptr;
    Clock::time_point deadline;
    if (hasDeadline) {
        if (relative_timeout->tv_sec < 0 || relative_timeout->tv_nsec < 0 ||
            relative_timeout->tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return false;
        }
        deadline = Clock::now() + std::chrono::seconds(relative_timeout->tv_sec) +
                   std::chrono::nanoseconds(relative_timeout->tv_nsec);
    }
    if (pi != nullptr) {
        return nanocompat::waitUntilChanged([pi] { return nanocompat::serialOf(pi); }, old_serial,
                                            new_serial_ptr, hasDeadline, deadline, nanocompat::pollInterval());
    }
    // The global serial only moves on what this process sees, so there is nothing to poll.
    return nanocompat::waitUntilChanged([] { return __system_property_area_serial(); }, old_serial,
                                        new_serial_ptr, hasDeadline, deadline, std::chrono::milliseconds(1000));
}

// ---- cutils/properties.h ----

extern "C" int property_get(const char* key, char* value, const char* default_value) {
    if (value == nullptr) return -1;
    int len = __system_property_get(key, value);
    if (len < 1 && default_value != nullptr) {
        snprintf(value, PROPERTY_VALUE_MAX, "%s", default_value);
        return static_cast<int>(strlen(value));
    }
    return len;
}

extern "C" int property_set(const char* key, const char* value) {
    return __system_property_set(key, value);
}

extern "C" int8_t property_get_bool(const char* key, int8_t default_value) {
    if (key == nullptr) return default_value;
    int8_t result = default_value;
    char buf[PROPERTY_VALUE_MAX] = {};
    const int len = property_get(key, buf, "");
    if (len == 1) {
        const char ch = buf[0];
        if (ch == '0' || ch == 'n') {
            result = false;
        } else if (ch == '1' || ch == 'y') {
            result = true;
        }
    } else if (len > 1) {
        if (!strcmp(buf, "no") || !strcmp(buf, "false") || !strcmp(buf, "off")) {
            result = false;
        } else if (!strcmp(buf, "yes") || !strcmp(buf, "true") || !strcmp(buf, "on")) {
            result = true;
        }
    }
    return result;
}

extern "C" int64_t property_get_int64(const char* key, int64_t default_value) {
    return static_cast<int64_t>(nanocompat::getImax(key, INT64_MIN, INT64_MAX, default_value));
}

extern "C" int32_t property_get_int32(const char* key, int32_t default_value) {
    return static_cast<int32_t>(nanocompat::getImax(key, INT32_MIN, INT32_MAX, default_value));
}

extern "C" int property_list(void (*propfn)(const char* key, const char* value, void* cookie), void* cookie) {
    if (propfn == nullptr) return -1;
    thimsy::ConfigStore* st = nanocompat::store();
    if (st == nullptr) return 0;
    for (const auto& kv : st->list()) propfn(kv.first.c_str(), kv.second.c_str(), cookie);
    return 0;
}

// ---- android-base/properties.h ----

namespace android {
namespace base {

std::string GetProperty(const std::string& key, const std::string& default_value) {
    std::string v = nanocompat::getValue(key.c_str());
    return v.empty() ? default_value : v;
}

bool GetBoolProperty(const std::string& key, bool default_value) {
    switch (ParseBool(GetProperty(key, ""))) {
        case ParseBoolResult::kTrue:
            return true;
        case ParseBoolResult::kFalse:
            return false;
        case ParseBoolResult::kError:
            break;
    }
    return default_value;
}

bool SetProperty(const std::string& key, const std::string& value) {
    if (key.find('\0') != std::string::npos) return false;
    return __system_property_set(key.c_str(), value.c_str()) == 0;
}

namespace {

// The deadline for a relative timeout; none for milliseconds::max() (or anything that overflows).
bool deadlineFor(std::chrono::milliseconds timeout, Clock::time_point* deadline) {
    if (timeout == std::chrono::milliseconds::max()) return false;
    const auto now = Clock::now();
    if (timeout.count() < 0) timeout = std::chrono::milliseconds(0);
    if (timeout > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now)) return false;
    *deadline = now + timeout;
    return true;
}

// Waits until the key has a value; its handle, or nullptr after the deadline.
const prop_info* waitCreation(const std::string& key, bool hasDeadline, Clock::time_point deadline) {
    for (;;) {
        const uint32_t serial = __system_property_area_serial();
        if (const prop_info* pi = __system_property_find(key.c_str())) return pi;
        if (hasDeadline && Clock::now() >= deadline) return nullptr;
        // A creation by another process is only seen by reading, unless the store watches.
        nanocompat::waitUntilChanged([] { return __system_property_area_serial(); }, serial, nullptr,
                                     true,
                                     hasDeadline ? std::min(deadline, Clock::now() + nanocompat::pollInterval())
                                                 : Clock::now() + nanocompat::pollInterval(),
                                     nanocompat::pollInterval());
    }
}

}  // namespace

bool WaitForProperty(const std::string& key, const std::string& expected_value,
                     std::chrono::milliseconds relative_timeout) {
    Clock::time_point deadline;
    const bool hasDeadline = deadlineFor(relative_timeout, &deadline);
    const prop_info* pi = waitCreation(key, hasDeadline, deadline);
    if (pi == nullptr) return false;
    for (;;) {
        const uint32_t serial = __system_property_serial(pi);
        if (nanocompat::getValue(key.c_str()) == expected_value) return true;
        if (hasDeadline && Clock::now() >= deadline) return false;
        nanocompat::waitUntilChanged([pi] { return nanocompat::serialOf(pi); }, serial, nullptr, hasDeadline,
                                     deadline, nanocompat::pollInterval());
    }
}

bool WaitForPropertyCreation(const std::string& key, std::chrono::milliseconds relative_timeout) {
    Clock::time_point deadline;
    const bool hasDeadline = deadlineFor(relative_timeout, &deadline);
    return waitCreation(key, hasDeadline, deadline) != nullptr;
}

}  // namespace base
}  // namespace android
