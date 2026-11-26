// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The GPU pack's environment (linux/package/thimsy-gpu-pack): each card's root has
// /usr/share/thimsy/gpu.json, {"driver": "swrast", "env": {"GALLIUM_DRIVER": "softpipe"}, ...}.
// The session gives "env" to Nano, and so to the games Nano starts (they inherit it), on top of its
// own environment (linux/docs/session.md, "Nano").
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace thimsy {
namespace session {

// gpu.json's "env" as NAME=VALUE entries, in the file's order. A missing "env" (or a missing file,
// given as empty text) is no entries. False (with *err) for text that is not a JSON object, or an
// "env" that is not an object. An entry whose name is not [A-Za-z_][A-Za-z0-9_]* or whose value is
// not a string without NUL is left out and named in *skipped.
bool gpuEnvFromJson(std::string_view text, std::vector<std::string>* env, std::string* err = nullptr,
                    std::vector<std::string>* skipped = nullptr);

}  // namespace session
}  // namespace thimsy
