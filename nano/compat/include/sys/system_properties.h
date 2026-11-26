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
 * Thimsy's <sys/system_properties.h> for the Linux and host builds of Nano: Bionic's property API
 * on the process-wide config store (nanocompat/nanocompat.h, docs/linux/IMPLEMENTATION.md B.2, B.3).
 *
 * Differences from Bionic that Nano can't see:
 *   - A prop_info is a handle to a key name, made on the first find of a key that has a value (or
 *     on a set of it) and kept for the life of the process.
 *   - Serials are per process. A key's serial changes on every set this process makes (even of the
 *     same value, as on Android) and on every change of its value this process sees: another
 *     process's write is seen while the store watches, or when this process reads the key.
 *     Found keys never have serial 0, so a first wait with old_serial 0 returns at once.
 *   - The global serial (__system_property_wait with a null pi) counts the sets and changes this
 *     process sees, in the same way.
 */
#ifndef NANOCOMPAT_SYS_SYSTEM_PROPERTIES_H
#define NANOCOMPAT_SYS_SYSTEM_PROPERTIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct prop_info prop_info;

#define PROP_VALUE_MAX 92
/* Deprecated on Android too: names have no length limit there (Thimsy: 255 bytes). */
#define PROP_NAME_MAX 32

/* Sets a property. 0 on success, -1 on failure (bad name, a value of PROP_VALUE_MAX bytes or more
 * for a key not starting "ro.", a read-only key already set, or an I/O error). */
int __system_property_set(const char* name, const char* value);

/* The handle of a property that exists, or NULL. */
const prop_info* __system_property_find(const char* name);

/* Copies the value (at most PROP_VALUE_MAX - 1 bytes) and returns its length; "" and 0 when unset. */
int __system_property_get(const char* name, char* value);

/* Calls callback with the name, the full value and the serial of pi. */
void __system_property_read_callback(const prop_info* pi,
                                     void (*callback)(void* cookie, const char* name,
                                                      const char* value, uint32_t serial),
                                     void* cookie);

/* Calls propfn for every property that has a value (in name order). Returns 0. */
int __system_property_foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie);

/* The current serial of pi. */
uint32_t __system_property_serial(const prop_info* pi);

/* The global serial: changes on every set and change this process sees. */
uint32_t __system_property_area_serial(void);

/*
 * Waits until pi's serial (or the global serial, when pi is NULL) differs from old_serial, then
 * stores the new one in *new_serial_ptr and returns true. relative_timeout NULL waits forever;
 * on timeout it returns false.
 */
bool __system_property_wait(const prop_info* pi, uint32_t old_serial, uint32_t* new_serial_ptr,
                            const struct timespec* relative_timeout);

#ifdef __cplusplus
}
#endif

#endif /* NANOCOMPAT_SYS_SYSTEM_PROPERTIES_H */
