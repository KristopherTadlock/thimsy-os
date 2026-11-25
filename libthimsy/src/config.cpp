// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "thimsy/config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <utility>

#include "thimsy/atomic_file.h"
#include "thimsy/watch.h"

namespace thimsy {

namespace {

bool startsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool isRo(std::string_view key) { return startsWith(key, "ro."); }
bool isPersist(std::string_view key) { return startsWith(key, "persist."); }

std::string dirOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string baseOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Reads one key's file; empty when it does not exist.
std::string readValueFile(const std::string& path) {
    std::string v;
    if (!readFile(path, &v)) return {};
    return v;
}

using Map = std::map<std::string, std::string, std::less<>>;

Map parsePersist(const std::string& text) {
    Map m;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq);
        if (!isValidKey(key) || !isPersist(key)) continue;
        std::string value = unescapeValue(line.substr(eq + 1));
        if (!value.empty()) m[std::string(key)] = std::move(value);
    }
    return m;
}

std::string formatPersist(const Map& m) {
    std::string out = "# Thimsy persistent settings (persist.*), written by libthimsy. Use thimsy-prop to change them.\n";
    for (const auto& kv : m) {
        out += kv.first;
        out += '=';
        out += escapeValue(kv.second);
        out += '\n';
    }
    return out;
}

struct Stamp {
    bool exists = false;
    dev_t dev = 0;
    ino_t ino = 0;
    off_t size = 0;
    long mtimeSec = 0;
    long mtimeNsec = 0;
    bool operator==(const Stamp& o) const {
        return exists == o.exists && dev == o.dev && ino == o.ino && size == o.size &&
               mtimeSec == o.mtimeSec && mtimeNsec == o.mtimeNsec;
    }
};

Stamp stampOf(const std::string& path) {
    Stamp s;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return s;
    s.exists = true;
    s.dev = st.st_dev;
    s.ino = st.st_ino;
    s.size = st.st_size;
    s.mtimeSec = static_cast<long>(st.st_mtim.tv_sec);
    s.mtimeNsec = st.st_mtim.tv_nsec;
    return s;
}

using Change = std::pair<std::string, std::string>;

}  // namespace

// ---- Free functions ----------------------------------------------------------------------------

bool isValidKey(std::string_view key) {
    if (key.empty() || key.size() > 255) return false;
    if (key.front() == '.' || key.back() == '.') return false;
    char prev = 0;
    for (char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-' || c == '@' || c == ':';
        if (!ok) return false;
        if (c == '.' && prev == '.') return false;
        prev = c;
    }
    return true;
}

bool isSecretKey(std::string_view key) {
    std::string lower(key);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    static const char* const kWords[] = {"pw", "pass", "token", "secret", "ssdev", "psk", "apikey", "credential"};
    for (const char* w : kWords) {
        if (lower.find(w) != std::string::npos) return true;
    }
    return false;
}

std::string redactValue(std::string_view key, std::string_view value) {
    if (!value.empty() && isSecretKey(key)) return "<redacted>";
    return std::string(value);
}

std::string escapeValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string unescapeValue(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            c = text[++i];
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
        }
        out += c;
    }
    return out;
}

const char* toString(ConfigStore::SetResult r) {
    switch (r) {
        case ConfigStore::SetResult::Ok: return "ok";
        case ConfigStore::SetResult::InvalidKey: return "invalid key";
        case ConfigStore::SetResult::ValueTooLong: return "value longer than 91 bytes";
        case ConfigStore::SetResult::InvalidValue: return "value contains a NUL byte";
        case ConfigStore::SetResult::ReadOnly: return "read-only key";
        case ConfigStore::SetResult::IoError: return "I/O error";
    }
    return "?";
}

// ---- Impl --------------------------------------------------------------------------------------

struct ConfigStore::Impl {
    Roots roots;
    std::string persistDir, persistBase, lockPath, roDir, sealPath;

