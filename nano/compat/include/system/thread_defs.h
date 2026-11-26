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

/*
 * Thimsy's <system/thread_defs.h> for the Linux and host builds of Nano: Android's thread
 * priorities, which are nice values (setpriority), with Android's numbers.
 */
#ifndef NANOCOMPAT_SYSTEM_THREAD_DEFS_H
#define NANOCOMPAT_SYSTEM_THREAD_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ANDROID_PRIORITY_LOWEST = 19,
    ANDROID_PRIORITY_BACKGROUND = 10,
    ANDROID_PRIORITY_NORMAL = 0,
    ANDROID_PRIORITY_FOREGROUND = -2,
    ANDROID_PRIORITY_DISPLAY = -4,
    ANDROID_PRIORITY_URGENT_DISPLAY = -8,
    ANDROID_PRIORITY_VIDEO = -10,
    ANDROID_PRIORITY_AUDIO = -16,
    ANDROID_PRIORITY_URGENT_AUDIO = -19,
    ANDROID_PRIORITY_HIGHEST = -20,
    ANDROID_PRIORITY_DEFAULT = ANDROID_PRIORITY_NORMAL,
    ANDROID_PRIORITY_MORE_FAVORABLE = -1,
    ANDROID_PRIORITY_LESS_FAVORABLE = +1,
};

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_SYSTEM_THREAD_DEFS_H */
