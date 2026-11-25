// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// thimsy-prop: the config store from the shell.
//
//   thimsy-prop get KEY [DEFAULT]       print the value (or DEFAULT, or an empty line)
//   thimsy-prop set KEY VALUE           store a value ("" removes it); exit 1 if refused
//   thimsy-prop list [PREFIX]           print key=value lines, sorted; secrets print <redacted>
//   thimsy-prop watch [--once] [--timeout MS] [PATTERN...]
//                                       print key=value for each change (PATTERN: a key, or a
//                                       prefix ending in '.' or '*'); --timeout exits 1 if
//                                       nothing changed in time
//   thimsy-prop boot [--device FILE] [--install FILE] [--build FILE]
//                                       write ro.* from build.props, device.json and install.json,
//                                       then seal ro.* (S01props). Exit 0, 2 when sealed with
//                                       warnings, 1 when it failed or already ran this boot
//
// The store's roots come from THIMSY_PERSIST_FILE and THIMSY_RUN_DIR when set.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "thimsy/atomic_file.h"
#include "thimsy/config.h"
#include "thimsy/json.h"

namespace {

using thimsy::ConfigStore;
namespace json = thimsy::json;

const char kUsage[] =
    "usage: thimsy-prop get KEY [DEFAULT]\n"
    "       thimsy-prop set KEY VALUE\n"
    "       thimsy-prop list [PREFIX]\n"
    "       thimsy-prop watch [--once] [--timeout MS] [PATTERN...]\n"
    "       thimsy-prop boot [--device FILE] [--install FILE] [--build FILE]\n";

int usage() {
    fputs(kUsage, stderr);
    return 64;
}

std::unique_ptr<ConfigStore> openStore() {
    std::string err;
    auto store = ConfigStore::open(ConfigStore::defaultRoots(), &err);
    if (!store) fprintf(stderr, "thimsy-prop: can't open the config store: %s\n", err.c_str());
    return store;
}

void printLine(const std::string& key, const std::string& value) {
    const std::string line = key + "=" + thimsy::escapeValue(thimsy::redactValue(key, value)) + "\n";
    fwrite(line.data(), 1, line.size(), stdout);
}

int cmdGet(int argc, char** argv) {
    if (argc < 1 || argc > 2) return usage();
    auto store = openStore();
    if (!store) return 1;
    const std::string v = store->get(argv[0], argc == 2 ? argv[1] : "");
    printf("%s\n", v.c_str());
    return 0;
}

int cmdSet(int argc, char** argv) {
    if (argc != 2) return usage();
    auto store = openStore();
    if (!store) return 1;
    const auto r = store->trySet(argv[0], argv[1]);
    if (r != ConfigStore::SetResult::Ok) {
        fprintf(stderr, "thimsy-prop: can't set %s: %s\n", argv[0], thimsy::toString(r));
        return 1;
    }
    return 0;
}

int cmdList(int argc, char** argv) {
    if (argc > 1) return usage();
    auto store = openStore();
    if (!store) return 1;
    for (const auto& kv : store->list(argc == 1 ? argv[0] : "")) printLine(kv.first, kv.second);
    return 0;
}

int cmdWatch(int argc, char** argv) {
    bool once = false;
    long timeoutMs = -1;
    std::vector<std::string> patterns;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--once") == 0) {
            once = true;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            char* end = nullptr;
            timeoutMs = strtol(argv[++i], &end, 10);
            if (!end || *end || timeoutMs < 0) return usage();
        } else if (argv[i][0] == '-') {
            return usage();
        } else {
            patterns.emplace_back(argv[i]);
        }
    }
    if (patterns.empty()) patterns.emplace_back("");
    auto store = openStore();
    if (!store) return 1;
    std::mutex mu;
    std::condition_variable cv;
    int seen = 0;
    for (const auto& p : patterns) {
        store->addObserver(p, [&](std::string_view key, std::string_view value) {
            std::lock_guard<std::mutex> lock(mu);
            if (once && seen > 0) return;
            printLine(std::string(key), std::string(value));
            fflush(stdout);
            ++seen;
            cv.notify_all();
        });
    }
    if (!store->startWatching()) {
        fprintf(stderr, "thimsy-prop: can't watch the store (inotify)\n");
        return 1;
    }
    // Ready: a script can start the writer after this line on stderr.
    fprintf(stderr, "thimsy-prop: watching\n");
    std::unique_lock<std::mutex> lock(mu);
    auto done = [&] { return once && seen > 0; };
    if (timeoutMs >= 0) {
        cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), done);
    } else {
        cv.wait(lock, done);
    }
    const int result = seen > 0 ? 0 : 1;
    lock.unlock();
    store->stopWatching();
    return result;
}