    mutable std::shared_mutex mu;  // cache, serials, persistStamp
    mutable Map cache;             // every key with a non-empty value
    mutable std::map<std::string, uint32_t, std::less<>> serials;
    mutable Stamp persistStamp;

    mutable std::mutex waitMu;
    mutable std::condition_variable waitCv;

    std::mutex persistWriteMu;

    struct Obs {
        int id;
        std::string pattern;
        bool prefix;
        Observer fn;
    };
    std::mutex obsMu;
    std::vector<Obs> observers;
    int nextObsId = 1;

    std::atomic<bool> isWatching{false};
    std::atomic<bool> stopFlag{false};
    std::unique_ptr<Watcher> watcher;
    std::thread thread;
    std::mutex watchCtlMu;

    std::string runPath(std::string_view key) const {
        return (isRo(key) ? roDir : roots.runDir) + "/" + std::string(key);
    }

    // Records a value seen for key (empty: removed). Caller holds mu exclusively.
    void applyLocked(const std::string& key, const std::string& value, std::vector<Change>* changes) const {
        auto it = cache.find(key);
        const bool had = it != cache.end();
        if (value.empty()) {
            if (!had) return;
            cache.erase(it);
        } else {
            if (had && it->second == value) return;
            cache[key] = value;
        }
        ++serials[key];
        if (changes) changes->emplace_back(key, value);
    }

    // Applies the persist file's whole content. Caller holds mu exclusively.
    void applyPersistLocked(const Map& m, std::vector<Change>* changes) const {
        std::vector<std::string> gone;
        for (auto it = cache.lower_bound("persist."); it != cache.end() && isPersist(it->first); ++it) {
            if (m.find(it->first) == m.end()) gone.push_back(it->first);
        }
        for (const auto& k : gone) applyLocked(k, "", changes);
        for (const auto& kv : m) applyLocked(kv.first, kv.second, changes);
    }

    // Re-reads the persist file if it changed since the last read. Caller holds mu exclusively.
    void refreshPersistLocked(std::vector<Change>* changes) const {
        const Stamp now = stampOf(roots.persistFile);
        if (now == persistStamp) return;
        std::string text;
        if (now.exists && !readFile(roots.persistFile, &text)) return;  // try again next time
        persistStamp = now;
        applyPersistLocked(parsePersist(text), changes);
    }

    // Re-reads one run or ro key from disk. Caller holds mu exclusively.
    void refreshKeyLocked(std::string_view key, std::vector<Change>* changes) const {
        if (isPersist(key)) {
            refreshPersistLocked(changes);
            return;
        }
        applyLocked(std::string(key), readValueFile(runPath(key)), changes);
    }

