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

// Thimsy's <android-base/file.h> for the Linux and host builds of Nano: libbase's file helpers on
// plain POSIX. As in libbase, ReadFileToString and WriteStringToFile open with O_NOFOLLOW unless
// follow_symlinks is true, so a symbolic link as the last path component fails (links earlier in
// the path, like the Linux image's /data, are followed as usual). Every call retries on EINTR.
#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <string>
#include <string_view>

#include <android-base/unique_fd.h>

namespace android {
namespace base {

bool ReadFdToString(borrowed_fd fd, std::string* content);
bool ReadFileToString(const std::string& path, std::string* content, bool follow_symlinks = false);

bool WriteStringToFd(std::string_view content, borrowed_fd fd);
bool WriteStringToFile(const std::string& content, const std::string& path,
                       bool follow_symlinks = false);
// Also sets the mode and owner; on failure the file is removed.
bool WriteStringToFile(const std::string& content, const std::string& path, mode_t mode,
                       uid_t owner, gid_t group, bool follow_symlinks = false);

// Exactly byte_count bytes, or false (errno set; EOF early gives ENODATA).
bool ReadFully(borrowed_fd fd, void* data, size_t byte_count);
bool ReadFullyAtOffset(borrowed_fd fd, void* data, size_t byte_count, off64_t offset);
bool WriteFully(borrowed_fd fd, const void* data, size_t byte_count);

bool RemoveFileIfExists(const std::string& path, std::string* err = nullptr);
bool Readlink(const std::string& path, std::string* result);
bool Realpath(const std::string& path, std::string* result);

std::string GetExecutablePath();
std::string GetExecutableDirectory();

// POSIX basename and dirname, without modifying their argument.
std::string Basename(std::string_view path);
std::string Dirname(std::string_view path);

}  // namespace base
}  // namespace android
