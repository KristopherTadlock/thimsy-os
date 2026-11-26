// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// The input event names thimsy-session needs, by their kernel names (device.json writes
// "BTN_MODE", the fake-hardware key FIFOs write "KEY_POWER 1"). Only the codes the session reads
// or that device.json's hotkey section may name are listed.
#pragma once

#include <string>
#include <string_view>

namespace thimsy {
namespace session {

struct InputCode {
    int type = 0;  // EV_KEY or EV_SW
    int code = 0;
};

// "KEY_POWER" -> {EV_KEY, 116}; also "EV_KEY:116"-style numeric forms ("1:116"). False if unknown.
bool inputCodeByName(std::string_view name, InputCode* out);

// The kernel name of a code, or "<type>:<code>" when it is not in the table.
std::string inputCodeName(int type, int code);

}  // namespace session
}  // namespace thimsy
