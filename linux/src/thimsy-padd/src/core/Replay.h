// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Thimsy OS contributors
//
// Replays a trace through the core (PadSpec, Merger, Rumble) and prints what padd would emit.
// Used by tools/padd-replay and the golden tests (tests/traces/*.trace and *.golden).
//
// Trace format, one statement per line; '#' starts a comment line; "double quotes" group words:
//
//   device PATH                         device.json, relative to the trace's directory
//   node LABEL name="..." [phys="..."] [uniq="..."] [bus=N] [vendor=N] [product=N]
//                                       a node as EVIOCGNAME and friends report it
//   caps LABEL keys CODE...             its EV_KEY codes
//   caps LABEL abs CODE MIN MAX [FUZZ FLAT]
//   caps LABEL ff FF_RUMBLE
//   <ms> LABEL TYPE CODE VALUE          an event from a node (names or numbers)
//   <ms> intercept 1|0
//   <ms> sync LABEL [CODE...] [ABS_CODE=VALUE...]   the node's whole state (after SYN_DROPPED)
//   <ms> gone LABEL                     the node went away
//   <ms> ff upload ID STRONG WEAK LENGTH_MS [DELAY_MS]
//   <ms> ff play ID COUNT | ff stop ID | ff erase ID | ff gain GAIN
//
// Header statements come before the first timed one. Then each node is matched against the
// pad's sources (pickSource); a LABEL with no node line that equals a source id is that source.
// Anything else (an external pad) is ignored. Output lines:
//
//   setup ...                           the virtual devices padd would create
//   [<ms>] bind LABEL -> SOURCE | [<ms>] ignore LABEL: why  (with <ms> when first seen in an event)
//   <ms> pad|keys|tap TYPE CODE VALUE   events on the virtual devices or the intercept tap
//   <ms> vib SOURCE on strong=S weak=W for=MS | <ms> vib SOURCE off
//   <ms> intercept on|off
#pragma once

#include <iosfwd>
#include <string>

namespace padd {

// deviceOverride, when not empty, replaces the trace's device line. Returns false with
// *err ("line N: why") on a malformed trace; what was printed up to then stays printed.
bool runReplay(std::istream& trace, const std::string& traceDir, std::ostream& out, std::string* err,
               const std::string& deviceOverride = "");

}  // namespace padd
