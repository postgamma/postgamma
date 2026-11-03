/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

// Generated from manifests/wasm-storage-protocol.json; do not edit.
#ifndef POSTGAMMA_WASM_STORAGE_PROTOCOL_H
#define POSTGAMMA_WASM_STORAGE_PROTOCOL_H

#include <errno.h>

typedef enum PostgammaStorageOperation {
    PGW_STORAGE_MOUNT = 0,
    PGW_STORAGE_LOOKUP = 1,
    PGW_STORAGE_CREATE_FILE = 2,
    PGW_STORAGE_CREATE_DIRECTORY = 3,
    PGW_STORAGE_MOVE = 4,
    PGW_STORAGE_REMOVE = 5,
    PGW_STORAGE_LIST = 6,
    PGW_STORAGE_OPEN = 7,
    PGW_STORAGE_CLOSE = 8,
    PGW_STORAGE_READ = 9,
    PGW_STORAGE_WRITE = 10,
    PGW_STORAGE_FLUSH = 11,
    PGW_STORAGE_SHUTDOWN = 12,
    PGW_STORAGE_FLUSH_DIRECTORY = 13,
    PGW_STORAGE_SIZE = 14,
    PGW_STORAGE_TRUNCATE = 15,
} PostgammaStorageOperation;

#ifdef __EMSCRIPTEN__
#ifdef __cplusplus
static_assert(EACCES == 2, "storage errno differs from target libc");
#else
_Static_assert(EACCES == 2, "storage errno differs from target libc");
#endif
#ifdef __cplusplus
static_assert(EBUSY == 10, "storage errno differs from target libc");
#else
_Static_assert(EBUSY == 10, "storage errno differs from target libc");
#endif
#ifdef __cplusplus
static_assert(EINVAL == 28, "storage errno differs from target libc");
#else
_Static_assert(EINVAL == 28, "storage errno differs from target libc");
#endif
#ifdef __cplusplus
static_assert(EIO == 29, "storage errno differs from target libc");
#else
_Static_assert(EIO == 29, "storage errno differs from target libc");
#endif
#ifdef __cplusplus
static_assert(ENOENT == 44, "storage errno differs from target libc");
#else
_Static_assert(ENOENT == 44, "storage errno differs from target libc");
#endif
#ifdef __cplusplus
static_assert(ENOSPC == 51, "storage errno differs from target libc");
#else
_Static_assert(ENOSPC == 51, "storage errno differs from target libc");
#endif
#ifdef __cplusplus
static_assert(ENOTSUP == 138, "storage errno differs from target libc");
#else
_Static_assert(ENOTSUP == 138, "storage errno differs from target libc");
#endif
#endif

#endif
