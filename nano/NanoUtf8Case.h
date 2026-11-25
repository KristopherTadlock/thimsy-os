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

// Upper and lower case for UTF-8 text: Latin (with Latin-1, Extended-A, the Pinyin and Vietnamese
// letters), Greek, Cyrillic, Armenian and fullwidth Latin. Other characters pass through unchanged,
// as do malformed bytes.

#pragma once

#include <cstdint>
#include <string>

namespace android {

uint32_t nanoToUpperCp(uint32_t cp);
uint32_t nanoToLowerCp(uint32_t cp);
std::string nanoUtf8Upper(const std::string& s);       // ß becomes SS
std::string nanoUtf8Lower(const std::string& s);
std::string nanoUtf8Capitalize(const std::string& s);  // lower case, each word's first letter upper

}  // namespace android
