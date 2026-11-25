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

// The ES-DE system a Nano system is, for the ES-DE theme engine (NanoThemeEngine.cpp). No Android or
// NanoMenu dependencies, so host tests can use it.
#pragma once

#include <string>
#include <vector>

namespace android {
namespace esdesys {

// True when n is an ES-DE system name (es_systems.xml <name>).
bool esdeKnownSystem(const std::string& n);

// The ES-DE full name of an ES-DE system name, "" when unknown.
std::string esdeSystemFullName(const std::string& esdeName);

// The ES-DE system name of a ROM folder (or, without one, a short name): the name when ES-DE knows
// it, else its lowercase, else the ES-DE system a MinUI-style folder name means (PS, MD, GG), else
// the name as it is. Names the gamelists/ and downloaded_media/ folders.
std::string esdeCanonSystemName(const std::string& romDir, const std::string& shortname);

// The ES-DE system a Nano system is: esdeCanonSystemName, or, when that is not an ES-DE system, the
// ES-DE system of Nano's emulator-catalog platform id. A system created from the catalog keeps the
// platform id as its folder ("coleco", "3ds", "tgcd").
std::string esdePlatformSystemName(const std::string& romDir, const std::string& shortname);

// The system a theme draws a Nano system as (${system.theme}): esdePlatformSystemName, or, for a
// catalog platform of games for another console (WiiWare, MSU-MD), that console's.
std::string esdeThemeSystemName(const std::string& romDir, const std::string& shortname);

// One of ES-DE's commands for a system (es_systems.xml <command>).
struct EsdeCommand {
    std::string label;               // "" for a system with one unlabelled command
    std::string command;             // its text, as es_systems.xml has it
    std::vector<std::string> keys;   // core:<name> (RetroArch core), pkg:<package> (each app it starts)
};

// A system's commands in es_systems.xml order (the first is ES-DE's default); empty when unknown.
std::vector<EsdeCommand> esdeSystemCommands(const std::string& esdeName);

// es_systems.xml's <extension>, <platform> and <theme> of a system; false when unknown.
bool esdeSystemDef(const std::string& esdeName, std::string* extension, std::string* platform, std::string* theme);

// The key an emulator is matched by: core:<name> for a RetroArch core (<name>_libretro_android.so),
// else pkg:<package>; "" when it has neither.
std::string esdeEmulatorKey(bool libretro, const std::string& coreSo, const std::string& package);

// The label of the system's command that runs key: of the commands starting it, the one that names
// it first (an app several commands can start goes to the one made for it); "" when none does, or
// when several do alike (one app with a command per media type).
std::string esdeLabelForKey(const std::string& esdeName, const std::string& key);

// The label of ES-DE's default command for the system (its first).
std::string esdeDefaultLabel(const std::string& esdeName);

// The keys of the system's command with that label; empty when it has none.
std::vector<std::string> esdeKeysForLabel(const std::string& esdeName, const std::string& label);

}  // namespace esdesys
}  // namespace android
