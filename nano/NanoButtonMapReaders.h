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

// The button map's per-emulator readers (button_map_overlay_plan.md §3): each reads the emulator's
// own binding files under ReaderEnv.root, exactly as the emulator would resolve them at its shipped
// version, and returns console button -> the inputs that press it. Files are apps' own (untrusted):
// every read is bounded, no link is followed, and malformed text gives defaults or "not mapped".
#pragma once

#include <string>
#include <vector>

#include "NanoButtonMap.h"

namespace android {
namespace bmap {

struct EmuBind {
    std::string id;            // anchor id ("btn-a", "dpad-up", "stick-l:up"), or "" for text rows
    std::string name;          // the console's name for it when not the art's ("Turbo B", "C-Up")
    std::vector<Input> in;     // any of these presses it; empty = unbound
    std::vector<std::vector<Input>> chords;   // or these held together ("L1 + A")
    std::string note;
};
struct EmuHotkey {
    std::string action;        // English
    std::vector<Input> chord;  // all held, the last one pressed
};
struct ReadResult {
    std::string console;       // system id of the console drawn ("" = keep the context's)
    std::string drawing;       // a drawing override ("" = the system's)
    std::string emulatorName;  // "RetroArch Snes9x", "PPSSPP"
    std::vector<EmuBind> binds;
    std::vector<EmuHotkey> hotkeys;
    Source source = Source::kUnknown;
    std::string notMappedWhere;
    std::vector<std::string> notes;
};

ReadResult readRetroArch(const ReaderEnv& env, const Context& ctx, const Catalog& cat);
ReadResult readPpsspp(const ReaderEnv& env, const Context& ctx);
ReadResult readDolphin(const ReaderEnv& env, const Context& ctx);
ReadResult readAzahar(const ReaderEnv& env, const Context& ctx);
ReadResult readFlycast(const ReaderEnv& env, const Context& ctx);
ReadResult readArmsx2(const ReaderEnv& env, const Context& ctx);
ReadResult readVita3k(const ReaderEnv& env, const Context& ctx);
ReadResult readWatermelonDs(const ReaderEnv& env, const Context& ctx);
ReadResult readDrasticNano(const ReaderEnv& env, const Context& ctx);
ReadResult readDrastic(const ReaderEnv& env, const Context& ctx);
ReadResult readM64PlusFz(const ReaderEnv& env, const Context& ctx);
ReadResult readScummvm(const ReaderEnv& env, const Context& ctx);
ReadResult readSeedlessDs(const ReaderEnv& env, const Context& ctx);
ReadResult readHakuX(const ReaderEnv& env, const Context& ctx);
// The reader for ctx.emulator; source kUnknown with a note for an emulator it does not know.
ReadResult readEmulator(const ReaderEnv& env, const Context& ctx, const Catalog& cat);

// ---- shared pieces, exposed for the tests ----

// A device path read under root: bounded (maxBytes), regular file, no link followed in the app's
// folder. /sdcard and /storage/emulated/0 are read at /data/media/0, /data/user/0 at /data/data.
bool readAppFile(const ReaderEnv& env, const std::string& devicePath, std::string* out,
                 size_t maxBytes = 1u << 20);
// devicePath with the links Move Emulator Files to microSD leaves on its way (Dolphin's folders)
// replaced by the card folders they point to; "" when a link points anywhere but
// <card>/emulator_config.
std::string throughMoveLinks(const ReaderEnv& env, const std::string& devicePath);
// readAppFile at throughMoveLinks(devicePath).
bool readMovedAppFile(const ReaderEnv& env, const std::string& devicePath, std::string* out,
                      size_t maxBytes = 1u << 20);
// RetroArch-style "key = \"value\"" text: the value of the last line naming key ("" absent).
std::string cfgValue(const std::string& text, const std::string& key, bool* found = nullptr);
// An INI section's key ("" absent); section "" is before the first header.
std::string iniValue(const std::string& text, const std::string& section, const std::string& key,
                     bool* found = nullptr);
bool iniHasSection(const std::string& text, const std::string& section);
// A PSP disc's id ("ULUS10041") from an ISO's UMD_DATA.BIN or a PBP's PARAM.SFO; "" otherwise.
std::string pspDiscId(const ReaderEnv& env, const std::string& devicePath);

}  // namespace bmap
}  // namespace android
