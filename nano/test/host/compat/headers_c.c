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
 * bionic_test's second C translation unit: every shim header that Android also offers to C, each
 * compiled as C, so a C file of Nano's (or a vendored C library) can include them.
 */
#include <android/bitmap.h>
#include <android/imagedecoder.h>
#include <android/log.h>
#include <android/rect.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>
#include <sys/system_properties.h>
#include <system/thread_defs.h>
#include <utils/Timers.h>

#define LOG_TAG_C "HeadersC"

int headersC_values(void) {
    ARect r = {0, 0, 1, 1};
    nsecs_t t = ms2ns(1);
    return (r.right == 1) && (t == 1000000) && (PROPERTY_VALUE_MAX == 92) && (AID_MEDIA_RW == 1023) &&
           (ANDROID_PRIORITY_DISPLAY == -4) && (ANDROID_IMAGE_DECODER_SUCCESS == 0) &&
           (ANDROID_BITMAP_FORMAT_RGBA_8888 == 1) && (ANDROID_LOG_ERROR == 6);
}

int headersC_log(void) {
    ALOGI_IF(0, "never %d", 1);
    return __android_log_is_loggable(ANDROID_LOG_FATAL, LOG_TAG_C, ANDROID_LOG_INFO);
}
