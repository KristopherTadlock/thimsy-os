// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Names of the kernel's input event types and codes (linux/input-event-codes.h), both ways.
// The table is generated (tools/gen-input-codes.py); the core does not depend on the header
// for names, only for the numbers it uses by macro.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace padd {

// An event type by name ("EV_KEY") or number ("1", "0x01"); -1 if unknown.
int eventTypeByName(std::string_view name);
// A code of the given event type by name ("BTN_SOUTH", "BTN_A", "ABS_X", "SYN_REPORT") or
// number; -1 if unknown or of another type. KEY_ and BTN_ are both EV_KEY codes.
int eventCodeByName(int type, std::string_view name);
// A KEY_/BTN_ name (EV_KEY), an ABS_ name (EV_ABS); -1 if unknown.
int keyCodeByName(std::string_view name);
int absCodeByName(std::string_view name);

// The canonical name ("BTN_SOUTH" for 0x130), or the number in hex ("0x2ff") when there is none.
std::string eventTypeName(int type);
std::string eventCodeName(int type, int code);

}  // namespace padd
