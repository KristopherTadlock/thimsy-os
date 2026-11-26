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
 * Thimsy's <cutils/properties.h> for the Linux and host builds of Nano: libcutils' property calls
 * on the process-wide config store (nanocompat/nanocompat.h, docs/linux/IMPLEMENTATION.md B.2).
 *
 * Android's behaviour is kept where Nano depends on it:
 *   - property_get copies at most PROPERTY_VALUE_MAX - 1 bytes; an unset or empty key gives the
 *     default (cut to the same length), or "" and 0 without one.
 *   - property_set refuses a value of 92 bytes or more for a key not starting "ro." (returns -1):
 *     Nano splits long lists into continuation keys for this limit. An empty value clears the key.
 *   - property_get_bool: "1", "y", "yes", "on", "true" are true; "0", "n", "no", "off", "false"
 *     are false (exact case); anything else gives the default.
 *   - property_get_int32/int64: strtoimax with base 0 ("0x10" is 16, "010" is 8), leading space
 *     skipped and trailing text ignored ("12abc" is 12, as libcutils does); no digits, overflow or
 *     a value outside the type's range give the default.
 */
#ifndef NANOCOMPAT_CUTILS_PROPERTIES_H
#define NANOCOMPAT_CUTILS_PROPERTIES_H

#include <stddef.h>
#include <stdint.h>
#include <sys/system_properties.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROPERTY_KEY_MAX PROP_NAME_MAX
#define PROPERTY_VALUE_MAX PROP_VALUE_MAX

int property_get(const char* key, char* value, const char* default_value);
int property_set(const char* key, const char* value);
int8_t property_get_bool(const char* key, int8_t default_value);
int64_t property_get_int64(const char* key, int64_t default_value);
int32_t property_get_int32(const char* key, int32_t default_value);
/* Calls propfn(key, value, cookie) for every property that has a value. Returns 0. */
int property_list(void (*propfn)(const char* key, const char* value, void* cookie), void* cookie);

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_CUTILS_PROPERTIES_H */
