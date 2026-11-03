/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_FILE_PLATFORM_H
#define POSTGAMMA_FILE_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Claims are scoped to this module's WasmFS, not to an OPFS origin. */
int postgamma_platform_file_claim(int fd, uint64_t generation, const void *owner);
int postgamma_platform_directory_claim(int fd, uint64_t generation, const void *owner);
int postgamma_platform_file_release(uint64_t generation, const void *owner);
bool postgamma_platform_directory_owned(int fd, uint64_t generation);
/* Returns zero for the same directory, ESTALE for a different object, or errno. */
int postgamma_platform_directory_match(int fd, int directory_fd, const char *path);
#ifdef __cplusplus
}
#endif
#endif