// ---- boot --------------------------------------------------------------------------------------

struct Source {
    std::string name;  // for messages
    std::vector<std::pair<std::string, std::string>> props;
};

std::string sanitizeKeyPart(const std::string& s) {
    std::string out;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out;
}

bool scalarText(const json::Value& v, std::string* out) {
    switch (v.type()) {
        case json::Value::Type::String: *out = v.asString(); return true;
        case json::Value::Type::Bool: *out = v.asBool() ? "true" : "false"; return true;
        case json::Value::Type::Number: *out = json::write(v); return true;
        default: return false;
    }
}

void addUnique(Source* src, const std::string& key, const std::string& value) {
    if (value.empty()) return;
    for (const auto& kv : src->props) {
        if (kv.first == key) return;  // within one source, the first (most explicit) wins
    }
    src->props.emplace_back(key, value);
}

bool loadJson(const std::string& path, json::Value* out, bool* missing, int* warnings) {
    std::string text;
    if (!thimsy::readFile(path, &text)) {
        *missing = true;
        return false;
    }
    std::string err;
    if (!json::parse(text, out, &err)) {
        fprintf(stderr, "thimsy-prop boot: %s: %s\n", path.c_str(), err.c_str());
        ++*warnings;
        return false;
    }
    if (!out->isObject()) {
        fprintf(stderr, "thimsy-prop boot: %s: not a JSON object\n", path.c_str());
        ++*warnings;
        return false;
    }
    return true;
}

// device.json: explicit "props" (ro.* only), then the Android match keys from "product" (I24),
// then ro.thimsy.device.<name> for top-level scalars and ro.thimsy.device.panel.<name>.
Source deviceSource(const json::Value& d, const std::string& path, int* warnings) {
    Source src;
    src.name = path;
    for (const auto& m : d["props"].members()) {
        std::string v;
        if (m.key.compare(0, 3, "ro.") != 0 || !thimsy::isValidKey(m.key) || !scalarText(m.value, &v)) {
            fprintf(stderr, "thimsy-prop boot: %s: props.%s ignored (only ro.* keys with scalar values)\n",
                    path.c_str(), m.key.c_str());
            ++*warnings;
            continue;
        }
        addUnique(&src, m.key, v);
    }
    const json::Value& product = d["product"];
    auto productField = [&](const char* name, const char* fallback) {
        std::string v;
        if (scalarText(product[name], &v) && !v.empty()) return v;
        if (fallback && scalarText(d[fallback], &v)) return v;
        return std::string();
    };
    const std::string manufacturer = productField("manufacturer", nullptr);
    const std::string model = productField("model", "model");
    const std::string brand = productField("brand", nullptr);
    const std::string device = productField("device", "id");
    const std::string name = productField("name", "id");
    const std::string platform = productField("platform", "family");
    for (const char* part : {"", "vendor.", "odm."}) {
        const std::string p = std::string("ro.product.") + part;
        addUnique(&src, p + "manufacturer", manufacturer);
        addUnique(&src, p + "model", model);
        addUnique(&src, p + "brand", brand);
        addUnique(&src, p + "device", device);
        addUnique(&src, p + "name", name);
    }
    addUnique(&src, "ro.board.platform", platform);
    for (const auto& m : d.members()) {
        std::string v;
        if (scalarText(m.value, &v)) addUnique(&src, "ro.thimsy.device." + sanitizeKeyPart(m.key), v);
    }
    for (const auto& m : d["panel"].members()) {
        std::string v;
        if (scalarText(m.value, &v)) addUnique(&src, "ro.thimsy.device.panel." + sanitizeKeyPart(m.key), v);
    }
    return src;
}

// install.json is on a user-writable card partition: it may only add ro.thimsy.install.*.
Source installSource(const json::Value& d, const std::string& path) {
    Source src;
    src.name = path;
    for (const auto& m : d.members()) {
        std::string v;
        if (scalarText(m.value, &v)) addUnique(&src, "ro.thimsy.install." + sanitizeKeyPart(m.key), v);
    }
    return src;
}

