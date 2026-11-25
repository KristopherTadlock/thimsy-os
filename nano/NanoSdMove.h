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

// Helpers of NanoSdMove.cpp shared with the BIOS installer (NanoBiosInstall.cpp): app settings
// files and where each emulator keeps its folders.
#pragma once

#include "NanoAppFiles.h"   // the settings parsers (sdPrefsGet, sdTrimDir, vita3kPrefPath, azaharDir, ...)

#include <string>
#include <vector>

namespace android {

bool sdReadFile(const std::string& path, std::string* out);             // a regular file, never through a link at its end
bool sdReadPrefs(const std::string& path, std::string* out);            // its backup when there is one (nanobios::appPrefsRead)
bool sdWriteAppFile(const std::string& path, const std::string& text);  // owned like its folder, app label (nanobios::appFileWrite)
std::string ppssppMemstick();                                           // "" = a folder picked through Android
std::string flycastHome(std::string* prefs);                            // "" = not a plain folder
std::string drasticSystemDir();                                         // "" = not a plain folder
// Gives pkg the folder rel on a volume ("primary", or a card's uuid) as if it had been picked through
// Android (NanoNetBridge uri_grant), waiting up to 10 s for the answer: true when granted.
bool sdUriGrant(const char* pkg, const std::string& volume, const std::string& rel);
bool sdUriRevoke(const char* pkg, const std::string& volume, const std::string& rel);   // with the grants under it
// Runs a program without a shell (argv[0] a full path) and waits, killing it after timeoutMs:
// the exit status, or -1.
int runArgv(const std::vector<std::string>& args, std::string* out = nullptr, int timeoutMs = 60000);
bool sdPkgRunning(const std::string& pkg);       // it has a process (pkg, or pkg:name)
void sdStopIfRunning(const std::string& pkg);    // am force-stop, only when it runs
bool emuMoveRunning();   // a move of emulator files, in either Nano

}  // namespace android
