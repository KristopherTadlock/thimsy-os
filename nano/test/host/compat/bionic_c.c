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
 * bionic_test's C translation unit, compiled as C with nanocompat/bionic.h force-included: the
 * header must stay valid C, and give C the same mallopt constants.
 */
#define _GNU_SOURCE 1 /* after the force-include, as a C file of Nano's might do */

#include <malloc.h>

int bionicC_purgeAll(void) { return M_PURGE_ALL; }
int bionicC_cacheCountMax(void) { return M_CACHE_COUNT_MAX; }
/* An unknown parameter does nothing; the result (0 in glibc, -1 under ASan) is ignored. */
int bionicC_mallopt(void) {
    (void)mallopt(M_PURGE_ALL, 0);
    return 1;
}