// build.props: "ro.key=value" lines; '#' comments and blank lines ignored.
Source buildSource(const std::string& text, const std::string& path, int* warnings) {
    Source src;
    src.name = path;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        const size_t eq = line.find('=');
        std::string key = eq == std::string::npos ? "" : line.substr(b, eq - b);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key.compare(0, 3, "ro.") != 0 || !thimsy::isValidKey(key)) {
            fprintf(stderr, "thimsy-prop boot: %s:%d: \"%s\" ignored (not an ro.* key=value line)\n", path.c_str(),
                    lineNo, line.c_str());
            ++*warnings;
            continue;
        }
        // As Android's build.prop loader: whitespace around the value is not part of it.
        std::string value = line.substr(eq + 1);
        const size_t vb = value.find_first_not_of(" \t");
        value = vb == std::string::npos ? "" : value.substr(vb);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        addUnique(&src, key, value);
    }
    return src;
}

const char* envOr(const char* name, const char* def) {
    const char* v = getenv(name);
    return v && *v ? v : def;
}

int cmdBoot(int argc, char** argv) {
    std::string devicePath = envOr("THIMSY_DEVICE_JSON", "/usr/share/thimsy/device.json");
    std::string buildPath = envOr("THIMSY_BUILD_PROPS", "/usr/share/thimsy/build.props");
    // TODO(G2e): the card's thimsy/ directory is assumed to be mounted at /boot; S01props passes
    // --install once the boot partition's mount point is fixed.
    std::string installPath = envOr("THIMSY_INSTALL_JSON", "/boot/thimsy/install.json");
    for (int i = 0; i < argc; ++i) {
        if (i + 1 >= argc) return usage();
        if (strcmp(argv[i], "--device") == 0) devicePath = argv[++i];
        else if (strcmp(argv[i], "--install") == 0) installPath = argv[++i];
        else if (strcmp(argv[i], "--build") == 0) buildPath = argv[++i];
        else return usage();
    }
    auto store = openStore();
    if (!store) return 1;
    if (store->readOnlySealed()) {
        fprintf(stderr, "thimsy-prop boot: ro.* is already sealed for this boot\n");
        return 1;
    }
    int warnings = 0;
    std::vector<Source> sources;  // in precedence order: an earlier source's key wins

    std::string text;
    if (thimsy::readFile(buildPath, &text)) {
        sources.push_back(buildSource(text, buildPath, &warnings));
    } else {
        fprintf(stderr, "thimsy-prop boot: %s: missing\n", buildPath.c_str());
        ++warnings;
    }
    json::Value doc;
    bool missing = false;
    if (loadJson(devicePath, &doc, &missing, &warnings)) {
        sources.push_back(deviceSource(doc, devicePath, &warnings));
    } else if (missing) {
        fprintf(stderr, "thimsy-prop boot: %s: missing\n", devicePath.c_str());
        ++warnings;
    }
    missing = false;
    doc = json::Value();
    if (loadJson(installPath, &doc, &missing, &warnings)) {
        sources.push_back(installSource(doc, installPath));
    }  // a missing install.json is normal: an image written by another tool has none

    std::vector<std::pair<std::string, std::string>> merged;
    std::vector<std::string> origin;
    for (const Source& src : sources) {
        for (const auto& kv : src.props) {
            size_t i = 0;
            while (i < merged.size() && merged[i].first != kv.first) ++i;
            if (i < merged.size()) {
                if (merged[i].second != kv.second) {
                    fprintf(stderr, "thimsy-prop boot: %s from %s ignored: already set by %s\n",
                            kv.first.c_str(), src.name.c_str(), origin[i].c_str());
                    ++warnings;
                }
                continue;
            }
            merged.push_back(kv);
            origin.push_back(src.name);
        }
    }
    bool failed = false;
    for (const auto& kv : merged) {
        const auto r = store->trySet(kv.first, kv.second);
        if (r == ConfigStore::SetResult::Ok) continue;
        fprintf(stderr, "thimsy-prop boot: %s: %s\n", kv.first.c_str(), thimsy::toString(r));
        if (r == ConfigStore::SetResult::IoError) failed = true;
        ++warnings;
    }
    if (!store->sealReadOnly()) {
        fprintf(stderr, "thimsy-prop boot: can't seal ro.*\n");
        return 1;
    }
    if (failed) return 1;
    return warnings ? 2 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "get") return cmdGet(argc - 2, argv + 2);
    if (cmd == "set") return cmdSet(argc - 2, argv + 2);
    if (cmd == "list") return cmdList(argc - 2, argv + 2);
    if (cmd == "watch") return cmdWatch(argc - 2, argv + 2);
    if (cmd == "boot") return cmdBoot(argc - 2, argv + 2);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        fputs(kUsage, stdout);
        return 0;
    }
    return usage();
}
