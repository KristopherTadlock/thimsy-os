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

// Inert stub (layer 2, see nanostubs/stub.h) of the NDK's <media/NdkMediaDataSource.h>: no data
// source can be created (AMediaDataSource_new returns null) and the setters do nothing.
// off64_t: C++ (g++ defines _GNU_SOURCE) and Bionic always have it; a C file on glibc needs
// _LARGEFILE64_SOURCE or _GNU_SOURCE before its first include.
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AMediaDataSource;
typedef struct AMediaDataSource AMediaDataSource;

typedef ssize_t (*AMediaDataSourceReadAt)(void* userdata, off64_t offset, void* buffer, size_t size);
typedef ssize_t (*AMediaDataSourceGetSize)(void* userdata);
typedef void (*AMediaDataSourceClose)(void* userdata);

AMediaDataSource* AMediaDataSource_new(void);
void AMediaDataSource_delete(AMediaDataSource* source);
void AMediaDataSource_setUserdata(AMediaDataSource* source, void* userdata);
void AMediaDataSource_setReadAt(AMediaDataSource* source, AMediaDataSourceReadAt readAt);
void AMediaDataSource_setGetSize(AMediaDataSource* source, AMediaDataSourceGetSize getSize);
void AMediaDataSource_setClose(AMediaDataSource* source, AMediaDataSourceClose close);

#ifdef __cplusplus
}
#endif
