// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The config store: Android's system property key names and limits on plain files.
//
//   persist.*  one file (Roots::persistFile, mode 0600): sorted "key=value" lines, values escaped
//              (\n, \\); read-modify-write under flock on "persist.lock" beside it, then an
//              atomic, durable replace.
//   ro.*       <runDir>/ro/<key>, written once at boot (thimsy-prop boot), then sealed: any later
//              set is refused, as on Android.
//   others     <runDir>/<key>, one file per key (tmpfs), replaced atomically, no lock.
//
// As on Android, an empty value is the same as an unset key: set(key, "") removes it, and get()
// returns the default for it. Values are limited to 91 bytes (PROP_VALUE_MAX - 1) except ro.*.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace thimsy {

class Watcher;

// Android's legal property names: [A-Za-z0-9_.:@-], not starting or ending with '.', no "..".
// Thimsy also limits the length to 255 bytes, since a name is a file name in the run dir.
bool isValidKey(std::string_view key);

// The longest value Android stores for a key that is not ro.* (PROP_VALUE_MAX - 1).
constexpr size_t kMaxValueLength = 91;

// Keys whose values must not be printed: their lower-cased name contains "pw", "pass", "token",
// "secret", "ssdev", "psk", "apikey" or "credential". Errs on the side of redacting.
bool isSecretKey(std::string_view key);

// "<redacted>" for a secret key with a non-empty value, else the value itself.
std::string redactValue(std::string_view key, std::string_view value);

// The persist file's line escaping (\n and \\; \r too, for safety), and its inverse.
std::string escapeValue(std::string_view value);
std::string unescapeValue(std::string_view text);

class ConfigStore {
 public:
    struct Roots {
        std::string persistFile;  // e.g. /storage/.config/thimsy/persist.props
        std::string runDir;       // e.g. /run/thimsy/props
    };
    // The roots on a device; THIMSY_PERSIST_FILE and THIMSY_RUN_DIR override them (tests, tools).
    static Roots defaultRoots();

    enum class SetResult {
        Ok,
        InvalidKey,
        ValueTooLong,   // over kMaxValueLength, for a key that is not ro.*
        InvalidValue,   // contains a NUL byte
        ReadOnly,       // ro.* after sealing, or an ro.* key that already has a value
        IoError,
    };

    // Creates the directories it needs and loads the current values. nullptr if a root is empty
    // or a directory can't be created.
    static std::unique_ptr<ConfigStore> open(const Roots& roots, std::string* err = nullptr);
    ~ConfigStore();
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    // The value, or def when the key is unset or empty. Any thread. While watching, this reads the
    // cache the watcher keeps current; otherwise it reads the key's file (persist: when the file
    // changed), so a short-lived process always sees other processes' writes.
    std::string get(std::string_view key, std::string_view def = {}) const;
    bool has(std::string_view key) const { return !get(key).empty(); }

    // Stores a value (Android's rules, see SetResult). Any thread. Observers run after the value is
    // stored and the store's lock is released, on the calling thread.
    bool set(std::string_view key, std::string_view value) { return trySet(key, value) == SetResult::Ok; }
    SetResult trySet(std::string_view key, std::string_view value);

    // Every key with a value, sorted, optionally only those starting with prefix.
    std::map<std::string, std::string> list(std::string_view prefix = {}) const;

    // A per-process counter for the key that bumps on every change this process sees (its own sets
    // and, while watching, other processes' writes). 0 for a key never seen set.
    uint32_t serial(std::string_view key) const;
    // Waits until serial(key) != oldSerial. timeoutMs < 0 waits forever. Returns true on a change,
    // false on timeout. Without the watcher it polls the key's file every 20 ms.
    bool waitChange(std::string_view key, uint32_t oldSerial, int timeoutMs) const;

    // Observers see every change after it is stored; they can't veto it. keyOrPrefix is a key, a
    // prefix ending in '.' or '*' (the '*' is dropped), or empty for every key. The value is empty
    // when the key was removed. Returns an id for removeObserver.
    using Observer = std::function<void(std::string_view key, std::string_view value)>;
    int addObserver(std::string_view keyOrPrefix, Observer observer);
    void removeObserver(int id);

    // One thread watches the run dir, its ro/ dir and the persist file's dir, keeping the cache and
    // serials current and calling observers for other processes' writes. The overload takes a
    // watcher (the test seam). Returns false if the watcher can't be created or a dir can't be added.
    bool startWatching();
    bool startWatching(std::unique_ptr<Watcher> watcher);
    void stopWatching();
    bool watching() const;

    // ro.*: seals the read-only keys (thimsy-prop boot, after writing them). Persists in the run
    // dir (<runDir>/ro/.sealed), so it holds for every process until the next boot.
    bool sealReadOnly();
    bool readOnlySealed() const;

    const Roots& roots() const;

 private:
    struct Impl;
    explicit ConfigStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

const char* toString(ConfigStore::SetResult r);

}  // namespace thimsy