    // Scans one directory's key files (keys accepted by `want`) into *out.
    template <class Want>
    static void scanDir(const std::string& dir, Want want, Map* out) {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (struct dirent* ent = readdir(d)) {
            const std::string name = ent->d_name;
            if (!isValidKey(name) || !want(name)) continue;
            struct stat st;
            if (fstatat(dirfd(d), ent->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) continue;
            std::string v = readValueFile(dir + "/" + name);
            if (!v.empty()) (*out)[name] = std::move(v);
        }
        closedir(d);
    }

    // Re-reads everything. Caller holds mu exclusively.
    void rescanLocked(std::vector<Change>* changes) const {
        refreshPersistLocked(changes);
        Map disk;
        scanDir(roots.runDir, [](const std::string& k) { return !isRo(k) && !isPersist(k); }, &disk);
        scanDir(roDir, [](const std::string& k) { return isRo(k); }, &disk);
        std::vector<std::string> gone;
        for (const auto& kv : cache) {
            if (!isPersist(kv.first) && disk.find(kv.first) == disk.end()) gone.push_back(kv.first);
        }
        for (const auto& k : gone) applyLocked(k, "", changes);
        for (const auto& kv : disk) applyLocked(kv.first, kv.second, changes);
    }

    // Wakes waitChange and calls observers. Call without mu held.
    void publish(const std::vector<Change>& changes) {
        if (changes.empty()) return;
        {
            std::lock_guard<std::mutex> lock(waitMu);
            waitCv.notify_all();
        }
        std::vector<Obs> obs;
        {
            std::lock_guard<std::mutex> lock(obsMu);
            obs = observers;
        }
        if (obs.empty()) return;
        for (const Change& c : changes) {
            for (const Obs& o : obs) {
                const bool match = o.pattern.empty() || (o.prefix ? startsWith(c.first, o.pattern) : c.first == o.pattern);
                if (!match) continue;
                try {
                    o.fn(c.first, c.second);
                } catch (...) {
                    // An observer can't veto or undo a stored value.
                }
            }
        }
    }

    bool sealed() const { return ::access(sealPath.c_str(), F_OK) == 0; }

    SetResult writePersist(std::string_view key, std::string_view value, std::vector<Change>* changes) {
        std::lock_guard<std::mutex> plock(persistWriteMu);
        const int lfd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lfd < 0) return SetResult::IoError;
        while (flock(lfd, LOCK_EX) != 0) {
            if (errno != EINTR) {
                ::close(lfd);
                return SetResult::IoError;
            }
        }
        removeStaleTemps(roots.persistFile);
        std::string text;
        Map m;
        if (readFile(roots.persistFile, &text)) {
            m = parsePersist(text);
        } else if (errno != ENOENT) {
            ::close(lfd);
            return SetResult::IoError;
        }
        if (value.empty()) {
            auto it = m.find(key);
            if (it != m.end()) m.erase(it);
        } else {
            m[std::string(key)] = std::string(value);
        }
        AtomicWriteOptions opts;
        opts.mode = 0600;
        opts.durable = true;
        const bool ok = writeFileAtomic(roots.persistFile, formatPersist(m), opts);
        if (ok) {
            std::unique_lock<std::shared_mutex> lock(mu);
            persistStamp = stampOf(roots.persistFile);
            applyPersistLocked(m, changes);
        }
        ::close(lfd);  // releases the flock
        return ok ? SetResult::Ok : SetResult::IoError;
    }

