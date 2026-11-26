// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
#include "core/GpuEnv.h"

#include "thimsy/json.h"

namespace thimsy {
namespace session {
namespace {

bool validName(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        bool digit = c >= '0' && c <= '9';
        if (!(alpha || (i > 0 && digit))) return false;
    }
    return true;
}

}  // namespace

bool gpuEnvFromJson(std::string_view text, std::vector<std::string>* env, std::string* err,
                    std::vector<std::string>* skipped) {
    env->clear();
    if (skipped) skipped->clear();
    bool blank = true;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            blank = false;
            break;
        }
    }
    if (blank) return true;
    json::Value doc;
    std::string why;
    if (!json::parse(text, &doc, &why)) {
        if (err) *err = "gpu.json: " + why;
        return false;
    }
    if (!doc.isObject()) {
        if (err) *err = "gpu.json is not an object";
        return false;
    }
    const json::Value* e = doc.find("env");
    if (!e || e->isNull()) return true;
    if (!e->isObject()) {
        if (err) *err = "gpu.json's \"env\" is not an object";
        return false;
    }
    for (const json::Member& m : e->members()) {
        if (!validName(m.key) || !m.value.isString() || m.value.asString().find('\0') != std::string::npos) {
            if (skipped) skipped->push_back(m.key);
            continue;
        }
        env->push_back(m.key + "=" + m.value.asString());
    }
    return true;
}

}  // namespace session
}  // namespace thimsy
