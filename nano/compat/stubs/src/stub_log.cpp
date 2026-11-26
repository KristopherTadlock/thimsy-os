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

// The log line of a stub reached at run time (nanostubs/stub.h).
#include <nanostubs/stub.h>

#include <android/log.h>

extern "C" void nanostub_log(const char* what) {
#ifdef NDEBUG
    const int prio = ANDROID_LOG_DEBUG;
#else
    const int prio = ANDROID_LOG_WARN;
#endif
    __android_log_print(prio, "nanostub", "%s: not available on this platform (inert stub)",
                        what ? what : "?");
}
