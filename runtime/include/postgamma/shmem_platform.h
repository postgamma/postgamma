/*
 * Copyright 2026 Shujie Zhang
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef POSTGAMMA_SHMEM_PLATFORM_H
#define POSTGAMMA_SHMEM_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Owner is an instance runtime; attacher is a logical execution, never a pthread. */
int postgamma_platform_shmem_create(const void *owner, uint64_t generation,
    const void *attacher, size_t size, size_t alignment, uint32_t *id, void **address);
int postgamma_platform_shmem_attach(const void *owner, uint64_t generation,
    const void *attacher, uint32_t id, void *expected_address);
int postgamma_platform_shmem_detach(const void *owner, uint64_t generation,
    const void *attacher, uint32_t id);
int postgamma_platform_shmem_remove(const void *owner, uint64_t generation, uint32_t id);
bool postgamma_platform_shmem_in_use(uint32_t id);
size_t postgamma_platform_shmem_count(void);
#endif