    void watchLoop() {
        std::vector<Watcher::Event> events;
        while (!stopFlag.load()) {
            events.clear();
            if (!watcher->wait(&events, 1000)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (stopFlag.load()) break;
            if (events.empty()) continue;
            std::vector<Change> changes;
            {
                std::unique_lock<std::shared_mutex> lock(mu);
                for (const auto& e : events) {
                    if (e.what & Watcher::kOverflow) {
                        rescanLocked(&changes);
                    } else if (e.dir == persistDir && e.name == persistBase) {
                        refreshPersistLocked(&changes);
                    } else if (e.dir == roots.runDir) {
                        if (isValidKey(e.name) && !isRo(e.name) && !isPersist(e.name)) refreshKeyLocked(e.name, &changes);
                    } else if (e.dir == roDir) {
                        if (isValidKey(e.name) && isRo(e.name)) refreshKeyLocked(e.name, &changes);
                    }
                }
            }
            publish(changes);
        }
    }
};

// ---- ConfigStore -------------------------------------------------------------------------------

ConfigStore::Roots ConfigStore::defaultRoots() {
    Roots r;
    const char* p = getenv("THIMSY_PERSIST_FILE");
    const char* d = getenv("THIMSY_RUN_DIR");
    r.persistFile = p && *p ? p : "/storage/.config/thimsy/persist.props";
    r.runDir = d && *d ? d : "/run/thimsy/props";
    return r;
}

ConfigStore::ConfigStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ConfigStore::~ConfigStore() { stopWatching(); }

std::unique_ptr<ConfigStore> ConfigStore::open(const Roots& roots, std::string* err) {
    if (roots.persistFile.empty() || roots.runDir.empty()) {
        if (err) *err = "empty root";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->roots = roots;
    while (impl->roots.runDir.size() > 1 && impl->roots.runDir.back() == '/') impl->roots.runDir.pop_back();
    impl->persistDir = dirOf(roots.persistFile);
    impl->persistBase = baseOf(roots.persistFile);
    impl->lockPath = impl->persistDir + "/persist.lock";
    impl->roDir = impl->roots.runDir + "/ro";
    impl->sealPath = impl->roDir + "/.sealed";
    if (impl->persistBase.empty()) {
        if (err) *err = "no file name in " + roots.persistFile;
        return nullptr;
    }
    if (!makeDirs(impl->persistDir, 0700)) {
        if (err) *err = "can't create " + impl->persistDir + ": " + strerror(errno);
        return nullptr;
    }
    if (!makeDirs(impl->roDir, 0755)) {
        if (err) *err = "can't create " + impl->roDir + ": " + strerror(errno);
        return nullptr;
    }
    {
        std::unique_lock<std::shared_mutex> lock(impl->mu);
        impl->rescanLocked(nullptr);
    }
    return std::unique_ptr<ConfigStore>(new ConfigStore(std::move(impl)));
}

const ConfigStore::Roots& ConfigStore::roots() const { return impl_->roots; }

std::string ConfigStore::get(std::string_view key, std::string_view def) const {
    if (!isValidKey(key)) return std::string(def);
    if (impl_->isWatching.load()) {
        std::shared_lock<std::shared_mutex> lock(impl_->mu);
        auto it = impl_->cache.find(key);
        return it == impl_->cache.end() ? std::string(def) : it->second;
    }
    std::unique_lock<std::shared_mutex> lock(impl_->mu);
    impl_->refreshKeyLocked(key, nullptr);  // read-through: serials bump, observers are not called
    auto it = impl_->cache.find(key);
    return it == impl_->cache.end() ? std::string(def) : it->second;
}

std::map<std::string, std::string> ConfigStore::list(std::string_view prefix) const {
    std::map<std::string, std::string> out;
    if (!impl_->isWatching.load()) {
        std::unique_lock<std::shared_mutex> lock(impl_->mu);
        impl_->rescanLocked(nullptr);
    }
    std::shared_lock<std::shared_mutex> lock(impl_->mu);
    for (auto it = impl_->cache.lower_bound(prefix); it != impl_->cache.end() && startsWith(it->first, prefix); ++it) {
        out.emplace(it->first, it->second);
    }
    return out;
}

ConfigStore::SetResult ConfigStore::trySet(std::string_view key, std::string_view value) {
    if (!isValidKey(key)) return SetResult::InvalidKey;
    if (value.find('\0') != std::string_view::npos) return SetResult::InvalidValue;
    const bool ro = isRo(key);
    if (!ro && value.size() > kMaxValueLength) return SetResult::ValueTooLong;
    std::vector<Change> changes;
    if (ro) {
        if (impl_->sealed()) return SetResult::ReadOnly;
        const std::string path = impl_->runPath(key);
        if (!readValueFile(path).empty()) return SetResult::ReadOnly;
        if (value.empty()) return SetResult::Ok;
        AtomicWriteOptions opts;
        opts.mode = isSecretKey(key) ? 0600 : 0644;
        opts.durable = false;
        if (!writeFileAtomic(path, value, opts)) return SetResult::IoError;
        std::unique_lock<std::shared_mutex> lock(impl_->mu);
        impl_->applyLocked(std::string(key), std::string(value), &changes);
    } else if (isPersist(key)) {
        const SetResult r = impl_->writePersist(key, value, &changes);
        if (r != SetResult::Ok) return r;
    } else {
        const std::string path = impl_->runPath(key);
        if (value.empty()) {
            if (::unlink(path.c_str()) != 0 && errno != ENOENT) return SetResult::IoError;
        } else {
            AtomicWriteOptions opts;
            opts.mode = isSecretKey(key) ? 0600 : 0644;
            opts.durable = false;  // tmpfs on a device
            if (!writeFileAtomic(path, value, opts)) return SetResult::IoError;
        }
        std::unique_lock<std::shared_mutex> lock(impl_->mu);
        impl_->applyLocked(std::string(key), std::string(value), &changes);
    }
    impl_->publish(changes);
    return SetResult::Ok;
}

uint32_t ConfigStore::serial(std::string_view key) const {
    if (!impl_->isWatching.load() && isValidKey(key)) {
        std::unique_lock<std::shared_mutex> lock(impl_->mu);
        impl_->refreshKeyLocked(key, nullptr);
    }
    std::shared_lock<std::shared_mutex> lock(impl_->mu);
    auto it = impl_->serials.find(key);
    return it == impl_->serials.end() ? 0 : it->second;
}

bool ConfigStore::waitChange(std::string_view key, uint32_t oldSerial, int timeoutMs) const {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    std::unique_lock<std::mutex> lock(impl_->waitMu);
    for (;;) {
        if (serial(key) != oldSerial) return true;
        // Without the watcher, other processes' writes are only seen by polling the file.
        auto step = std::chrono::milliseconds(impl_->isWatching.load() ? 1000 : 20);
        if (timeoutMs >= 0) {
            const auto now = Clock::now();
            if (now >= deadline) return false;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) + std::chrono::milliseconds(1);
            step = std::min(step, left);
        }
        impl_->waitCv.wait_for(lock, step);
    }
}

int ConfigStore::addObserver(std::string_view keyOrPrefix, Observer observer) {
    Impl::Obs o;
    o.pattern = std::string(keyOrPrefix);
    o.prefix = false;
    if (!o.pattern.empty() && o.pattern.back() == '*') {
        o.pattern.pop_back();
        o.prefix = true;
    } else if (!o.pattern.empty() && o.pattern.back() == '.') {
        o.prefix = true;
    }
    o.fn = std::move(observer);
    std::lock_guard<std::mutex> lock(impl_->obsMu);
    o.id = impl_->nextObsId++;
    impl_->observers.push_back(std::move(o));
    return impl_->observers.back().id;
}

void ConfigStore::removeObserver(int id) {
    std::lock_guard<std::mutex> lock(impl_->obsMu);
    auto& v = impl_->observers;
    v.erase(std::remove_if(v.begin(), v.end(), [id](const Impl::Obs& o) { return o.id == id; }), v.end());
}

bool ConfigStore::startWatching() { return startWatching(Watcher::createInotify()); }

bool ConfigStore::startWatching(std::unique_ptr<Watcher> watcher) {
    std::lock_guard<std::mutex> ctl(impl_->watchCtlMu);
    if (impl_->isWatching.load()) return true;
    if (!watcher) return false;
    if (!watcher->addDir(impl_->roots.runDir) || !watcher->addDir(impl_->roDir) ||
        !watcher->addDir(impl_->persistDir)) {
        return false;
    }
    std::vector<Change> changes;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->mu);
        impl_->rescanLocked(&changes);  // whatever changed before the watches existed
    }
    impl_->watcher = std::move(watcher);
    impl_->stopFlag.store(false);
    impl_->isWatching.store(true);
    impl_->thread = std::thread([this] { impl_->watchLoop(); });
    impl_->publish(changes);
    return true;
}

void ConfigStore::stopWatching() {
    std::lock_guard<std::mutex> ctl(impl_->watchCtlMu);
    if (!impl_->isWatching.load()) return;
    impl_->stopFlag.store(true);
    impl_->watcher->wake();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->isWatching.store(false);
    impl_->watcher.reset();
}

bool ConfigStore::watching() const { return impl_->isWatching.load(); }

bool ConfigStore::sealReadOnly() {
    AtomicWriteOptions opts;
    opts.mode = 0444;
    opts.durable = false;
    return writeFileAtomic(impl_->sealPath, "sealed\n", opts);
}

bool ConfigStore::readOnlySealed() const { return impl_->sealed(); }

}  // namespace thimsy
